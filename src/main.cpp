// =============================================================================
// CodeCrafters Shell implementation (C++, POSIX only).
// =============================================================================
// Builtins: exit, echo, pwd, cd, type.
// Features: quoting, PATH lookup, external execution, redirection,
// command completion, file/directory completion, multiple and partial matches.
// Character-by-character editing is supported when stdin is a terminal.
//
// HOW A COMMAND FLOWS THROUGH THIS FILE
//   main()                      REPL loop: print "$ ", read, run, repeat.
//    └─ readInputLine()         Reads one line (raw terminal mode + TAB completion).
//    └─ tokenize()              Text -> Tokens (words and redirection operators).
//    └─ parseCommand()          Tokens -> ParsedCommand (argv + redirection list).
//    └─ executeBuiltin()        Runs echo/pwd/cd/type inside this process, or
//       executeExternal()       fork() + execv() a program found on PATH.
//
// POINTS TO NOTE
//  * The header comment used to say C++17; CMakeLists.txt actually builds with
//    CMAKE_CXX_STANDARD 23. Nothing here needs more than C++17 (std::filesystem).
//  * This uses POSIX-only headers (<termios.h>, <unistd.h>, <sys/wait.h>), so it
//    builds on Linux/macOS/WSL, not natively on Windows with MSVC.
//  * Not supported (yet): pipes '|', variables '$X', globbing '*', '&&', ';',
//    background jobs '&', and the '$?' last-exit-status variable.
// =============================================================================

#include <cerrno>        // errno, EINTR
#include <cctype>        // std::isspace
#include <cstdio>        // perror
#include <cstdlib>       // std::getenv, std::atoi
#include <filesystem>    // std::filesystem::path, directory_iterator, current_path
#include <fcntl.h>       // open() and its O_* flags
#include <iostream>      // std::cout, std::cerr, std::cin
#include <stdexcept>     // std::runtime_error (used for parse errors)
#include <algorithm>     // std::sort, std::unique
#include <string>
#include <termios.h>     // tcgetattr/tcsetattr: switch the terminal to raw-ish mode
#include <sys/wait.h>    // waitpid and the WIFEXITED/WEXITSTATUS macros
#include <unistd.h>      // fork, execv, dup, dup2, close, read, access, isatty
#include <vector>
#include <system_error>  // std::error_code (non-throwing filesystem calls)
#include <utility>       // std::move

// =============================================================================
// Core data types
// =============================================================================

// Which kind of quote the scanner is currently inside. Quoting changes how
// every following character is interpreted until the matching quote closes.
//   Normal : spaces split words, '\' escapes the next char, > and < are operators.
//   Single : everything is literal until the next '  (no escapes at all).
//   Double : literal except  \$  \`  \"  \\  and backslash-newline.
enum class QuoteMode { Normal, Single, Double };

// What the tokenizer can produce. Everything that is not an operator is a Word.
//   RedirectOut          >   or 1>    stdout -> file (truncate)
//   RedirectAppend       >>  or 1>>   stdout -> file (append)
//   RedirectError        2>           stderr -> file (truncate)
//   RedirectErrorAppend  2>>          stderr -> file (append)
//   RedirectInput        <            file  -> stdin
//   RedirectErrorToOut   2>&1         stderr -> wherever stdout points right now
enum class TokenType {
    Word,
    RedirectOut,
    RedirectAppend,
    RedirectError,
    RedirectErrorAppend,
    RedirectInput,
    RedirectErrorToOut
};

// How a redirection opens its target:
//   Truncate  -> O_TRUNC  (empty the file first)
//   Append    -> O_APPEND (every write goes to the end of the file)
//   Read      -> O_RDONLY (for '<')
//   Duplicate -> no file; copy one descriptor onto another (for 2>&1)
enum class RedirectionMode { Truncate, Append, Read, Duplicate };

// One lexical unit. `value` is the *decoded* text: quotes and escapes have
// already been removed, so  'a b'  becomes the single word  a b.
struct Token {
    TokenType type;
    std::string value;
};

// Snapshot of the half-typed line at the moment TAB is pressed.
struct CompletionContext
{
    bool isCommand = true;     // true while typing the first word (the command name)
    std::string prefix;        // Current argument, with quoting/escaping decoded
    std::string commandName;   // First completed word, when completing an argument
    QuoteMode quoteMode = QuoteMode::Normal;  // Is the word inside an open quote?
};

// One possible completion shown to / inserted for the user.
struct CompletionCandidate
{
    std::string text;          // Full decoded word, e.g. "src/main.cpp" or "echo"
    bool appendSpace;          // true for "finished" words; false for directories,
                               // so the user can keep typing after the '/'
};

// A filesystem entry that matched during path completion.
struct FilenameMatch
{
    std::string replacement;   // Directory part + name (+ '/' if it's a directory)
    bool isDirectory;
};

// A partially typed path split at the last '/':
//   "src/ma" -> directory "src/", directoryPrefix "src/", filenamePrefix "ma"
//   "ma"     -> directory ".",    directoryPrefix "",     filenamePrefix "ma"
struct PathCompletionContext
{
    std::filesystem::path directory;   // Where to list entries
    std::string directoryPrefix;       // Text to keep in front of each match
    std::string filenamePrefix;        // What each entry name must start with
};

