#include <SFML/Audio.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
  sf::Music song;

  // Playlist
  std::vector<std::string> playlist;

  for (const auto &file : std::filesystem::directory_iterator("music")) {
    if (file.path().extension() == ".mp3") {
      playlist.push_back(file.path().string());
    }
  }

  if (playlist.empty()) {
    std::cerr << "NO MP3 files found in the music folder " << std::endl;
    return 1;
  }

  // Current song index
  std::size_t currentSong = 0;

  // Load first song
  if (!song.openFromFile(playlist[currentSong])) {
    std::cerr << "Error: Music cannot be loaded" << std::endl;
    return 1;
  }

  std::cout << "The music loaded successfully" << playlist[currentSong]
            << std::endl;
  std::cout << "Version    : 0.1 " << std::endl;
  std::cout << "Created by : Hitesh " << std::endl;
  std::cout << "[p] Play [o] Pause [q] Quit [n] Next [b] Previous" << std::endl;
  ;

  std::string command;
  bool is_running = true;

  while (is_running) {
    std::cout << "-> ";
    std::cin >> command;

    if (command == "q") {
      std::cout << "Goodbye" << std::endl;
      is_running = false;
    }

    else if (command == "p") {
      song.play();
      std::cout << "The song is playing" << std::endl;
    }

    else if (command == "o") {
      song.pause();
      std::cout << "The song is paused" << std::endl;
    }

    else if (command == "n") {
      if (currentSong < playlist.size() - 1) {
        currentSong++; // currentSong = currentSong +  1
        song.stop();

        if (!song.openFromFile(playlist[currentSong])) {
          std::cerr << "Error: Could not load next song" << std::endl;
        } else {
          song.play();

          std::cout << "Playing: " << playlist[currentSong] << '\n'
                    << std::endl;
        }
      } else {
        std::cout << "Already at the last song" << std::endl;
      }
    }

    else if (command == "b") {
      if (currentSong > 0) {
        currentSong--; // currentSong = currentSong - 1

        song.stop();

        if (!song.openFromFile(playlist[currentSong])) {

          std::cerr << "Error: Could not load previous song " << std::endl;
        } else {
          song.play();

          std::cout << "Playing: " << playlist[currentSong] << '\n'
                    << std::endl;
        }
      } else {
        std::cout << "Already at the first song" << std::endl;
      }
    }

    else {
      std::cout << "Command error" << std::endl;
    }
  }

  return 0;
}
