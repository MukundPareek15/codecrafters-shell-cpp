
#include <iostream>
#include <string>

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  while(true)
  {
    std::cout << "$ ";

    std::string input;
    std::getline(std::cin, input);

    /*In this stage, you'll implement the type builtin for your shell.

    The type Builtin
    The type builtin is used to determine how a command would be interpreted if it were used. It checks whether a command is a builtin, an executable file, or unrecognized.*/

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