// One redirection, ready to be applied with open()/dup2().
//   targetFd : the descriptor being replaced (0 = stdin, 1 = stdout, 2 = stderr)
//   filename : file to open (unused for Duplicate)
//   sourceFd : descriptor to copy from (only used for Duplicate, i.e. 2>&1)
struct Redirection {
    int targetFd = -1;
    RedirectionMode mode = RedirectionMode::Truncate;
    std::string filename;
    int sourceFd = -1;
};

// The result of parsing one input line.
// `arguments` is argv: arguments[0] is the command name.
// `redirections` keeps the order they were written in. Order matters:
//   cmd > f 2>&1   sends both streams to f
//   cmd 2>&1 > f   sends stderr to the terminal, only stdout to f
struct ParsedCommand {
    std::vector<std::string> arguments;
    std::vector<Redirection> redirections;
};

// =============================================================================
// Terminal handling
// =============================================================================

// RAII guard that puts the terminal into non-canonical, no-echo mode and puts
// the original settings back when the guard goes out of scope.
//
// Why: By default the terminal driver is in "canonical" (cooked) mode. It
// buffers a whole line, handles Backspace by itself, and only hands the program
// the text after Enter. To react to TAB immediately and draw our own echo, we
// have to receive every keystroke as it's typed.
//
// Points to note:
//  * If stdin is not a terminal (piped input, tests, `./shell < script`),
//    nothing is changed and active() returns false.
//  * Only ICANON and ECHO are turned off. ISIG stays on, so Ctrl+C still sends
//    SIGINT. Because the shell installs no handler, Ctrl+C kills the shell, and
//    the destructor does NOT run on death by signal. The terminal can then be
//    left without echo; type `reset` or `stty sane` blindly to fix it.
//  * In non-canonical mode the special keys are no longer interpreted:
//    Ctrl+D arrives as byte 4 and Backspace usually as byte 127 (DEL).
//    readInputLine() handles both by hand.
class TerminalModeGuard
{
public:
    TerminalModeGuard()
    {
        if (!isatty(STDIN_FILENO))
            return;

        // Save the current settings so the destructor can put them back.
        if (tcgetattr(STDIN_FILENO, &original_) == -1)
            return;

        termios settings = original_;

        //Disable canonical mode and echoing
        // c_lflag holds the "local" flags. Clear the two bits and leave the rest alone.
        settings.c_lflag &= ~(ICANON | ECHO);

        //Return after one character
        // VMIN = 1, VTIME = 0: read() blocks until at least 1 byte is available,
        // with no timeout.
        settings.c_cc[VMIN] = 1;
        settings.c_cc[VTIME] = 0;

        // TCSANOW applies the change immediately (not after output drains).
        active_ = tcsetattr(STDIN_FILENO, TCSANOW, &settings) == 0;

    }

    ~TerminalModeGuard()
    {
        if (active_)
        {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_);
        }
    }

    bool active () const { return active_; }

private:
    termios original_{};   // Settings in effect before we touched anything
    bool active_ = false;  // true only if we actually changed the terminal
};

// =============================================================================
// Tab completion helpers
// =============================================================================

// Completion operates on an unfinished input line. Unlike tokenize(), this scanner
// permits unclosed quotes. It finds the active word and decodes quoted spaces.
//
// Example: input = `cat "my fi`
//   completedWords = {"cat"}, prefix = "my fi", quoteMode = Double
//   -> isCommand = false, commandName = "cat"
//
// Point to note: this scanner does not know about > or <, so in `echo hi>ou`
// the active word is "hi>ou". With spaces (`echo hi > ou`) it works as expected.
CompletionContext getCompletionContext(const std::string& input)
{
    std::vector<std::string> completedWords;  // Words already closed by whitespace
    std::string current;                      // The word under the cursor (decoded)
    QuoteMode mode = QuoteMode::Normal;
    bool wordStarted = false;  // Needed because "" is a real (empty) word

    for (size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];

        // Inside '...': every character is literal until the closing quote.
        if (mode == QuoteMode::Single)
        {
            if (c == '\'')
                mode = QuoteMode::Normal;
            else
                current.push_back(c);
            continue;
        }

        // Inside "...": a backslash escapes only $ ` " \ and newline (POSIX rule).
        // Any other backslash is kept literally, e.g. "a\b" stays a\b.
        if (mode == QuoteMode::Double)
        {
            if (c == '"')
                mode = QuoteMode::Normal;
            else if (c == '\\' && i + 1 < input.size())
            {
                char next = input[i + 1];
                if (next == '$' || next == '`' || next == '"' || next == '\\')
                {
                    current.push_back(next);
                    ++i;
                }
                else if (next == '\n')
                    ++i;          // backslash-newline = line continuation, drop both
                else
                    current.push_back('\\');
            }
            else
                current.push_back(c);
            continue;
        }

        // Normal (unquoted) mode.
        // The cast to unsigned char matters: passing a negative char (any byte
        // >= 0x80, e.g. UTF-8) to std::isspace is undefined behaviour.
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (wordStarted)
            {
                completedWords.push_back(current);
                current.clear();
                wordStarted = false;
            }
        }
        else if (c == '\'' || c == '"')
        {
            // Quotes don't end a word: a"b c"d is the single word "ab cd".
            mode = (c == '\'') ? QuoteMode::Single : QuoteMode::Double;
            wordStarted = true;
        }
        else if (c == '\\' && i + 1 < input.size())
        {
            // Unquoted backslash: the next character is taken literally.
            char next = input[++i];
            if (next != '\n')
            {
                current.push_back(next);
                wordStarted = true;
            }
        }
        else
        {
            // Ordinary character. A trailing lone '\' also lands here and is kept as-is.
            current.push_back(c);
            wordStarted = true;
        }
    }

    // Whatever is still in `current` is the word the cursor is on.
    // If nothing has been completed yet, that word is the command name.
    CompletionContext result;
    result.isCommand = completedWords.empty();
    result.commandName = completedWords.empty() ? "" : completedWords.front();
    result.prefix = current;
    result.quoteMode = mode;
    return result;
}

