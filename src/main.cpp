
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <stdexcept>


//Define the three states
enum class QuoteMode
{
    Normal,
    Single,
    Double
};

std::vector<std::string> tokenize(const std::string &input)
{
    std::vector<std::string> tokens;

    std::string currentToken;

    QuoteMode mode = QuoteMode::Normal;

    bool tokenStarted = false;

    for (size_t i = 0; i < input.size(); ++i)
    {
        char c = input[i];

        if (mode == QuoteMode::Normal)
        {
            if(std::isspace(static_cast<unsigned char>(c)))
            {
                if (tokenStarted)
                {
                    tokens.push_back(currentToken);
                    currentToken.clear();
                    tokenStarted = false;
                }
            }

            else if (c == '\'')
            {
                mode = QuoteMode::Single;
                tokenStarted = true;
            }

            else if (c == '"')
            {
                mode = QuoteMode::Double;
                tokenStarted = true;
            }

            else if (c == '\\')
            {
                if (i + 1 >= input.size())
                {
                    throw std::runtime_error("Incomplete escape");
                }
            
                char next = input[++i];
            
                if (next != '\n')
                {
                    currentToken.push_back(next);
                    tokenStarted = true;
                }
            }

            else 
            {
                currentToken.push_back(c);
                tokenStarted = true;
            }
        }
        else if (mode == QuoteMode::Single)
        {
            if (c == '\'')
            {
                mode = QuoteMode::Normal;
            }
            else
            {
                currentToken.push_back(c);
            }
        }
        else if (mode == QuoteMode::Double)
        {
            if (c == '"')
            {
                mode = QuoteMode::Normal;
            }
            else if (c == '\\')
            {
                if (i + 1 >= input.size())
                {
                    throw std::runtime_error("Incomplete escape");
                }
            
                char next = input[i + 1];
            
                if (next == '$' ||
                    next == '`' ||
                    next == '"' ||
                    next == '\\')
                {
                    currentToken.push_back(next);
                    ++i;
                }
                else if (next == '\n')
                {
                    ++i;
                }
                else
                {
                    currentToken.push_back('\\');
                }
            }

            else
            {
                currentToken.push_back(c);
            }
        }
    }

    if(mode != QuoteMode::Normal)
    {
        throw std::runtime_error("Unclosed quote");
    }

    if(tokenStarted)
    {
        tokens.push_back(currentToken);
    }
    
    return tokens;
}

// Find an executable using PATH or an explicit file path.
std::string findExecutable(const std::string& command)
{
    namespace fs = std::filesystem;

    auto isExecutable = [](const fs::path& candidate)
    {
        std::error_code ec;
        return fs::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0;
    };

    // Handle absolute and relative paths such as /bin/ls or ./program
    if (command.find('/') != std::string::npos)
    {
        return isExecutable(command) ? command : "";
    }

    const char* path = std::getenv("PATH");

    if (!path)
        return "";

    std::stringstream ss(path);
    std::string directory;

    while (std::getline(ss, directory, ':'))
    {
        fs::path candidate = fs::path(directory.empty() ? "." : directory) / command;

        if (isExecutable(candidate))
        {
            return candidate.string();
        }
    }

    return "";
}

int main()
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    while (true)
    {
        std::cout << "$ ";

        std::string input;

        if (!std::getline(std::cin, input))
        {
            break;
        }

        std::vector<std::string> tokens;

        try
        {
            tokens = tokenize(input);
        }
        catch (std::runtime_error& error)
        {
            std::cerr << "Error: " <<error.what() << std::endl;
            continue;
        }

        if (tokens.empty())
        {
            continue;
        }

        std::string command = tokens[0];

        std::vector<std::string> arguments(tokens.begin() + 1, tokens.end());

        // Builtin: exit
        if (command == "exit")
        {
            return arguments.empty() ? 0 : std::atoi(arguments[0].c_str());
        }

        // Builtin: echo
        else if (command == "echo")
        {
            for (size_t i = 0; i < arguments.size(); i++)
            {
                if (i > 0)
                    std::cout << " ";

                std::cout << arguments[i];
            }

            std::cout << std::endl;
        }

        // Builtin: pwd
        else if (command == "pwd")
        {
          std::filesystem::path cwd = std::filesystem::current_path();
          std::cout << cwd.string() << std::endl;
        }

        // Builtin: cd
        else if (command == "cd")
        {          
          std::filesystem::path target;

          if (arguments.empty() || arguments[0] == "~")
          {
            const char* home = std::getenv("HOME");

            if(!home)
            {
              std::cerr << "cd: HOME not set \n";
              continue;
            }

            target = home;
          }
          else
          {
            target = arguments[0];
          }

          std::error_code ec;

          std::filesystem::current_path(target, ec);

          if(ec)
          {
            std::cout <<"cd: " << target.string() << ": " << ec.message() << std::endl;
          }

          continue;
        }

        // Builtin: type
        else if (command == "type")
        {
            if (arguments.empty())
            {
                std::cout << "type: missing operand" << std::endl;
                continue;
            }

            std::string target = arguments[0];

            if (target == "exit" ||
                target == "echo" ||
                target == "type" ||
                target == "pwd"  ||
                target == "cd")
            {
                std::cout << target << " is a shell builtin" << std::endl;
            }
            else
            {
                std::string executablePath = findExecutable(target);

                if (!executablePath.empty())
                {
                    std::cout << target << " is " << executablePath << std::endl;
                }
                else
                {
                    std::cout << target << ": not found" << std::endl;
                }
            }
        }

        // External commands
        else
        {
            std::string executablePath = findExecutable(command);

            if (executablePath.empty())
            {
                std::cout << command << ": command not found" << std::endl;
                continue;
            }

            // argv[0] must be the command name.
            std::vector<std::string> programArgs;
            programArgs.push_back(command);

            for (const auto& argument : arguments)
            {
                programArgs.push_back(argument);
            }

            // Convert strings into C-style argument pointers.
            std::vector<char*> argv;

            for (auto& argument : programArgs)
            {
                argv.push_back(argument.data());
            }

            // execv requires a null-terminated argument array.
            argv.push_back(nullptr);

            // Create a child process.
            pid_t pid = fork();

            if (pid < 0)
            {
                perror("fork");
            }
            else if (pid == 0)
            {
                // CHILD PROCESS

                if (!outputFile.empty())
                {
                    int fd = open(outputFile.c_str(), O_WRONLY |O_CREAT | O_TRUNC, 0644);

                    if (fd == -1)
                    {
                        perror("open");
                        _exit(1);
                    }

                    if (dup2(fd, STDOUT_FILENO) == -1)
                    {
                        perror("dup2");
                        close(fd);
                        _exit(1);
                    }

                    close(fd);
                    std::cout.flush();
                }

                execv(executablePath.c_str(), argv.data());

                // Only reached if execv fails.
                perror("execv");
                _exit(127);
            }
            else
            {
                // PARENT PROCESS
                int status;
                pid_t result;

                do
                {
                    result = waitpid(pid, &status, 0);
                }
                while (result == -1 && errno == EINTR);

                if (result == -1)
                {
                    perror("waitpid");
                }
            }
        }
    }

    return 0;
}
