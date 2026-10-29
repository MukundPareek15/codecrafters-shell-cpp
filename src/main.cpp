
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <cstdio>

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

        // Parse command and arguments
        std::istringstream stream(input);

        std::string command;
        stream >> command;

        if (command.empty())
        {
            continue;
        }

        std::vector<std::string> arguments;
        std::string arg;

        while (stream >> arg)
        {
            arguments.push_back(arg);
        }

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

        else if (command == "pwd")
        {
          std::filesystem::path cwd = std::filesystem::current_path();
          std::cout << cwd.string() << std::endl;
        }

        else if (command == "cd")
        {
          if (arguments.size() < 2)
          {
            continue;
          }

          std::filesystem::path target = arguments[1];

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
