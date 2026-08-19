#include <SFML/Audio.hpp>
#include <iostream>
#include <string>
int main() {

  sf::Music song;
  // ask user to input the music
  std::string filename;

  std::cout << "enter the file name : " << std::endl;
  std::getline(std::cin, filename);

  // load the music
  if (!song.openFromFile(home / Hitesh / projects / github / muzx / test.mp3)) {
    std::cerr << "Error :: Music can not be loaded " << std::endl;
    return 1;
  }
  // std::cout << "version    : 0.1" << std::endl;
  // std::cout << "created by : hitesh" << std::endl;
  // std::cout << "[p] play [o] pasue [q] quit " << std::endl;

  std::string command;
  bool is_running = true;
  while (is_running) {
    std::cout << "->" << std::endl;
    std::cin >> command;

    if (command == "q") {
      std::cout << "good bye" << std::endl;
      is_running = false;
    } else if (command == "p") {
      std::cout << "the song is playing " << std::endl;
    } else if (command == "o") {
      std::cout << "the song is pasue " << std::endl;
    } else {
      std::cout << "command error " << std::endl;
    }
  }
  return 0;
}
