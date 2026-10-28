
#include <iostream>
#include <string>
#include <cstdlib>
#include <sstream>
#include <vector>
#include <filesystem>
#include <unistd.h>

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  while(true)
  {
    std::cout << "$ ";

    std::string input;
    std::getline(std::cin, input);

    if (input.substr(0,5) == "type ")
    {
      std::string command = input.substr(5);
      if (command == "exit" || command == "echo" || command == "type")
      {
        std::cout << command << " is a shell builtin" << std::endl;
      } else if (const char* path = std::getenv("PATH"))
      {
        std::string pathStr(path);

        std::stringstream ss(pathStr);

        std::vector<std::string> directories;

        std::string directory;

        while (std::getline(ss, directory, ':'))
        {
          directories.push_back(directory);
        }

        bool found = false;

        for (const auto& dir : directories)
        {
          std::filesystem::path candidate =
            std::filesystem::path(dir) / command;

            if (std::filesystem::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0)
              {
                std::cout << command << " is " << candidate.string() << std::endl;
                found = true;
                break;
              }
        }

        if (!found)
        {
          std::cout << command << ": not found" << std::endl;
        }
        
      }else      
      {
        std::cout << command << ": not found" << std::endl;
      }

    } else if(const char* path = std::getenv("PATH"))
    {
      std::string pathStr(path);
      std::stringstream ss(pathStr);
      std::vector<std::string> directories;
      std::string directory;
      while (std::getline(ss, directory, ':'))
      {
        directories.push_back(directory);
      }
      bool found = false;
      for (const auto& dir : directories)
      {
        std::filesystem::path candidate =
          std::filesystem::path(dir) / command;
          if (std::filesystem::is_regular_file(candidate) && access(candidate.c_str(), X_OK) == 0)
            {
              std::cout << command << " is " << candidate.string() << "~ the program is executing here"<< std::endl;
              found = true;
              break;
            }
      }
      if (!found)
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
