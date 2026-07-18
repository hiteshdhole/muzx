#include <iostream>
#include <string>
int main() {

  std::cout << "---------------MUZX----------------" << std::endl;
  std::cout << "version    : 0.1" << std::endl;
  std::cout << "created by : hitesh" << std::endl;

  std::string command;

  while (true) {
    std::cout << "->" << std::endl;
    std::cin >> command;

    if (command == "quit") {
      std::cout << "good bye" << std::endl;
      break;
    } else if (command == "play") {
      std::cout << "the song is playing " << std::endl;
    } else if (command == "pause") {
      std::cout << "the song is pasue " << std::endl;
    } else {
      std::cout << "command error " << std::endl;
    }
  }
  return 0;
}
