#include "audio_player.hpp"
#include "config.hpp"
#include "library.hpp"
#include "playlist.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

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

void showLibrary(const library &musicLibrary, const selection &cursor) {
  std::cout << "\033[2J\033[H";

  std::cout << "MUSIC LIBRARY\n\n";

  for (std::size_t i = 0; i < musicLibrary.size(); i++) {
    if (i == cursor.current()) {
      std::cout << "> ";
    } else {
      std::cout << "  ";
    }

    std::cout << musicLibrary.itemName(i) << '\n';
  }

  std::cout << "\n[j/k] Move   [Enter] Open   [o] Queue   [q] Quit\n";
}

void showSongs(const std::string &folder, const std::string &folderName,
               queue &musicQueue) {
  std::vector<std::string> songs;

  for (const auto &entry : std::filesystem::directory_iterator(folder)) {
    if (entry.path().extension() == ".mp3") {
      songs.push_back(entry.path().string());
    }
  }

  selection songCursor;

  while (true) {
    std::cout << "\033[2J\033[H";

    std::cout << folderName << "\n\n";

    for (std::size_t i = 0; i < songs.size(); i++) {
      if (i == songCursor.current()) {
        std::cout << "> ";
      } else {
        std::cout << "  ";
      }

      std::cout << std::filesystem::path(songs[i]).filename().string() << '\n';
    }

    std::cout << "\n[j/k] Move   [Enter] Add to Queue   [q] Back\n";

    char key = getkey();

    if (key == 'j') {
      songCursor.moveDown(songs.size());
    } else if (key == 'k') {
      songCursor.moveUp(songs.size());
    } else if (key == '\n') {
      if (!songs.empty()) {
        musicQueue.add(songs[songCursor.current()]);

        std::cout << "\nAdded to queue: "
                  << std::filesystem::path(songs[songCursor.current()])
                         .filename()
                         .string()
                  << '\n';

        usleep(500000);
      }
    } else if (key == 'q') {
      break;
    }
  }
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

  library musicLibrary;
  selection cursor;
  queue musicQueue;

  std::string music_folder = settings.get_music_folder();

  if (music_folder.empty()) {
    std::cout << "Enter your music folder: ";
    std::getline(std::cin, music_folder);

    if (!settings.save_music_folder(music_folder)) {
      std::cerr << "Error: Could not save music folder" << std::endl;
      return 1;
    }
  }

  if (!musicLibrary.load(music_folder)) {
    std::cerr << "No music folders found" << std::endl;
    return 1;
  }

  // Playlist
  playlist music_playlist;

  if (!music_playlist.load(music_folder)) {
    std::cerr << "NO MP3 files found in the selected music folder" << std::endl;
    return 1;
  }

  enableRawMode();

  while (true) {
    showLibrary(musicLibrary, cursor);

    char key = getkey();

    if (key == 'j') {
      cursor.moveDown(musicLibrary.size());
    } else if (key == 'k') {
      cursor.moveUp(musicLibrary.size());
    } else if (key == '\n') {
      if (!musicLibrary.empty()) {
        std::string folder = musicLibrary.item(cursor.current());

        showSongs(folder, musicLibrary.itemName(cursor.current()), musicQueue);
      }
    } else if (key == 'q') {
      break;
    }
  }

  disableRawMode();

  return 0;

  // Current song index

  // Load first song
  if (!player.load(music_playlist.current())) {
    std::cerr << "Error: Music cannot be loaded" << std::endl;
    return 1;
  }
  std::cout << "================ MUZX PLAYLIST ================" << std::endl;

  std::cout << "Now Playing: " << music_playlist.currentName() << std::endl;

  std::cout << "Track: " << music_playlist.currentIndex() + 1 << " / "
            << music_playlist.getSongs().size() << std::endl;
  std::cout << "================================================" << std::endl;

  for (std::size_t i = 0; i < music_playlist.getSongs().size(); i++) {
    std::cout << i + 1 << ". " << music_playlist.songName(i) << std::endl;
  }

  std::cout << "================================================" << std::endl;

  std::cout << "[p] Play  [o] Pause  [x] Stop  [n] Next  "
            << "[b] Previous  [s] Shuffle  [q] Quit" << std::endl;

  bool is_running = true;
  bool manuallyStopped = false;
  enableRawMode();

  while (is_running) {
    if (player.isFinished() && !manuallyStopped) {
      if (music_playlist.next()) {
        player.stop();

        if (player.load(music_playlist.current())) {
          player.play();

          std::cout << "\nAuto-playing: " << music_playlist.currentName()
                    << std::endl;

          std::cout << "Track: " << music_playlist.currentIndex() + 1 << " / "
                    << music_playlist.getSongs().size() << std::endl;
        }
      } else {
        std::cout << "\nPlaylist finished." << std::endl;
        is_running = false;
      }
    }

    if (keyAvailable()) {
      char command = getkey();

      if (command == 'q') {
        std::cout << "Goodbye" << std::endl;
        is_running = false;
      }

      else if (command == 'p') {
        manuallyStopped = false;
        player.play();

        std::cout << "The song is playing" << std::endl;
      }

      else if (command == 'o') {
        player.pause();

        std::cout << "The song is paused" << std::endl;
      }

      else if (command == 'x') {
        player.stop();
        manuallyStopped = true;

        std::cout << "Stopped: " << music_playlist.currentName() << std::endl;
      }

      else if (command == 'n') {
        if (music_playlist.next()) {
          manuallyStopped = false;
          player.stop();

          if (!player.load(music_playlist.current())) {
            std::cerr << "Error: Could not load next song" << std::endl;
          } else {
            player.play();
            std::cout << "Playing: " << music_playlist.currentName()
                      << std::endl;

            std::cout << "Track: " << music_playlist.currentIndex() + 1 << " / "
                      << music_playlist.getSongs().size() << "\n"
                      << std::endl;
          }
        } else {
          std::cout << "Already at the last song" << std::endl;
        }
      }

      else if (command == 'b') {
        if (music_playlist.previous()) {
          manuallyStopped = false;
          player.stop();

          if (!player.load(music_playlist.current())) {
            std::cerr << "Error: Could not load previous song" << std::endl;
          } else {
            player.play();

            std::cout << "Playing: " << music_playlist.currentName()
                      << std::endl;

            std::cout << "Track: " << music_playlist.currentIndex() + 1 << " / "
                      << music_playlist.getSongs().size() << std::endl;
          }
        } else {
          std::cout << "Already at the first song" << std::endl;
        }
      }

      else if (command == 's') {
        if (music_playlist.toggleShuffle()) {
          std::cout << "Shuffle: ON" << std::endl;

          music_playlist.shuffle();

          manuallyStopped = false;
          player.stop();

          if (player.load(music_playlist.current())) {
            player.play();

            std::cout << "Playing: " << music_playlist.currentName()
                      << std::endl;
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
