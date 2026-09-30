#include "audio_player.hpp"
#include "config.hpp"
#include "playlist.hpp"
#include <iostream>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

char getkey() {
  char key;
  read(STDIN_FILENO, &key, 1);
  return key;
}
void enableRawMode() {
  termios terminal;

  tcgetattr(STDIN_FILENO, &terminal);
  terminal.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}

void disableRawMode() {
  termios terminal;

  tcgetattr(STDIN_FILENO, &terminal);
  terminal.c_lflag |= (ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}
bool keyAvailable() {
  timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = 0;

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  return select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) > 0;
}
int main() {
  audio_player player;

  config settings;

  std::string music_folder = settings.get_music_folder();

  if (music_folder.empty()) {
    std::cout << "Enter your music folder: ";
    std::getline(std::cin, music_folder);

    if (!settings.save_music_folder(music_folder)) {
      std::cerr << "Error: Could not save music folder" << std::endl;
      return 1;
    }
  }

  // Playlist
  playlist music_playlist;

  if (!music_playlist.load(music_folder)) {
    std::cerr << "NO MP3 files found in the selected music folder" << std::endl;
    return 1;
  }

  // Current song index

  // Load first song
  if (!player.load(music_playlist.current())) {
    std::cerr << "Error: Music cannot be loaded" << std::endl;
    return 1;
  }
  std::cout << " ================MUZX PLAYLIST==================" << std::endl;

  for (std::size_t i = 0; i < music_playlist.getSongs().size(); i++) {
    std::cout << i + 1 << " . " << music_playlist.getSongs()[i] << std::endl;
  }

  std::cout << "================================================" << std::endl;
  std::cout << "The music loaded successfully" << music_playlist.current()
            << std::endl;
  std::cout << "Version    : 0.1 " << std::endl;
  std::cout << "Created by : Hitesh " << std::endl;
  std::cout << "[p] Play [o] Pause [q] Quit [n] Next [b] Previous [s] Shuffle"
            << std::endl;
  ;

  bool is_running = true;
  enableRawMode();
  while (is_running) {
    if (player.isFinished()) {
      if (music_playlist.next()) {
        player.stop();

        if (player.load(music_playlist.current())) {
          player.play();

          std::cout << "\nAuto-playing: " << music_playlist.current()
                    << std::endl;
        }
      } else {
        std::cout << "\nPlaylist finished." << std::endl;
        is_running = false;
      }
    }

    if (keyAvailable()) {
      std::cout << "-> " << std::flush;

      char command = getkey();

      if (command == 'q') {
        std::cout << "Goodbye" << std::endl;
        is_running = false;
      }

      else if (command == 'p') {
        player.play();
        std::cout << "The song is playing" << std::endl;
      }

      else if (command == 'o') {
        player.pause();
        std::cout << "The song is paused" << std::endl;
      }

      else if (command == 'n') {
        if (music_playlist.next()) {
          player.stop();

          if (!player.load(music_playlist.current())) {
            std::cerr << "Error: Could not load next song" << std::endl;
          } else {
            player.play();

            std::cout << "Playing: " << music_playlist.current() << '\n'
                      << std::endl;
          }
        } else {
          std::cout << "Already at the last song" << std::endl;
        }
      }

      else if (command == 'b') {
        if (music_playlist.previous()) {
          player.stop();

          if (!player.load(music_playlist.current())) {
            std::cerr << "Error: Could not load previous song" << std::endl;
          } else {
            player.play();

            std::cout << "Playing: " << music_playlist.current() << '\n'
                      << std::endl;
          }
        } else {
          std::cout << "Already at the first song" << std::endl;
        }
      }

      else if (command == 's') {
        if (music_playlist.toggleShuffle()) {
          std::cout << "Shuffle: ON" << std::endl;

          music_playlist.shuffle();

          player.stop();

          if (player.load(music_playlist.current())) {
            player.play();

            std::cout << "Playing: " << music_playlist.current() << std::endl;
          }
        } else {
          std::cout << "Shuffle: OFF" << std::endl;
        }
      }

      else {
        std::cout << "Command error" << std::endl;
      }
    }
  }
  disableRawMode();
  return 0;
}
