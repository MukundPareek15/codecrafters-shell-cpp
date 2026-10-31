// CodeCrafters Shell, extended C++17 POSIX implementation.
// Features: quoting, variable expansion, redirections, pipelines, background jobs,
// history and persistence, filename/command/programmable completion.
// Build: g++ -std=c++17 -Wall -Wextra -Wpedantic -O2 app/main.cpp -o shell
// Designed for Unix-like environments, including Linux, macOS, and WSL.
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

enum class QuoteMode { Normal, Single, Double };
enum class TokenKind { Word, Pipe, Background, RedirOut, RedirAppend, RedirErr,
                       RedirErrAppend, RedirIn, ErrToOut };
enum class RedirectMode { Truncate, Append, Read, Dup };
struct Token { TokenKind kind; std::string value; };
struct Redirection {
    int target = 1;
    RedirectMode mode = RedirectMode::Truncate;
    std::string filename;
    int source = -1;
};
struct Command { std::vector<std::string> args; std::vector<Redirection> redirects; };
struct Pipeline { std::vector<Command> commands; bool background = false; };
struct Job {
    int number = 0;
    size_t launchOrder = 0;
    std::vector<pid_t> pids;
    std::vector<bool> finished;
    std::string text;
    bool notified = false;
};
struct ShellState {
    std::unordered_map<std::string, std::string> variables;
    std::map<std::string, std::string> completions;
    std::vector<std::string> history;
    size_t historyFlushed = 0;
    std::vector<Job> jobs;
    size_t nextLaunchOrder = 1;
    bool historyAppendOnExit = false;
    int lastStatus = 0;
    bool shouldExit = false;
    int exitCode = 0;
};

bool nameStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }
bool validName(const std::string& s) {
    if (s.empty() || !nameStart(s[0])) return false;
    return std::all_of(s.begin() + 1, s.end(), nameChar);
}
std::string variableValue(const std::string& name, const ShellState& state) {
    if (name == "?") return std::to_string(state.lastStatus);
    if (name == "$") return std::to_string(static_cast<long>(getpid()));
    if (const auto it = state.variables.find(name); it != state.variables.end()) return it->second;
    if (const char* p = std::getenv(name.c_str())) return p;
    return "";
}

// Expand a parameter starting at $, returning false if it is not a supported parameter.
bool consumeParameter(const std::string& line, size_t& i, const ShellState& state,
                      std::string& expanded) {
    if (i + 1 >= line.size()) return false;
    size_t end = i + 1;
    std::string name;
    if (line[end] == '{') {
        size_t close = line.find('}', end + 1);
        if (close == std::string::npos) throw std::runtime_error("bad substitution");
        name = line.substr(end + 1, close - end - 1);
        if (!validName(name) && name != "?" && name != "$")
            throw std::runtime_error("bad substitution");
        i = close;
    } else if (line[end] == '?' || line[end] == '$') {
        name = line[end]; i = end;
    } else if (nameStart(line[end])) {
        size_t begin = end;
        while (end < line.size() && nameChar(line[end])) ++end;
        name = line.substr(begin, end - begin);
        i = end - 1;
    } else return false;
    expanded = variableValue(name, state);
    return true;
}