// Split a partially typed path into "which directory to list" and
// "what the entry name must start with". See PathCompletionContext.
// Works for absolute paths too: "/us" -> directory "/", filenamePrefix "us".
PathCompletionContext splitPathForCompletion(const std::string& fragment)
{
    size_t slash = fragment.find_last_of('/');
    if (slash == std::string::npos)
        return {".", "", fragment};   // No slash: look in the current directory

    // Keep the trailing '/' in the prefix so it can be prepended to each match.
    std::string directoryPrefix = fragment.substr(0, slash + 1);
    return {
        std::filesystem::path(directoryPrefix),
        directoryPrefix,
        fragment.substr(slash + 1)
    };
}

// Make the newly inserted characters valid shell input in the current quote mode.
//
// Completion works on *decoded* text (e.g. the file `my file.txt`), but what we
// insert into the line has to re-parse to the same text, so we re-encode it:
//   Normal : my\ file.txt      (escape spaces and shell metacharacters)
//   Double : "my file.txt      (only " \ $ ` need a backslash)
//   Single : 'my file.txt      (nothing can be escaped inside '...')
std::string escapeCompletionSuffix(const std::string& suffix, QuoteMode mode)
{
    std::string escaped;
    for (char c : suffix)
    {
        if (mode == QuoteMode::Single)
        {
            // A single quote can't appear inside '...', so close the quote,
            // add an escaped quote, then reopen:  'it'\''s'  ->  it's
            if (c == '\'')
                escaped += "'\\''"; // End quote, escape quote, reopen quote.
            else
                escaped.push_back(c);
        }
        else if (mode == QuoteMode::Double)
        {
            if (c == '"' || c == '\\' || c == '$' || c == '`')
                escaped.push_back('\\');
            escaped.push_back(c);
        }
        else
        {
            // Unquoted: escape whitespace and every character a POSIX shell treats
            // as special. Some of these (| & ; * ...) aren't implemented by this
            // shell yet, but escaping them keeps the line correct in a real shell too.
            if (std::isspace(static_cast<unsigned char>(c)) ||
                c == '\\' || c == '\'' || c == '"' || c == '>' ||
                c == '<' || c == '|' || c == '&' || c == ';' ||
                c == '(' || c == ')' || c == '$' || c == '`' ||
                c == '*' || c == '?' || c == '[' || c == ']')
                escaped.push_back('\\');
            escaped.push_back(c);
        }
    }
    return escaped;
}

// List the files/directories that could complete `fragment`, sorted by name.
// With directoriesOnly = true (used for `cd`), regular files are skipped.
std::vector<FilenameMatch> findFilenameCompletions(const std::string& fragment, bool directoriesOnly = false)
{
    namespace fs = std::filesystem;

    std::vector<FilenameMatch> matches;

    PathCompletionContext context = splitPathForCompletion(fragment);

    // The std::error_code overloads don't throw. A missing or unreadable
    // directory just sets `ec`, the loop never starts, and we return no matches
    // instead of crashing.
    std::error_code ec;

    fs::directory_iterator it(context.directory, fs::directory_options::skip_permission_denied, ec);

    // A default-constructed directory_iterator is the "end" sentinel.
    fs::directory_iterator end;

    while (!ec && it != end)
    {
        const fs::directory_entry& entry = *it;

        std::string name = entry.path().filename().string();

        // Check whether the name matches the typed prefix.
        // `rfind(prefix, 0) == 0` is the pre-C++20 idiom for starts_with:
        // it only searches at position 0.
        bool startsWithPrefix = name.rfind(context.filenamePrefix, 0) == 0;

        // Hide dotfiles unless the user starts with a dot.
        bool hidden = !name.empty() && name[0] == '.';

        bool includeHidden = !context.filenamePrefix.empty() && context.filenamePrefix[0] == '.';

        if (startsWithPrefix && (!hidden || includeHidden))
        {
            // is_directory() follows symlinks, so a link to a directory
            // completes with a trailing '/' as well.
            std::error_code typeError;
            bool isDirectory = entry.is_directory(typeError);

            if (!typeError && (!directoriesOnly || isDirectory))
            {
                std::string replacement = context.directoryPrefix + name;

                if (isDirectory)
                    replacement += "/";

                matches.push_back({replacement, isDirectory});
            }
        }

        // increment(ec) is the non-throwing version of ++it.
        it.increment(ec);
    }

    // directory_iterator returns entries in no particular order, so sort them
    // for stable, predictable output.
    std::sort(
        matches.begin(),
        matches.end(),
        [](const FilenameMatch& a, const FilenameMatch& b)
        {
            return a.replacement < b.replacement;
        }
    );

    return matches;
}

