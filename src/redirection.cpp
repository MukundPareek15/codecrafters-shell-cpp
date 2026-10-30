

/*
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

enum class QuoteMode { Normal, Single, Double };

enum class TokenType {
    Word,
    RedirectOut,
    RedirectAppend,
    RedirectError,
    RedirectErrorAppend,
    RedirectInput,
    RedirectErrorToOut
};

struct Token {
    TokenType type;
    std::string value;
};

enum class RedirectionMode { Truncate, Append, Read, Duplicate };

struct Redirection {
    int targetFd = -1;
    RedirectionMode mode = RedirectionMode::Truncate;
    std::string filename;
    int sourceFd = -1;
};

struct ParsedCommand {
    std::vector<std::string> arguments;
    std::vector<Redirection> redirections;
};

// Separate shell operators from words without interpreting quoted characters as operators.
std::vector<Token> tokenize(const std::string& input) {
    std::vector<Token> tokens;
    std::string current;
    QuoteMode mode = QuoteMode::Normal;
    bool wordStarted = false;

    auto finishWord = [&]() {
        if (wordStarted) {
            tokens.push_back({TokenType::Word, current});
            current.clear();
            wordStarted = false;
        }
    };

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];

        if (mode == QuoteMode::Single) {
            if (c == '\'') {
                mode = QuoteMode::Normal;
            } else {
                current += c;
            }
            continue;
        }

        if (mode == QuoteMode::Double) {
            if (c == '"') {
                mode = QuoteMode::Normal;
            } else if (c == '\\') {
                if (i + 1 == input.size()) {
                    throw std::runtime_error("incomplete escape");
                }
                const char next = input[i + 1];
                if (next == '$' || next == '`' || next == '"' || next == '\\') {
                    current += next;
                    ++i;
                } else if (next == '\n') {
                    ++i;
                } else {
                    current += '\\';
                }
            } else {
                current += c;
            }
            continue;
        }

        // Normal mode.
        if (std::isspace(static_cast<unsigned char>(c))) {
            finishWord();
        } else if (c == '\'') {
            wordStarted = true;
            mode = QuoteMode::Single;
        } else if (c == '"') {
            wordStarted = true;
            mode = QuoteMode::Double;
        } else if (c == '\\') {
            if (i + 1 == input.size()) {
                throw std::runtime_error("incomplete escape");
            }
            const char next = input[++i];
            if (next != '\n') {
                current += next;
                wordStarted = true;
            }
        } else if (!wordStarted && (c == '1' || c == '2') &&
                   i + 1 < input.size() && input[i + 1] == '>') {
            if (c == '2' && input.compare(i, 4, "2>&1") == 0) {
                tokens.push_back({TokenType::RedirectErrorToOut, "2>&1"});
                i += 3;
            } else if (i + 2 < input.size() && input[i + 2] == '>') {
                tokens.push_back({c == '1' ? TokenType::RedirectAppend
                                           : TokenType::RedirectErrorAppend,
                                  std::string(1, c) + ">>"});
                i += 2;
            } else {
                tokens.push_back({c == '1' ? TokenType::RedirectOut
                                           : TokenType::RedirectError,
                                  std::string(1, c) + ">"});
                ++i;
            }
        } else if (c == '>' || c == '<') {
            finishWord();
            if (c == '>' && i + 1 < input.size() && input[i + 1] == '>') {
                tokens.push_back({TokenType::RedirectAppend, ">>"});
                ++i;
            } else if (c == '>') {
                tokens.push_back({TokenType::RedirectOut, ">"});
            } else {
                tokens.push_back({TokenType::RedirectInput, "<"});
            }
        } else {
            current += c;
            wordStarted = true;
        }
    }

    if (mode != QuoteMode::Normal) {
        throw std::runtime_error("unclosed quote");
    }
    finishWord();
    return tokens;
}

ParsedCommand parseCommand(const std::vector<Token>& tokens) {
    ParsedCommand parsed;

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        if (token.type == TokenType::Word) {
            parsed.arguments.push_back(token.value);
            continue;
        }

        Redirection redirection;
        if (token.type == TokenType::RedirectErrorToOut) {
            redirection.targetFd = STDERR_FILENO;
            redirection.mode = RedirectionMode::Duplicate;
            redirection.sourceFd = STDOUT_FILENO;
        } else {
            if (i + 1 >= tokens.size() || tokens[i + 1].type != TokenType::Word) {
                throw std::runtime_error("expected filename after " + token.value);
            }
            redirection.filename = tokens[++i].value;

            switch (token.type) {
                case TokenType::RedirectOut:
                    redirection.targetFd = STDOUT_FILENO;
                    redirection.mode = RedirectionMode::Truncate;
                    break;
                case TokenType::RedirectAppend:
                    redirection.targetFd = STDOUT_FILENO;
                    redirection.mode = RedirectionMode::Append;
                    break;
                case TokenType::RedirectError:
                    redirection.targetFd = STDERR_FILENO;
                    redirection.mode = RedirectionMode::Truncate;
                    break;
                case TokenType::RedirectErrorAppend:
                    redirection.targetFd = STDERR_FILENO;
                    redirection.mode = RedirectionMode::Append;
                    break;
                case TokenType::RedirectInput:
                    redirection.targetFd = STDIN_FILENO;
                    redirection.mode = RedirectionMode::Read;
                    break;
                default:
                    throw std::runtime_error("unsupported redirection");
            }
        }
        parsed.redirections.push_back(std::move(redirection));
    }

    return parsed;
}

bool isBuiltin(const std::string& name) {
    return name == "exit" || name == "echo" || name == "pwd" ||
           name == "cd" || name == "type";
}

std::string findExecutable(const std::string& command) {
    namespace fs = std::filesystem;

    auto executable = [](const fs::path& path) {
        std::error_code error;
        return fs::is_regular_file(path, error) &&
               access(path.c_str(), X_OK) == 0;
    };

    if (command.find('/') != std::string::npos) {
        return executable(command) ? command : "";
    }

    const char* pathVariable = std::getenv("PATH");
    if (!pathVariable) {
        return "";
    }

    const std::string pathText(pathVariable);
    std::size_t start = 0;
    while (true) {
        const std::size_t end = pathText.find(':', start);
        const std::string directory = pathText.substr(start, end - start);
        const fs::path candidate = fs::path(directory.empty() ? "." : directory) / command;
        if (executable(candidate)) {
            return candidate.string();
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return "";
}

bool applyRedirections(const std::vector<Redirection>& redirections) {
    for (const Redirection& redirection : redirections) {
        if (redirection.mode == RedirectionMode::Duplicate) {
            if (dup2(redirection.sourceFd, redirection.targetFd) == -1) {
                perror("dup2");
                return false;
            }
            continue;
        }

        int flags = O_RDONLY;
        switch (redirection.mode) {
            case RedirectionMode::Truncate:
                flags = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case RedirectionMode::Append:
                flags = O_WRONLY | O_CREAT | O_APPEND;
                break;
            case RedirectionMode::Read:
                flags = O_RDONLY;
                break;
            default:
                return false;
        }

        const int fd = open(redirection.filename.c_str(), flags, 0644);
        if (fd == -1) {
            perror(redirection.filename.c_str());
            return false;
        }
        if (dup2(fd, redirection.targetFd) == -1) {
            perror("dup2");
            if (fd != redirection.targetFd) {
                close(fd);
            }
            return false;
        }
        if (fd != redirection.targetFd) {
            close(fd);
        }
    }
    return true;
}

// Builtins run in the parent; always restore its standard descriptors afterward.
class ScopedRedirections {
public:
    ScopedRedirections() = default;
    ScopedRedirections(const ScopedRedirections&) = delete;
    ScopedRedirections& operator=(const ScopedRedirections&) = delete;

    bool apply(const std::vector<Redirection>& redirections) {
        if (redirections.empty()) {
            return true;
        }

        std::cout.flush();
        std::cerr.flush();
        for (int fd = 0; fd < 3; ++fd) {
            saved_[fd] = dup(fd);
            if (saved_[fd] == -1) {
                perror("dup");
                discardSaved();
                return false;
            }
        }
        active_ = true;
        if (!applyRedirections(redirections)) {
            restore();
            return false;
        }
        return true;
    }

    ~ScopedRedirections() { restore(); }

private:
    int saved_[3] = {-1, -1, -1};
    bool active_ = false;

    void discardSaved() {
        for (int& fd : saved_) {
            if (fd != -1) {
                close(fd);
                fd = -1;
            }
        }
    }

    void restore() {
        if (!active_) {
            return;
        }
        std::cout.flush();
        std::cerr.flush();
        for (int fd = 0; fd < 3; ++fd) {
            if (dup2(saved_[fd], fd) == -1) {
                perror("dup2 restore");
            }
        }
        discardSaved();
        active_ = false;
    }
};

// Return a shell exit code for exit. Other builtins leave the loop running.
int executeBuiltin(const std::vector<std::string>& args) {
    const std::string& command = args[0];

    if (command == "echo") {
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (i > 1) {
                std::cout << ' ';
            }
            std::cout << args[i];
        }
        std::cout << '\n';
    } else if (command == "pwd") {
        std::error_code error;
        const auto cwd = std::filesystem::current_path(error);
        if (error) {
            std::cerr << "pwd: " << error.message() << '\n';
        } else {
            std::cout << cwd.string() << '\n';
        }
    } else if (command == "cd") {
        std::filesystem::path destination;
        if (args.size() == 1 || args[1] == "~") {
            const char* home = std::getenv("HOME");
            if (!home) {
                std::cerr << "cd: HOME not set\n";
                return 1;
            }
            destination = home;
        } else {
            destination = args[1];
        }

        std::error_code error;
        std::filesystem::current_path(destination, error);
        if (error) {
            std::cerr << "cd: " << destination.string() << ": " << error.message() << '\n';
            return 1;
        }
    } else if (command == "type") {
        if (args.size() < 2) {
            std::cerr << "type: missing operand\n";
            return 1;
        }
        const std::string& name = args[1];
        if (isBuiltin(name)) {
            std::cout << name << " is a shell builtin\n";
        } else {
            const std::string path = findExecutable(name);
            if (path.empty()) {
                std::cout << name << ": not found\n";
                return 1;
            }
            std::cout << name << " is " << path << '\n';
        }
    }
    return 0;
}

int executeExternal(const ParsedCommand& command) {
    const pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // The child redirects its own descriptors, not the parent's.
        if (!applyRedirections(command.redirections)) {
            _exit(1);
        }

        const std::string path = findExecutable(command.arguments[0]);
        if (path.empty()) {
            std::cerr << command.arguments[0] << ": command not found\n";
            _exit(127);
        }

        // execv needs writable C-string pointers and a final null pointer.
        std::vector<std::string> argumentStorage = command.arguments;
        std::vector<char*> argv;
        argv.reserve(argumentStorage.size() + 1);
        for (std::string& argument : argumentStorage) {
            argv.push_back(argument.data());
        }
        argv.push_back(nullptr);

        execv(path.c_str(), argv.data());
        perror("execv");
        _exit(126);
    }

    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";
        std::string input;
        if (!std::getline(std::cin, input)) {
            break;
        }

        ParsedCommand command;
        try {
            command = parseCommand(tokenize(input));
        } catch (const std::runtime_error& error) {
            std::cerr << "shell: " << error.what() << '\n';
            continue;
        }

        if (command.arguments.empty()) {
            // A bare redirection can still create/truncate a file.
            ScopedRedirections redirects;
            redirects.apply(command.redirections);
            continue;
        }

        if (isBuiltin(command.arguments[0])) {
            ScopedRedirections redirects;
            if (!redirects.apply(command.redirections)) {
                continue;
            }
            if (command.arguments[0] == "exit") {
                return command.arguments.size() > 1
                    ? std::atoi(command.arguments[1].c_str()) : 0;
            }
            executeBuiltin(command.arguments);
        } else {
            executeExternal(command);
        }
    }

    return 0;
}
*/