// Lex commands and operators while applying quote-aware variable substitution.
std::vector<Token> lex(const std::string& line, const ShellState& state) {
    std::vector<Token> tokens;
    std::string current;
    QuoteMode mode = QuoteMode::Normal;
    bool started = false;
    auto flush = [&]() {
        if (started) { tokens.push_back({TokenKind::Word, current}); current.clear(); started = false; }
    };
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (mode == QuoteMode::Single) {
            if (c == '\'') mode = QuoteMode::Normal;
            else current += c;
            continue;
        }
        if (mode == QuoteMode::Double) {
            if (c == '"') { mode = QuoteMode::Normal; continue; }
            if (c == '\\' && i + 1 < line.size()) {
                char n = line[i + 1];
                if (n == '$' || n == '`' || n == '"' || n == '\\') { current += n; ++i; continue; }
                if (n == '\n') { ++i; continue; }
            }
            if (c == '$') {
                std::string expansion;
                if (consumeParameter(line, i, state, expansion)) { current += expansion; continue; }
            }
            current += c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { flush(); continue; }
        if (c == '\'') { mode = QuoteMode::Single; started = true; continue; }
        if (c == '"') { mode = QuoteMode::Double; started = true; continue; }
        if (c == '\\') {
            if (i + 1 >= line.size()) throw std::runtime_error("incomplete escape");
            char n = line[++i];
            if (n != '\n') { current += n; started = true; }
            continue;
        }
        if (c == '$') {
            std::string expansion;
            if (consumeParameter(line, i, state, expansion)) {
                // In this challenge-oriented subset, unquoted expansions stay in one word.
                // A standalone unset variable does not create an empty argument.
                if (!expansion.empty()) { current += expansion; started = true; }
                continue;
            }
        }
        if (!started && (c == '1' || c == '2') && i + 1 < line.size() && line[i + 1] == '>') {
            if (c == '2' && line.compare(i, 4, "2>&1") == 0) {
                tokens.push_back({TokenKind::ErrToOut, "2>&1"}); i += 3;
            } else if (i + 2 < line.size() && line[i + 2] == '>') {
                tokens.push_back({c == '2' ? TokenKind::RedirErrAppend : TokenKind::RedirAppend,
                                  std::string(1, c) + ">>"}); i += 2;
            } else {
                tokens.push_back({c == '2' ? TokenKind::RedirErr : TokenKind::RedirOut,
                                  std::string(1, c) + ">"}); ++i;
            }
            continue;
        }
        if (c == '>' || c == '<' || c == '|' || c == '&') {
            flush();
            if (c == '>') {
                if (i + 1 < line.size() && line[i + 1] == '>') {
                    tokens.push_back({TokenKind::RedirAppend, ">>"}); ++i;
                } else tokens.push_back({TokenKind::RedirOut, ">"});
            } else if (c == '<') tokens.push_back({TokenKind::RedirIn, "<"});
            else if (c == '|') tokens.push_back({TokenKind::Pipe, "|"});
            else tokens.push_back({TokenKind::Background, "&"});
            continue;
        }
        current += c; started = true;
    }
    if (mode != QuoteMode::Normal) throw std::runtime_error("unclosed quote");
    flush();
    return tokens;
}

Pipeline parsePipeline(const std::vector<Token>& tokens) {
    Pipeline pipe;
    pipe.commands.push_back({});
    for (size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        Command& cmd = pipe.commands.back();
        if (t.kind == TokenKind::Word) { cmd.args.push_back(t.value); continue; }
        if (t.kind == TokenKind::Pipe) {
            if (cmd.args.empty()) throw std::runtime_error("missing command before pipe");
            pipe.commands.push_back({}); continue;
        }
        if (t.kind == TokenKind::Background) {
            if (i != tokens.size() - 1) throw std::runtime_error("unexpected '&'");
            pipe.background = true; continue;
        }
        Redirection r;
        if (t.kind == TokenKind::ErrToOut) {
            r.target = STDERR_FILENO; r.mode = RedirectMode::Dup; r.source = STDOUT_FILENO;
        } else {
            if (i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::Word)
                throw std::runtime_error("expected filename after " + t.value);
            r.filename = tokens[++i].value;
            switch (t.kind) {
                case TokenKind::RedirOut: r.target = 1; r.mode = RedirectMode::Truncate; break;
                case TokenKind::RedirAppend: r.target = 1; r.mode = RedirectMode::Append; break;
                case TokenKind::RedirErr: r.target = 2; r.mode = RedirectMode::Truncate; break;
                case TokenKind::RedirErrAppend: r.target = 2; r.mode = RedirectMode::Append; break;
                case TokenKind::RedirIn: r.target = 0; r.mode = RedirectMode::Read; break;
                default: throw std::runtime_error("unsupported redirection");
            }
        }
        cmd.redirects.push_back(std::move(r));
    }
    if (pipe.commands.size() > 1 && pipe.commands.back().args.empty())
        throw std::runtime_error("missing command after pipe");
    return pipe;
}

bool applyRedirections(const std::vector<Redirection>& redirs) {
    for (const auto& r : redirs) {
        if (r.mode == RedirectMode::Dup) {
            if (dup2(r.source, r.target) < 0) { perror("dup2"); return false; }
            continue;
        }
        int flags = (r.mode == RedirectMode::Read) ? O_RDONLY :
                    O_WRONLY | O_CREAT | (r.mode == RedirectMode::Append ? O_APPEND : O_TRUNC);
        int fd = open(r.filename.c_str(), flags, 0644);
        if (fd < 0) { perror(r.filename.c_str()); return false; }
        if (dup2(fd, r.target) < 0) { perror("dup2"); if (fd != r.target) close(fd); return false; }
        if (fd != r.target) close(fd);
    }
    return true;
}
class ScopedRedirections {
    int saved_[3] = {-1,-1,-1};
    bool active_ = false;
public:
    ScopedRedirections() = default;
    ScopedRedirections(const ScopedRedirections&) = delete;
    ScopedRedirections& operator=(const ScopedRedirections&) = delete;
    bool apply(const std::vector<Redirection>& redirs) {
        if (redirs.empty()) return true;
        std::cout.flush(); std::cerr.flush();
        active_ = true;
        for (int i = 0; i != 3; ++i) {
            saved_[i] = dup(i);
            if (saved_[i] < 0) { perror("dup"); restore(); return false; }
        }
        if (!applyRedirections(redirs)) { restore(); return false; }
        return true;
    }
    void restore() {
        if (!active_) return;
        std::cout.flush(); std::cerr.flush();
        for (int i = 0; i != 3; ++i) {
            if (saved_[i] >= 0) {
                if (dup2(saved_[i], i) < 0) perror("restore dup2");
                close(saved_[i]); saved_[i] = -1;
            }
        }
        active_ = false;
    }
    ~ScopedRedirections() { restore(); }
};