// =============================================================================
// Lexing and parsing
// =============================================================================

// Separate shell operators from words without interpreting quoted characters as operators.
//
// It's a small state machine driven by `mode` (QuoteMode). Examples:
//   echo 'a  b' "c"d      -> [Word echo] [Word "a  b"] [Word cd]
//   ls>out.txt            -> [Word ls] [RedirectOut] [Word out.txt]
//   echo hi 2>>err.log    -> [Word echo] [Word hi] [RedirectErrorAppend] [Word err.log]
//   echo '>' x            -> [Word echo] [Word >] [Word x]   (a quoted > is just text)
//
// Throws std::runtime_error on an unclosed quote or a trailing backslash.
// main() catches it and prints "shell: <message>".
std::vector<Token> tokenize(const std::string& input) {
    std::vector<Token> tokens;
    std::string current;
    QuoteMode mode = QuoteMode::Normal;
    // Tracks "a word has begun" separately from !current.empty(), so that
    // an empty quoted string ('' or "") still produces an (empty) argument.
    bool wordStarted = false;

    // Push the word being built, if any. Called on whitespace, before an
    // operator, and at end of input.
    auto finishWord = [&]() {
        if (wordStarted) {
            tokens.push_back({TokenType::Word, current});
            current.clear();
            wordStarted = false;
        }
    };

    for (std::size_t i = 0; i < input.size(); ++i) {
        const char c = input[i];

        // Single quotes: fully literal. Only another ' ends the mode.
        if (mode == QuoteMode::Single) {
            if (c == '\'') {
                mode = QuoteMode::Normal;
            } else {
                current += c;
            }
            continue;
        }

        // Double quotes: literal, except for backslash before $ ` " \ newline.
        if (mode == QuoteMode::Double) {
            if (c == '"') {
                mode = QuoteMode::Normal;
            } else if (c == '\\') {
                if (i + 1 == input.size()) {
                    throw std::runtime_error("incomplete escape");
                }
                const char next = input[i + 1];
                if (next == '$' || next == '`' || next == '"' || next == '\\') {
                    current += next;       // Escaped: keep only the second char
                    ++i;
                } else if (next == '\n') {
                    ++i;                   // Line continuation: drop both
                } else {
                    current += '\\';       // Not special: keep the backslash
                }                          // (`next` is handled next iteration)
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
            // Numbered redirections: 1>, 1>>, 2>, 2>>, 2>&1.
            // The `!wordStarted` check is important. A digit only counts as a file
            // descriptor at the *start* of a word, so `echo a2>f` means
            // word "a2" plus a plain '>' redirect, the same as bash.
            if (c == '2' && input.compare(i, 4, "2>&1") == 0) {
                tokens.push_back({TokenType::RedirectErrorToOut, "2>&1"});
                i += 3;   // Skip ">&1" (the loop's ++i skips the '2')
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
            // An unquoted operator ends the current word even without a space,
            // so `ls>out` splits into "ls", ">", "out".
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

    // Reaching the end of the input while still inside a quote is an error.
    // (Real shells would show a "> " continuation prompt instead.)
    if (mode != QuoteMode::Normal) {
        throw std::runtime_error("unclosed quote");
    }
    finishWord();
    return tokens;
}

// Turn the flat token list into argv plus a list of redirections.
// Every redirection operator except 2>&1 takes the NEXT token as its filename.
// Redirections can appear anywhere: `> out echo hi` is valid, the same as bash.
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
            // 2>&1 means "make fd 2 a copy of fd 1", i.e. dup2(1, 2).
            redirection.targetFd = STDERR_FILENO;
            redirection.mode = RedirectionMode::Duplicate;
            redirection.sourceFd = STDOUT_FILENO;
        } else {
            // e.g. `echo hi >` with nothing after the operator.
            if (i + 1 >= tokens.size() || tokens[i + 1].type != TokenType::Word) {
                throw std::runtime_error("expected filename after " + token.value);
            }
            redirection.filename = tokens[++i].value;   // Consume the filename token

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

// =============================================================================
// Command lookup
// =============================================================================

// Builtins are commands the shell runs itself instead of launching a program.
// `cd` *must* be a builtin: a child process can't change its parent's working
// directory. `exit` must be one for the same kind of reason.
// Point to note: this list also appears in findBuiltinCompletions(). Keep the two in sync.
bool isBuiltin(const std::string& name) {
    return name == "exit" || name == "echo" || name == "pwd" ||
           name == "cd" || name == "type";
}

// Resolve a command name to an executable path, or "" if none is found.
//   * A name containing '/' (./run, /bin/ls, bin/tool) is used as-is, with no PATH search.
//   * Otherwise each directory in $PATH (separated by ':') is tried in order.
//     The first match wins, which is why PATH order matters.
std::string findExecutable(const std::string& command) {
    namespace fs = std::filesystem;

    // "Executable" here means a regular file (not a directory) that we have
    // permission to execute. access(X_OK) checks with the *real* user ID.
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
        // When end == npos, (end - start) is huge, and substr() simply clamps it
        // to "the rest of the string".
        const std::string directory = pathText.substr(start, end - start);
        // An empty PATH entry ("::" or a leading/trailing ':') means the current
        // directory. This is a POSIX rule, and a known security pitfall.
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

// =============================================================================
// Redirection
// =============================================================================

// Apply each redirection to THIS process's file descriptors, in order.
// Returns false (after printing an error) if any of them fails.
//
// The key system call is dup2(oldfd, newfd): it makes `newfd` refer to the same
// open file as `oldfd`, closing whatever `newfd` pointed to before. Anything
// that later writes to fd 1 (std::cout, printf, or an exec'd program) ends up
// in the file.
bool applyRedirections(const std::vector<Redirection>& redirections) {
    for (const Redirection& redirection : redirections) {
        if (redirection.mode == RedirectionMode::Duplicate) {
            // 2>&1: copy whatever fd 1 points to *at this moment* onto fd 2.
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

        // 0644 = rw-r--r--. It is only used when O_CREAT creates a new file,
        // and the process umask is subtracted from it.
        const int fd = open(redirection.filename.c_str(), flags, 0644);
        if (fd == -1) {
            perror(redirection.filename.c_str());   // e.g. "out.txt: Permission denied"
            return false;
        }
        if (dup2(fd, redirection.targetFd) == -1) {
            perror("dup2");
            if (fd != redirection.targetFd) {
                close(fd);
            }
            return false;
        }
        // After dup2, `targetFd` refers to the file, so the temporary `fd` is no
        // longer needed. Close it to avoid leaking descriptors. The `!=` guard
        // covers the rare case where open() returned the target number itself
        // (when that fd was closed). Closing it would undo the redirection.
        if (fd != redirection.targetFd) {
            close(fd);
        }
    }
    return true;
}

// =============================================================================
// Command-name completion
// =============================================================================

// Builtin names that start with `prefix`.
std::vector<std::string> findBuiltinCompletions(const std::string &prefix)
{
    const std::vector<std::string> builtins = {"exit", "echo", "pwd", "cd", "type"};
    std::vector<std::string> matches;

    for (const auto& builtin : builtins)
    {
        if (builtin.rfind(prefix, 0) == 0)   // starts_with(prefix)
        {
            matches.push_back(builtin);
        }
    }

    return matches;
}

// Builtins plus every executable in every $PATH directory whose name starts
// with `prefix`. The result is sorted and de-duplicated.
// Point to note: this scans every PATH directory on every TAB press. That's fine
// for a toy shell. Bash keeps a hash table of commands it has found instead.
std::vector<std::string> findExecutableCompletions(const std::string& prefix)
{
    namespace fs = std::filesystem;

    //Match the builtins
    std::vector<std::string> matches = findBuiltinCompletions(prefix);

    //Read Path
    const char* pathVariable = std::getenv("PATH");

    if (!pathVariable)
    {
        std::sort(matches.begin(), matches.end());

        matches.erase(
            std::unique(matches.begin(), matches.end()),
            matches.end()
        );

        return matches;
    }
    std::string pathText(pathVariable);

    size_t start = 0;


    //Visit each path directory
    while(true)
    {
        size_t end = pathText.find(':', start);

        std::string directory = pathText.substr(start, end == std::string::npos ? std::string::npos : end - start);

        // Empty path components represent current directory
        fs::path folder = directory.empty() ? fs::path(".") : fs::path(directory);

        // Non-throwing: PATH often lists directories that don't exist.
        std::error_code directoryError;

        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, directoryError);

        fs::directory_iterator last;

        while (!directoryError && it != last)
        {
            const fs::directory_entry& entry = *it;

            std::string filename =
                entry.path().filename().string();

            // Does the entry name start with what the user typed?
            bool prefixMatches =
                filename.rfind(prefix, 0) == 0;

            if (prefixMatches)
            {
                std::error_code fileError;

                // Keep it only if it's a regular file we can execute
                // (same rule as findExecutable()).
                if (fs::is_regular_file(entry.path(), fileError) &&
                    access(entry.path().c_str(), X_OK) == 0)
                {
                    matches.push_back(filename);
                }
            }

            it.increment(directoryError);

        }
        if(end == std::string::npos)
            break;

        start = end + 1;

    }

    //Sort Alphabetically
    std::sort(matches.begin(), matches.end());

    //Remove repeated command names
    // The same name can show up more than once: a builtin and /bin/echo, or
    // /bin/ls and /usr/bin/ls on "merged /usr" systems. std::unique only
    // collapses *adjacent* duplicates (that's why we sort first), and it returns
    // the new logical end, which erase() then trims. This is the erase-unique idiom.
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());

    return matches;

}


// Longest string that every element starts with.
//   {"echo", "ecr", "ed"} -> "e"     {"main.cpp", "main.h"} -> "main."
// Used for partial completion: when several names match, insert the shared part.
std::string longestCommonPrefix(const std::vector<std::string>& matches)
{
    if (matches.empty())
        return "";

    const std::string& first = matches[0];

    size_t commonLength = first.size();

    // Compare each matching name against the first.
    // `commonLength` can only shrink, so each comparison is bounded by it.
    for (size_t i = 1; i < matches.size(); ++i)
    {
        size_t j = 0;

        while (j < commonLength && j < matches[i].size() && first[j] == matches[i][j])
        {
            ++j;
        }

        commonLength = j;

        if (commonLength == 0)
            break;          // Nothing in common. No need to look further.
    }

    return first.substr(0, commonLength);
}


// Builtins run in the parent; always restore its standard descriptors afterward.
//
// Why this is needed: an external command is redirected inside its forked
// child, and the parent never notices. A builtin like `echo hi > f` runs in
// the shell process itself. Without restoring fd 1, every later prompt and
// output would also go into f.
//
// How: dup() each of fds 0-2 to a spare descriptor (the backup), apply the
// redirections, and in the destructor dup2() the backups back. This is RAII:
// the restore runs however the scope is left, including the `return` for `exit`
// in main().
class ScopedRedirections {
public:
    ScopedRedirections() = default;
    // Non-copyable: the object owns the backup descriptors. A copy would
    // restore and close the same fds twice.
    ScopedRedirections(const ScopedRedirections&) = delete;
    ScopedRedirections& operator=(const ScopedRedirections&) = delete;

    bool apply(const std::vector<Redirection>& redirections) {
        if (redirections.empty()) {
            return true;   // Nothing to do, so no backups are needed
        }

        // std::cout has its own user-space buffer. Flush it before switching
        // fds so text written *before* the redirection doesn't land in the file.
        std::cout.flush();
        std::cerr.flush();
        for (int fd = 0; fd < 3; ++fd) {
            saved_[fd] = dup(fd);   // Backup copy of stdin/stdout/stderr
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
    int saved_[3] = {-1, -1, -1};   // Backups of fds 0, 1, 2 (-1 = none)
    bool active_ = false;           // true while redirections are in effect

    // Close the backup descriptors without restoring anything.
    void discardSaved() {
        for (int& fd : saved_) {
            if (fd != -1) {
                close(fd);
                fd = -1;
            }
        }
    }

    // Put the original stdin/stdout/stderr back, then close the backups.
    void restore() {
        if (!active_) {
            return;
        }
        // Flush so output from the builtin reaches the redirected file
        // before fd 1 is pointed back at the terminal.
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

// =============================================================================
// Execution
// =============================================================================

// Execute builtins other than exit, which is handled by main().
// Returns a shell-style status: 0 = success, non-zero = failure.
// Point to note: main() currently ignores this value (there's no `$?` yet).
int executeBuiltin(const std::vector<std::string>& args) {
    const std::string& command = args[0];

    if (command == "echo") {
        // Print the arguments separated by single spaces. Quoting was already
        // handled by the tokenizer, so `echo "a   b"` keeps its three spaces.
        // Flags like -n / -e are not supported.
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
        // `cd` and `cd ~` both go to $HOME. Note: `~/dir` is NOT expanded;
        // only a bare "~" is handled here.
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

        // current_path(p) is the C++ wrapper around chdir(2). It changes the
        // working directory of THIS process, which is why cd is a builtin.
        std::error_code error;
        std::filesystem::current_path(destination, error);
        if (error) {
            std::cerr << "cd: " << destination.string() << ": " << error.message() << '\n';
            return 1;
        }
    } else if (command == "type") {
        // Report how a name would be resolved: builtin first, then PATH.
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
                // stdout, not stderr. The CodeCrafters tests expect it there.
                std::cout << name << ": not found\n";
                return 1;
            }
            std::cout << name << " is " << path << '\n';
        }
    }
    return 0;
}

// Run an external program using the classic fork/exec/wait pattern:
//   1. fork()   creates a copy of this process (the child).
//   2. Child:   applies redirections to its own fds, then execv() replaces the
//               child's program image with the target program.
//   3. Parent:  waitpid() blocks until the child finishes.
// Returns the child's exit status, using the shell conventions below.
int executeExternal(const ParsedCommand& command) {
    const pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return 1;
    }

    // fork() returns 0 in the child and the child's PID in the parent.
    if (pid == 0) {
        // The child redirects its own descriptors, not the parent's.
        if (!applyRedirections(command.redirections)) {
            _exit(1);
        }

        // The lookup happens *after* redirection, so "command not found" goes
        // to the redirected stderr, as in bash: `nope 2>/dev/null` stays silent.
        const std::string path = findExecutable(command.arguments[0]);
        if (path.empty()) {
            std::cerr << command.arguments[0] << ": command not found\n";
            _exit(127);   // 127 = "command not found", by shell convention
        }

        // execv needs writable C-string pointers and a final null pointer.
        // The child gets a private copy of the strings to point into.
        // Point to note: argv[0] is the name as typed (e.g. "ls"), not the full
        // path. That's the Unix convention, and some programs check argv[0].
        std::vector<std::string> argumentStorage = command.arguments;
        std::vector<char*> argv;
        argv.reserve(argumentStorage.size() + 1);
        for (std::string& argument : argumentStorage) {
            argv.push_back(argument.data());   // non-const data() since C++17
        }
        argv.push_back(nullptr);

        // On success execv never returns: this process *becomes* the program.
        // The environment is inherited automatically.
        execv(path.c_str(), argv.data());
        // Reaching here means exec failed (e.g. bad binary format).
        perror("execv");
        // _exit, not exit: exit() would run atexit handlers and static
        // destructors and flush stdio buffers copied from the parent, which can
        // print the same output twice. _exit ends the child immediately.
        _exit(126);   // 126 = "found but could not execute", by shell convention
    }

    // Parent: wait for the child. waitpid can be interrupted by a signal
    // (it returns -1 with errno == EINTR). That isn't a real failure, so retry.
    int status = 0;
    pid_t result;
    do {
        result = waitpid(pid, &status, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        perror("waitpid");
        return 1;
    }
    // `status` is packed. Decode it with the W* macros.
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);        // Normal exit: its exit code (0-255)
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);     // Killed by signal N -> 128+N (e.g. 130 for Ctrl+C)
    }
    return 1;
}

// =============================================================================
// Line editor
// =============================================================================

// Read one line of input into `input`. Returns false on end-of-input, which
// tells main() to exit the shell.
//
// Two paths:
//   * Not a terminal (pipes, test harness): a plain std::getline.
//   * Terminal: read one byte at a time in non-canonical mode and do the
//     editing by hand (echo, Backspace, TAB completion, Ctrl+D).
//
// Point to note: the TerminalModeGuard is local, so the terminal is only in
// raw mode while a line is being read. Programs started by the shell (vim,
// python, ...) see a normal terminal.
//
// Limitations (byte-oriented editor):
//   * Arrow keys send escape sequences like ESC [ A. ESC (27) is dropped, but
//     '[' and 'A' are printable, so they get inserted into the line.
//   * Backspace removes one *byte*, so it only removes part of a multi-byte
//     UTF-8 character.
//   * There's no cursor movement or history.
bool readInputLine(std::string& input)
{
    TerminalModeGuard terminal;

    /*Preserve normal behavior for redirected stdin and environments without a usable terminal*/

    if(!terminal.active())
    {
        return static_cast<bool>(std::getline(std::cin, input));
    }

    input.clear();
    // Bash-style double-TAB: the first ambiguous TAB only beeps, and a second
    // TAB in a row lists all the matches. Any other key resets this flag.
    bool previousTabWasAmbiguous = false;

    while(true)
    {
        char c = '\0';

        // Raw read(2) from fd 0, bypassing std::cin's buffer. Thanks to VMIN = 1
        // it returns as soon as a single key is pressed.
        ssize_t bytes = read(STDIN_FILENO, &c, 1);

        if (bytes == 0)
        {
            // EOF on the terminal: submit what we have, or report end-of-input.
            return !input.empty();
        }

        if (bytes == -1)
        {
            // Interrupted by a signal before any byte arrived: try again.
            if(errno == EINTR)
                continue;

            return false;
        }

        // Enter. The ICRNL input flag (still on) normally turns CR into '\n',
        // but both are accepted to be safe.
        if(c=='\n'||c =='\r')
        {
            std::cout << '\n';
            return true;
        }

        // Backspace: most terminals send DEL (127); some send BS (8).
        if(c == 127 || c == '\b')
        {
            previousTabWasAmbiguous = false;
            if(!input.empty())
            {
                input.pop_back();
                // Move the cursor left, overwrite the char with a space, move left again.
                std::cout << "\b \b";
            }
            continue;
        }

        // TAB: command-name completion or filename/directory completion.
        if (c == '\t')
        {
            CompletionContext context = getCompletionContext(input);
            std::vector<CompletionCandidate> candidates;

            // --- Step 1: collect candidates for the word under the cursor ---
            if (context.isCommand)
            {
                if (context.prefix.find('/') == std::string::npos)
                {
                    // Bare command names come from builtins and PATH.
                    for (const auto& name : findExecutableCompletions(context.prefix))
                        candidates.push_back({name, true});
                }
                else
                {
                    // A command containing '/' is a path, not a PATH lookup.
                    // Offer directories (to keep descending) and executable files.
                    for (const auto& match : findFilenameCompletions(context.prefix))
                    {
                        std::error_code ec;
                        bool executable = std::filesystem::is_regular_file(match.replacement, ec) &&
                                          access(match.replacement.c_str(), X_OK) == 0;
                        if (match.isDirectory || executable)
                            candidates.push_back({match.replacement, !match.isDirectory});
                    }
                }
            }
            else
            {
                // An argument: complete file names. `cd` only makes sense with
                // directories, so files are filtered out for it.
                bool directoriesOnly = (context.commandName == "cd");
                for (const auto& match : findFilenameCompletions(context.prefix, directoriesOnly))
                    candidates.push_back({match.replacement, !match.isDirectory});
            }

            // --- Step 2: no match -> ring the terminal bell ('\a', BEL) ---
            if (candidates.empty())
            {
                std::cout << '\a';
                previousTabWasAmbiguous = false;
                continue;
            }

            // --- Step 3: exactly one match -> complete it fully ---
            // One match: insert the remainder, then '/' for directories or
            // a closing quote and space for completed quoted filenames.
            // Only the *missing* part is inserted. Every candidate starts with
            // context.prefix, so substr(prefix.size()) is the new text.
            if (candidates.size() == 1)
            {
                const auto& candidate = candidates.front();
                const std::string suffix = candidate.text.substr(context.prefix.size());
                std::string inserted = escapeCompletionSuffix(suffix, context.quoteMode);

                if (candidate.appendSpace)
                {
                    if (context.quoteMode == QuoteMode::Single)
                        inserted += '\'';
                    else if (context.quoteMode == QuoteMode::Double)
                        inserted += '"';
                    inserted += ' ';
                }

                // Keep the buffer and the screen in sync: we turned ECHO off,
                // so we have to print exactly what we add.
                input += inserted;
                std::cout << inserted;
                previousTabWasAmbiguous = false;
                continue;
            }

            std::vector<std::string> names;
            names.reserve(candidates.size());
            for (const auto& candidate : candidates)
                names.push_back(candidate.text);

            // --- Step 4: several matches -> insert their shared prefix, if longer ---
            // Several matches may still share more characters than typed.
            const std::string common = longestCommonPrefix(names);
            if (common.size() > context.prefix.size())
            {
                std::string inserted = escapeCompletionSuffix(
                    common.substr(context.prefix.size()), context.quoteMode);
                input += inserted;
                std::cout << inserted;
                previousTabWasAmbiguous = false;
                continue;
            }

            // --- Step 5: truly ambiguous -> beep once, then list on the 2nd TAB ---
            if (!previousTabWasAmbiguous)
            {
                std::cout << '\a';
                previousTabWasAmbiguous = true;
            }
            else
            {
                // List the matches separated by two spaces, then redraw the prompt and
                // the current line so the user can keep typing.
                // Point to note: "$ " is hard-coded here and in main(). If the prompt
                // changes, update both places.
                std::cout << '\n';
                for (size_t i = 0; i < names.size(); ++i)
                {
                    if (i > 0)
                        std::cout << "  ";
                    std::cout << names[i];
                }
                std::cout << "\n$ " << input;
                previousTabWasAmbiguous = false;
            }
            continue;
        }

        //CTRL+D on an empty line
        // ASCII 4 = EOT. Like bash, Ctrl+D only exits on an empty line.
        // Otherwise it's ignored (it's < 32, so the check below skips it).
        if(c == 4 && input.empty())
            return false;


        //Ordinary printable input (byte-oriented)
        // Control characters (< 32) such as ESC and Ctrl+letters are dropped.
        // Bytes >= 128 (UTF-8) are negative as a signed char, so the cast to
        // unsigned char lets them through.
        if(static_cast<unsigned char>(c) >= 32 && c != 127)
        {
            previousTabWasAmbiguous = false;
            input.push_back(c);
            std::cout.put(c);   // Manual echo, since ECHO is off
        }
    }
}

// =============================================================================
// Entry point: the REPL (Read-Eval-Print Loop)
// =============================================================================
int main() {
    // unitbuf = flush after every output operation. Without it the "$ " prompt
    // could sit in the buffer and never appear, and our output could interleave
    // badly with output from child processes, which write to the fd directly.
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true) {
        std::cout << "$ ";
        std::string input;
        if (!readInputLine(input)) {
            break;   // EOF / Ctrl+D -> leave the loop and exit with status 0
        }

        // Lex and parse. Syntax errors are reported, then we show a new prompt.
        ParsedCommand command;
        try
        {
            command = parseCommand(tokenize(input));
        } catch (const std::runtime_error& error)
        {
            std::cerr << "shell: " << error.what() << '\n';
            continue;
        }

        if (command.arguments.empty())
        {
            // Empty line, or only redirections.
            // A bare redirection can still create/truncate a file.
            // (e.g. `> log.txt` empties log.txt, like in bash.)
            ScopedRedirections redirects;
            redirects.apply(command.redirections);
            continue;
        }

        if (isBuiltin(command.arguments[0]))
        {
            // Builtins run in this process, so the redirections must be undone
            // afterwards. `redirects` does that when it goes out of scope.
            ScopedRedirections redirects;
            if (!redirects.apply(command.redirections)) {
                continue;
            }
            if (command.arguments[0] == "exit") {
                // `exit` or `exit N`. Points to note: atoi() returns 0 for
                // non-numeric text, and the OS keeps only the low 8 bits
                // (exit 256 -> status 0). Returning from main (instead of
                // calling std::exit) runs the destructors properly.
                return command.arguments.size() > 1
                    ? std::atoi(command.arguments[1].c_str()) : 0;
            }
            executeBuiltin(command.arguments);
        } else {
            // Everything else is a program on disk: fork + exec.
            executeExternal(command);
        }
    }

    return 0;
}
