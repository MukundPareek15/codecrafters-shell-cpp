
#include <iostream>
#include <string>
#include <cstdlib>

/*When type receives a command input, your shell must follow these steps:

Check if the command is a builtin command (like exit or echo). If it is, report it as a builtin (<command> is a shell builtin) and stop.
If the command is not a builtin, your shell must go through every directory in PATH. For each directory:
Check if a file with the command name exists.
Check if the file has execute permissions.
If the file exists and has execute permissions, print <command> is <full_path> and stop.
If the file exists but lacks execute permissions, skip it and continue to the next directory.
If no executable is found in any directory, print <command>: not found.*/

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  while(true)
  {
    std::cout << "$ ";

    const char* path = std::getenv("PATH");

    std::string input;
    std::getline(std::cin, input);

    if (input.substr(0,5) == "type ")
    {
      std::string command = input.substr(5);
      if (command == "exit" || command == "echo" || command == "type")
      {
        std::cout << command << " is a shell builtin" << std::endl;
      } else
      {
        std::cout << command << ": not found" << std::endl;
      }
    } else if(input == "exit")
    {
      break;
    } else if (input.substr(0,5) == "echo ")
    {
      std::cout << input.substr(5) << std::endl;
    } else
    {
      std::cout << input << ": command not found" << std::endl;
    }
  }

  
}