bool isBuiltin(const std::string& name) {
    return name == "exit" || name == "echo" || name == "pwd" || name == "cd" ||
           name == "type" || name == "complete" || name == "jobs" || name == "history" ||
           name == "declare" || name == "shopt" || name == "export";
}
std::vector<std::string> builtinNames() {
    return {"cd", "complete", "declare", "echo", "exit", "export", "history", "jobs", "pwd", "shopt", "type"};
}
std::string findExecutable(const std::string& name) {
    auto test = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_regular_file(p, ec) && access(p.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) return test(name) ? name : "";
    const char* env = std::getenv("PATH");
    if (!env) return "";
    std::string dirs(env);
    size_t pos = 0;
    while (true) {
        size_t end = dirs.find(':', pos);
        std::string dir = dirs.substr(pos, end == std::string::npos ? end : end-pos);
        fs::path candidate = fs::path(dir.empty() ? "." : dir) / name;
        if (test(candidate)) return candidate.string();
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return "";
}

std::vector<std::string> builtinAndPathCompletions(const std::string& prefix) {
    std::vector<std::string> names;
    for (const auto& b : builtinNames()) if (b.rfind(prefix, 0) == 0) names.push_back(b);
    const char* env = std::getenv("PATH");
    if (env) {
        std::string dirs(env);
        size_t pos = 0;
        while (true) {
            size_t end = dirs.find(':', pos);
            std::string dir = dirs.substr(pos, end == std::string::npos ? end : end-pos);
            std::error_code ec;
            fs::directory_iterator it(dir.empty() ? "." : dir,
                fs::directory_options::skip_permission_denied, ec), last;
            while (!ec && it != last) {
                std::string base = it->path().filename().string();
                if (base.rfind(prefix, 0) == 0) {
                    std::error_code fileec;
                    if (fs::is_regular_file(it->path(), fileec) && access(it->path().c_str(), X_OK) == 0)
                        names.push_back(base);
                }
                it.increment(ec);
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
std::string commonPrefix(const std::vector<std::string>& items) {
    if (items.empty()) return "";
    size_t n = items[0].size();
    for (size_t i = 1; i < items.size(); ++i) {
        size_t j = 0;
        while (j < n && j < items[i].size() && items[0][j] == items[i][j]) ++j;
        n = j;
    }
    return items[0].substr(0, n);
}

// History persistence: 'history -a' only appends entries not previously appended.
std::string historyFile(const std::vector<std::string>& args, size_t index) {
    if (args.size() > index) return args[index];
    const char* env = std::getenv("HISTFILE");
    return env ? std::string(env) : "";
}
bool readHistory(ShellState& state, const std::string& file) {
    std::ifstream stream(file);
    if (!stream) { std::cerr << "history: " << file << ": cannot read file\n"; return false; }
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        state.history.push_back(line);
    }
    return true;
}
bool writeHistory(ShellState& state, const std::string& file, bool append) {
    std::ofstream stream(file, append ? std::ios::app : std::ios::trunc);
    if (!stream) { std::cerr << "history: " << file << ": cannot write file\n"; return false; }
    size_t first = append ? state.historyFlushed : 0;
    for (size_t i = first; i < state.history.size(); ++i) stream << state.history[i] << '\n';
    stream.flush();
    if (!stream) { std::cerr << "history: " << file << ": write failed\n"; return false; }
    state.historyFlushed = state.history.size();
    return true;
}
void initialHistory(ShellState& state) {
    const char* path = std::getenv("HISTFILE");
    if (path && *path) {
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            state.history.push_back(line);
        }
        state.historyFlushed = state.history.size();
    }
}
void saveHistoryOnExit(ShellState& state) {
    const char* path = std::getenv("HISTFILE");
    if (path && *path) writeHistory(state, path, state.historyAppendOnExit);
}

bool jobFinished(const Job& job) {
    return std::all_of(job.finished.begin(), job.finished.end(), [](bool v) { return v; });
}
void updateJobStatus(ShellState& state) {
    for (auto& job : state.jobs) {
        for (size_t i = 0; i < job.pids.size(); ++i) {
            if (job.finished[i]) continue;
            int status = 0;
            pid_t res;
            do { res = waitpid(job.pids[i], &status, WNOHANG); }
            while (res < 0 && errno == EINTR);
            if (res == job.pids[i] || (res < 0 && errno == ECHILD)) job.finished[i] = true;
        }
    }
}
std::string jobMarker(const ShellState& state, int jobNumber) {
    // '+' marks the newest job, even when its numeric ID was recycled.
    size_t newest = 0, previous = 0;
    int newestId = -1, previousId = -1;
    for (const auto& job : state.jobs) {
        if (job.launchOrder > newest) {
            previous = newest; previousId = newestId;
            newest = job.launchOrder; newestId = job.number;
        } else if (job.launchOrder > previous) {
            previous = job.launchOrder; previousId = job.number;
        }
    }
    if (jobNumber == newestId) return "+";
    if (jobNumber == previousId) return "-";
    return " ";
}
int nextJobNumber(const ShellState& state) {
    for (int n = 1;; ++n) {
        bool used = false;
        for (const auto& j : state.jobs) if (j.number == n) used = true;
        if (!used) return n;
    }
}
void announceFinishedJobs(ShellState& state) {
    updateJobStatus(state);
    for (auto& job : state.jobs) {
        if (jobFinished(job) && !job.notified) {
            std::cout << '[' << job.number << ']' << jobMarker(state, job.number)
                      << "  Done                    " << job.text << '\n';
            job.notified = true;
        }
    }
    state.jobs.erase(std::remove_if(state.jobs.begin(), state.jobs.end(),
        [](const Job& j) { return j.notified; }), state.jobs.end());
}

std::string shellQuoted(const std::string& s) {
    std::string result = "'";
    for (char c : s) result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}
std::string declareQuoted(const std::string& s) {
    std::string value = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\' || c == '$' || c == '`') value += '\\';
        value += c;
    }
    return value + '"';
}

// Return status. Running an exit builtin signals the caller through ShellState.
int runBuiltin(const std::vector<std::string>& args, ShellState& state) {
    if (args.empty()) return 0;
    const std::string& cmd = args[0];
    if (cmd == "exit") {
        state.shouldExit = true;
        state.exitCode = args.size() > 1 ? std::atoi(args[1].c_str()) : state.lastStatus;
        return state.exitCode;
    }
    if (cmd == "echo") {
        for (size_t i = 1; i < args.size(); ++i) {
            if (i > 1) std::cout << ' ';
            std::cout << args[i];
        }
        std::cout << '\n'; return 0;
    }
    if (cmd == "pwd") {
        std::error_code ec;
        auto cwd = fs::current_path(ec);
        if (ec) { std::cerr << "pwd: " << ec.message() << '\n'; return 1; }
        std::cout << cwd.string() << '\n'; return 0;
    }
    if (cmd == "cd") {
        std::string path;
        if (args.size() == 1 || args[1] == "~") {
            const char* home = std::getenv("HOME");
            if (!home) { std::cerr << "cd: HOME not set\n"; return 1; }
            path = home;
        } else path = args[1];
        std::error_code ec;
        fs::current_path(path, ec);
        if (ec) { std::cerr << "cd: " << path << ": " << ec.message() << '\n'; return 1; }
        return 0;
    }
    if (cmd == "type") {
        if (args.size() < 2) { std::cerr << "type: missing operand\n"; return 1; }
        int status = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            if (isBuiltin(args[i])) std::cout << args[i] << " is a shell builtin\n";
            else {
                std::string found = findExecutable(args[i]);
                if (!found.empty()) std::cout << args[i] << " is " << found << '\n';
                else { std::cout << args[i] << ": not found\n"; status = 1; }
            }
        }
        return status;
    }
    if (cmd == "complete") {
        if (args.size() >= 2 && args[1] == "-C") {
            if (args.size() < 4) { std::cerr << "complete: usage: complete -C command name\n"; return 2; }
            for (size_t i = 3; i < args.size(); ++i) state.completions[args[i]] = args[2];
            return 0;
        }
        if (args.size() >= 2 && args[1] == "-r") {
            if (args.size() < 3) { state.completions.clear(); return 0; }
            int result = 0;
            for (size_t i = 2; i < args.size(); ++i)
                if (!state.completions.erase(args[i])) result = 1;
            return result;
        }
        if (args.size() == 1 || args[1] == "-p") {
            if (args.size() <= 2) {
                for (const auto& kv : state.completions)
                    std::cout << "complete -C " << shellQuoted(kv.second) << ' ' << kv.first << '\n';
                return 0;
            }
            int status = 0;
            for (size_t i = 2; i < args.size(); ++i) {
                auto it = state.completions.find(args[i]);
                if (it == state.completions.end()) {
                    std::cerr << "complete: " << args[i] << ": no completion specification\n";
                    status = 1;
                } else std::cout << "complete -C " << shellQuoted(it->second) << ' ' << it->first << '\n';
            }
            return status;
        }
        std::cerr << "complete: unsupported option\n"; return 2;
    }
    if (cmd == "jobs") {
        updateJobStatus(state);
        int specific = -1;
        if (args.size() > 1) {
            std::string n = args[1];
            if (!n.empty() && n[0] == '%') n.erase(0, 1);
            if (n.empty() || !std::all_of(n.begin(), n.end(), [](char c){return std::isdigit(static_cast<unsigned char>(c));})) {
                std::cerr << "jobs: invalid job number\n"; return 1;
            }
            specific = std::atoi(n.c_str());
        }
        bool found = false;
        for (const auto& job : state.jobs) {
            if (specific >= 0 && job.number != specific) continue;
            found = true;
            std::cout << '[' << job.number << ']' << jobMarker(state, job.number)
                      << "  " << (jobFinished(job) ? "Done" : "Running") << "                 " << job.text << '\n';
        }
        if (specific >= 0 && !found) { std::cerr << "jobs: %" << specific << ": no such job\n"; return 1; }
        return 0;
    }
    if (cmd == "history") {
        if (args.size() > 1 && (args[1] == "-r" || args[1] == "-w" || args[1] == "-a")) {
            std::string path = historyFile(args, 2);
            if (path.empty()) { std::cerr << "history: no history file\n"; return 1; }
            if (args[1] == "-r") return readHistory(state, path) ? 0 : 1;
            return writeHistory(state, path, args[1] == "-a") ? 0 : 1;
        }
        size_t count = state.history.size();
        if (args.size() > 1) {
            try { count = static_cast<size_t>(std::stoull(args[1])); }
            catch (...) { std::cerr << "history: numeric argument required\n"; return 1; }
        }
        size_t from = state.history.size() > count ? state.history.size() - count : 0;
        for (size_t i = from; i < state.history.size(); ++i)
            std::cout << std::setw(5) << (i + 1) << "  " << state.history[i] << '\n';
        return 0;
    }
    if (cmd == "declare") {
        if (args.size() == 1) {
            std::map<std::string, std::string> sorted(state.variables.begin(), state.variables.end());
            for (const auto& kv : sorted)
                std::cout << "declare -- " << kv.first << '=' << declareQuoted(kv.second) << '\n';
            return 0;
        }
        if (args[1] == "-p") {
            if (args.size() == 2) {
                std::map<std::string, std::string> sorted(state.variables.begin(), state.variables.end());
                for (const auto& kv : sorted)
                    std::cout << "declare -- " << kv.first << '=' << declareQuoted(kv.second) << '\n';
                return 0;
            }
            int status = 0;
            for (size_t i = 2; i < args.size(); ++i) {
                auto it = state.variables.find(args[i]);
                if (it != state.variables.end())
                    std::cout << "declare -- " << it->first << '=' << declareQuoted(it->second) << '\n';
                else { std::cerr << "bash: declare: " << args[i] << ": not found\n"; status = 1; }
            }
            return status;
        }
        int status = 0;
        for (size_t i = 1; i < args.size(); ++i) {
            size_t eq = args[i].find('=');
            std::string name = args[i].substr(0, eq);
            if (!validName(name)) {
                std::cerr << "bash: declare: `" << args[i] << "': not a valid identifier\n";
                status = 1;
                continue;
            }
            if (eq != std::string::npos) state.variables[name] = args[i].substr(eq + 1);
            else if (state.variables.find(name) == state.variables.end()) state.variables[name] = "";
        }
        return status;
    }
    if (cmd == "shopt") {
        if (args.size() >= 3 && (args[1] == "-s" || args[1] == "-u") && args[2] == "histappend") {
            state.historyAppendOnExit = args[1] == "-s"; return 0;
        }
        if (args.size() >= 3 && args[1] == "-q" && args[2] == "histappend")
            return state.historyAppendOnExit ? 0 : 1;
        std::cerr << "shopt: unsupported option\n"; return 1;
    }
    if (cmd == "export") {
        for (size_t i = 1; i < args.size(); ++i) {
            size_t eq = args[i].find('=');
            std::string name = args[i].substr(0, eq);
            if (!validName(name)) { std::cerr << "export: invalid identifier\n"; return 1; }
            std::string value = eq == std::string::npos ? variableValue(name, state) : args[i].substr(eq + 1);
            state.variables[name] = value;
            if (setenv(name.c_str(), value.c_str(), 1) < 0) { perror("setenv"); return 1; }
        }
        return 0;
    }
    return 1;
}

struct CompletionContext {
    bool isCommand = true;
    std::string commandName;
    std::string previousWord;
    std::string prefix;
    size_t rawStart = 0;
    QuoteMode quoteMode = QuoteMode::Normal;
};
struct Candidate { std::string text; bool appendSpace = true; };

// Completion cannot use lex() because incomplete quoted words are normal during editing.
CompletionContext completionContext(const std::string& line) {
    CompletionContext ctx;
    std::vector<std::string> words;
    std::string current;
    bool started = false;
    QuoteMode mode = QuoteMode::Normal;
    size_t start = line.size();
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (mode == QuoteMode::Single) {
            if (c == '\'') mode = QuoteMode::Normal;
            else current += c;
            continue;
        }
        if (mode == QuoteMode::Double) {
            if (c == '"') mode = QuoteMode::Normal;
            else if (c == '\\' && i + 1 < line.size()) {
                char n = line[i+1];
                if (n == '$' || n == '`' || n == '"' || n == '\\') { current += n; ++i; }
                else current += c;
            } else current += c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (started) { words.push_back(current); current.clear(); started = false; }
            start = i + 1;
        } else {
            if (!started) { started = true; start = i; }
            if (c == '\'') mode = QuoteMode::Single;
            else if (c == '"') mode = QuoteMode::Double;
            else if (c == '\\' && i + 1 < line.size()) current += line[++i];
            else current += c;
        }
    }
    ctx.isCommand = words.empty();
    ctx.commandName = words.empty() ? (started ? current : "") : words[0];
    ctx.previousWord = words.empty() ? "" : words.back();
    ctx.prefix = current;
    ctx.rawStart = started ? start : line.size();
    ctx.quoteMode = mode;
    return ctx;
}
std::string escapeSuffix(const std::string& suffix, QuoteMode mode) {
    std::string result;
    for (char c : suffix) {
        if (mode == QuoteMode::Single) {
            if (c == '\'') result += "'\\''";
            else result += c;
        } else if (mode == QuoteMode::Double) {
            if (c == '$' || c == '"' || c == '\\' || c == '`') result += '\\';
            result += c;
        } else {
            if (std::isspace(static_cast<unsigned char>(c)) ||
                std::string("\\'\"<>|&;$`*?[]()").find(c) != std::string::npos)
                result += '\\';
            result += c;
        }
    }
    return result;
}
std::vector<Candidate> filenameCompletions(const std::string& prefix, bool dirsOnly) {
    size_t slash = prefix.find_last_of('/');
    std::string directory = slash == std::string::npos ? "." : prefix.substr(0, slash + 1);
    std::string partial = slash == std::string::npos ? prefix : prefix.substr(slash + 1);
    std::string typedDir = slash == std::string::npos ? "" : prefix.substr(0, slash + 1);
    std::vector<Candidate> result;
    std::error_code ec;
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::string name = it->path().filename().string();
        bool hidden = !name.empty() && name[0] == '.';
        if (name.rfind(partial, 0) == 0 && (!hidden || (!partial.empty() && partial[0] == '.'))) {
            std::error_code typeErr;
            bool isDir = it->is_directory(typeErr);
            if (!typeErr && (isDir || !dirsOnly))
                result.push_back({typedDir + name + (isDir ? "/" : ""), !isDir});
        }
        it.increment(ec);
    }
    std::sort(result.begin(), result.end(), [](auto& a, auto& b){return a.text < b.text;});
    return result;
}

// Runs a registered -C completer, with output captured via a pipe.
std::vector<std::string> runCompleter(const std::string& specification,
                                      const CompletionContext& ctx,
                                      const std::string& line,
                                      const ShellState& state) {
    std::vector<std::string> names;
    std::vector<Token> tokens;
    try { tokens = lex(specification, state); }
    catch (...) { return names; }
    std::vector<std::string> args;
    for (const auto& t : tokens) {
        if (t.kind != TokenKind::Word) return names;
        args.push_back(t.value);
    }
    if (args.empty()) return names;
    args.push_back(ctx.commandName);
    args.push_back(ctx.prefix);
    args.push_back(ctx.previousWord);
    int fds[2];
    if (pipe(fds) < 0) return names;
    std::cout.flush(); std::cerr.flush();
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return names; }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(126);
        close(fds[1]);
        setenv("COMP_LINE", line.c_str(), 1);
        std::string point = std::to_string(line.size());
        setenv("COMP_POINT", point.c_str(), 1);
        setenv("COMP_TYPE", "9", 1);
        setenv("COMP_KEY", "9", 1);
        std::string resolved = findExecutable(args[0]);
        if (resolved.empty()) _exit(127);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(resolved.c_str(), argv.data());
        _exit(126);
    }
    close(fds[1]);
    std::string output;
    char buffer[4096];
    while (true) {
        ssize_t count = read(fds[0], buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) { if (errno == EINTR) continue; break; }
        if (output.size() + static_cast<size_t>(count) > 1024 * 1024) break;
        output.append(buffer, static_cast<size_t>(count));
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {}
    std::istringstream reader(output);
    std::string name;
    while (std::getline(reader, name)) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (!name.empty()) names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
std::vector<Candidate> collectCompletions(const std::string& line,
                                          const CompletionContext& ctx,
                                          const ShellState& state) {
    std::vector<Candidate> choices;
    if (ctx.isCommand) {
        for (const auto& name : builtinAndPathCompletions(ctx.prefix))
            choices.push_back({name, true});
    } else if (const auto it = state.completions.find(ctx.commandName);
               it != state.completions.end()) {
        for (const auto& text : runCompleter(it->second, ctx, line, state))
            choices.push_back({text, true});
    } else {
        // A blank argument lists the current directory (dotfiles stay hidden).
        choices = filenameCompletions(ctx.prefix, ctx.commandName == "cd");
    }
    return choices;
}

class TerminalGuard {
    termios original_{};
    bool active_ = false;
public:
    TerminalGuard() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &original_) < 0) return;
        termios updated = original_;
        updated.c_lflag &= ~(ICANON | ECHO);
        updated.c_cc[VMIN] = 1;
        updated.c_cc[VTIME] = 0;
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &updated) == 0;
    }
    bool active() const { return active_; }
    ~TerminalGuard() { if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &original_); }
};
void redrawLine(const std::string& line) {
    std::cout << "\r\033[2K$ " << line;
}
bool readInputLine(std::string& input, const ShellState& state) {
    TerminalGuard guard;
    if (!guard.active()) return static_cast<bool>(std::getline(std::cin, input));
    input.clear();
    size_t historyIndex = state.history.size();
    std::string draft;
    bool tabAmbiguous = false;
    while (true) {
        char c = 0;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n == 0) return !input.empty();
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (c == '\n' || c == '\r') { std::cout << '\n'; return true; }
        if (c == 4 && input.empty()) { std::cout << '\n'; return false; }
        if (c == 127 || c == '\b') {
            if (!input.empty()) { input.pop_back(); std::cout << "\b \b"; }
            tabAmbiguous = false; continue;
        }
        if (c == '\033') {
            char next = 0, arrow = 0;
            if (read(STDIN_FILENO, &next, 1) != 1 || next != '[' ||
                read(STDIN_FILENO, &arrow, 1) != 1) continue;
            if (arrow == 'A') {
                if (historyIndex == state.history.size()) draft = input;
                if (historyIndex > 0) { --historyIndex; input = state.history[historyIndex]; redrawLine(input); }
            } else if (arrow == 'B') {
                if (historyIndex < state.history.size()) {
                    ++historyIndex;
                    input = historyIndex == state.history.size() ? draft : state.history[historyIndex];
                    redrawLine(input);
                }
            }
            tabAmbiguous = false; continue;
        }
        if (c == '\t') {
            CompletionContext ctx = completionContext(input);
            auto candidates = collectCompletions(input, ctx, state);
            if (candidates.empty()) { std::cout << '\a'; tabAmbiguous = false; continue; }
            if (candidates.size() == 1) {
                const auto& choice = candidates[0];
                if (choice.text.rfind(ctx.prefix, 0) == 0) {
                    std::string suffix = escapeSuffix(choice.text.substr(ctx.prefix.size()), ctx.quoteMode);
                    input += suffix; std::cout << suffix;
                } else {
                    input.erase(ctx.rawStart);
                    input += escapeSuffix(choice.text, QuoteMode::Normal);
                    redrawLine(input);
                }
                if (choice.appendSpace) {
                    if (ctx.quoteMode == QuoteMode::Single) { input += '\''; std::cout << '\''; }
                    if (ctx.quoteMode == QuoteMode::Double) { input += '"'; std::cout << '"'; }
                    input += ' '; std::cout << ' ';
                }
                tabAmbiguous = false;
                continue;
            }
            std::vector<std::string> names;
            for (const auto& cnd : candidates) names.push_back(cnd.text);
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            std::string common = commonPrefix(names);
            if (common.rfind(ctx.prefix, 0) == 0 && common.size() > ctx.prefix.size()) {
                std::string suffix = escapeSuffix(common.substr(ctx.prefix.size()), ctx.quoteMode);
                input += suffix; std::cout << suffix;
                tabAmbiguous = false;
            } else if (!tabAmbiguous) {
                std::cout << '\a'; tabAmbiguous = true;
            } else {
                std::cout << '\n';
                for (size_t i = 0; i < names.size(); ++i) {
                    if (i) std::cout << "  ";
                    std::cout << names[i];
                }
                std::cout << '\n'; redrawLine(input);
                tabAmbiguous = false;
            }
            continue;
        }
        if (static_cast<unsigned char>(c) >= 32 && c != 127) {
            input += c; std::cout.put(c); tabAmbiguous = false;
        }
    }
}

int waitFor(pid_t pid) {
    int status = 0;
    pid_t result;
    do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    if (result < 0) { perror("waitpid"); return 1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

// Execute one pipeline. Single foreground builtins run in the parent, so 'cd' persists.
int executePipeline(const Pipeline& pipeline, const std::string& source, ShellState& state) {
    if (pipeline.commands.empty()) return 0;
    if (pipeline.commands.size() == 1 && !pipeline.background) {
        const auto& cmd = pipeline.commands[0];
        if (cmd.args.empty() || isBuiltin(cmd.args[0])) {
            ScopedRedirections redirs;
            if (!redirs.apply(cmd.redirects)) return 1;
            return cmd.args.empty() ? 0 : runBuiltin(cmd.args, state);
        }
    }
    std::cout.flush(); std::cerr.flush();
    std::vector<pid_t> children;
    int previousRead = -1;
    const size_t count = pipeline.commands.size();
    for (size_t i = 0; i < count; ++i) {
        int fds[2] = {-1,-1};
        if (i + 1 < count && pipe(fds) < 0) {
            perror("pipe");
            if (previousRead != -1) close(previousRead);
            for (pid_t p : children) waitFor(p);
            return 1;
        }
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            if (previousRead != -1) close(previousRead);
            if (fds[0] != -1) close(fds[0]);
            if (fds[1] != -1) close(fds[1]);
            for (pid_t p : children) waitFor(p);
            return 1;
        }
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (previousRead != -1) {
                if (dup2(previousRead, STDIN_FILENO) < 0) _exit(1);
            }
            if (fds[1] != -1) {
                if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(1);
            }
            if (previousRead != -1) close(previousRead);
            if (fds[0] != -1) close(fds[0]);
            if (fds[1] != -1) close(fds[1]);
            const Command& command = pipeline.commands[i];
            if (!applyRedirections(command.redirects)) _exit(1);
            if (command.args.empty()) _exit(0);
            if (isBuiltin(command.args[0])) {
                int status = runBuiltin(command.args, state);
                std::cout.flush(); std::cerr.flush();
                _exit(status);
            }
            std::string path = findExecutable(command.args[0]);
            if (path.empty()) {
                std::cerr << command.args[0] << ": command not found\n";
                std::cerr.flush(); _exit(127);
            }
            std::vector<std::string> storage = command.args;
            std::vector<char*> argv;
            argv.reserve(storage.size() + 1);
            for (auto& arg : storage) argv.push_back(arg.data());
            argv.push_back(nullptr);
            execv(path.c_str(), argv.data());
            perror("execv"); _exit(126);
        }
        children.push_back(pid);
        if (previousRead != -1) close(previousRead);
        if (fds[1] != -1) close(fds[1]);
        previousRead = fds[0];
    }
    if (previousRead != -1) close(previousRead);
    if (pipeline.background) {
        Job job;
        job.number = nextJobNumber(state);
        job.launchOrder = state.nextLaunchOrder++;
        job.pids = children;
        job.finished.assign(children.size(), false);
        job.text = source;
        state.jobs.push_back(std::move(job));
        std::cout << '[' << state.jobs.back().number << "] " << children.back() << '\n';
        return 0;
    }
    int result = 0;
    for (size_t i = 0; i < children.size(); ++i) {
        int code = waitFor(children[i]);
        if (i + 1 == children.size()) result = code;
    }
    return result;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    ShellState state;
    initialHistory(state);
    while (!state.shouldExit) {
        announceFinishedJobs(state);
        std::cout << "$ ";
        std::string line;
        if (!readInputLine(line, state)) break;
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        state.history.push_back(line);
        try {
            Pipeline pipeline = parsePipeline(lex(line, state));
            state.lastStatus = executePipeline(pipeline, line, state);
        } catch (const std::exception& e) {
            std::cerr << "shell: " << e.what() << '\n';
            state.lastStatus = 2;
        }
    }
    saveHistoryOnExit(state);
    return state.shouldExit ? state.exitCode : state.lastStatus;
}
