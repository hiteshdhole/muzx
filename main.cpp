#include "audio_player.hpp"
#include "config.hpp"
#include "library.hpp"
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
  char key = '\0';
  read(STDIN_FILENO, &key, 1);
  return key;
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

  std::cout << "================ MUSIC LIBRARY ================\n\n";

  if (musicLibrary.empty()) {
    std::cout << "No folders found.\n";
  } else {
    for (std::size_t i = 0; i < musicLibrary.size(); i++) {
      if (i == cursor.current()) {
        std::cout << "> ";
      } else {
        std::cout << "  ";
      }

      std::cout << musicLibrary.itemName(i) << '\n';
    }
  }

  std::cout << "\n[j/k] Move  [Enter] Open  [o] Queue  [q] Quit\n";
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

    std::cout << "================ " << folderName << " ================\n\n";

    if (songs.empty()) {
      std::cout << "No MP3 files found.\n";
    } else {
      for (std::size_t i = 0; i < songs.size(); i++) {
        if (i == songCursor.current()) {
          std::cout << "> ";
        } else {
          std::cout << "  ";
        }

        std::cout << std::filesystem::path(songs[i]).filename().string()
                  << '\n';
      }
    }

    std::cout << "\n[j/k] Move  [Enter] Add to Queue  [q] Back\n";

    char key = getkey();

    if (key == 'j') {
      songCursor.moveDown(songs.size());
    } else if (key == 'k') {
      songCursor.moveUp(songs.size());
    } else if (key == '\n') {
      if (!songs.empty()) {
        std::string song = songs[songCursor.current()];

        musicQueue.add(song);

        std::cout << "\nAdded to queue: "
                  << std::filesystem::path(song).filename().string() << '\n';

        usleep(400000);
      }
    } else if (key == 'q') {
      break;
    }
  }
}

void showQueue(queue &musicQueue) {
  audio_player player;

  selection queueCursor;

  bool isPlaying = false;
  bool isPaused = false;

  std::size_t playingIndex = 0;

  while (true) {
    std::cout << "\033[2J\033[H";

    std::cout << "================ QUEUE ================\n\n";

    if (musicQueue.empty()) {
      std::cout << "Queue is empty\n";
    } else {
      for (std::size_t i = 0; i < musicQueue.size(); i++) {
        if (i == queueCursor.current()) {
          std::cout << "> ";
        } else {
          std::cout << "  ";
        }

        std::cout
            << std::filesystem::path(musicQueue.song(i)).filename().string();

        if (isPlaying && i == playingIndex) {
          std::cout << " [PLAYING]";
        }

        else if (isPaused && i == playingIndex) {
          std::cout << " [PAUSED]";
        }

        std::cout << '\n';
      }
    }

    std::cout << "\n----------------------------------------\n";

    if (isPlaying || isPaused) {
      if (isPlaying) {
        std::cout << "Status: Playing\n";
      } else {
        std::cout << "Status: Paused\n";
      }

      if (playingIndex < musicQueue.size()) {
        std::cout << "Now Playing: "
                  << std::filesystem::path(musicQueue.song(playingIndex))
                         .filename()
                         .string()
                  << '\n';

        int currentSeconds =
            static_cast<int>(player.getPlayingOffset().asSeconds());

        int totalSeconds = static_cast<int>(player.getDuration().asSeconds());

        double progress = 0.0;

        if (totalSeconds > 0) {
          progress = static_cast<double>(currentSeconds) / totalSeconds;
        }

        const int barWidth = 20;

        int filled = static_cast<int>(progress * barWidth);

        std::cout << "Progress: [";

        for (int i = 0; i < barWidth; i++) {
          if (i < filled) {
            std::cout << '#';
          } else {
            std::cout << '-';
          }
        }

        std::cout << "] " << currentSeconds / 60 << ":"
                  << (currentSeconds % 60 < 10 ? "0" : "")
                  << currentSeconds % 60 << " / " << totalSeconds / 60 << ":"
                  << (totalSeconds % 60 < 10 ? "0" : "") << totalSeconds % 60
                  << '\n';
      }
    } else {
      std::cout << "Status: Stopped\n";
    }

    std::cout << "Queue: " << musicQueue.size() << " songs\n";

    std::cout << "Volume: " << static_cast<int>(player.getVolume()) << "%\n";

    std::cout << "----------------------------------------\n";

    std::cout << "[j/k] Move  "
                 "[Enter] Play  "
                 "[p] Pause  "
                 "[r] Resume  "
                 "[n] Next  "
                 "[b] Previous\n";

    std::cout << "[+/-] Volume  "
                 "[d] Remove  "
                 "[c] Clear Queue  "
                 "[q] Back\n";

    // Automatic next song
    if (isPlaying && player.isFinished()) {
      if (playingIndex + 1 < musicQueue.size()) {
        playingIndex++;

        player.stop();

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();

          isPlaying = true;
          isPaused = false;
        }
      } else {
        isPlaying = false;
        isPaused = false;
      }
    }

    // Wait for keyboard input
    if (!keyAvailable()) {
      usleep(10000);
      continue;
    }

    char key = getkey();

    if (key == 'j') {
      queueCursor.moveDown(musicQueue.size());
    }

    else if (key == 'k') {
      queueCursor.moveUp(musicQueue.size());
    }

    else if (key == '\n') {
      if (!musicQueue.empty()) {
        std::size_t selected = queueCursor.current();

        player.stop();

        if (player.load(musicQueue.song(selected))) {
          player.play();

          playingIndex = selected;

          isPlaying = true;
          isPaused = false;
        }
      }
    }

    else if (key == 'p') {
      if (isPlaying) {
        player.pause();

        isPlaying = false;
        isPaused = true;
      }
    }

    else if (key == 'r') {
      if (isPaused) {
        player.resume();

        isPlaying = true;
        isPaused = false;
      }
    }

    else if (key == 'n') {
      if (!musicQueue.empty() && playingIndex + 1 < musicQueue.size()) {
        player.stop();

        playingIndex++;

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();

          isPlaying = true;
          isPaused = false;
        }
      }
    }

    else if (key == 'b') {
      if (!musicQueue.empty() && playingIndex > 0) {
        player.stop();

        playingIndex--;

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();

          isPlaying = true;
          isPaused = false;
        }
      }
    }

    else if (key == '+') {
      player.increaseVolume();
    }

    else if (key == '-') {
      player.decreaseVolume();
    }

    else if (key == 'd') {
      if (!musicQueue.empty()) {
        std::size_t removedIndex = queueCursor.current();

        if ((isPlaying || isPaused) && removedIndex == playingIndex) {
          player.stop();

          isPlaying = false;
          isPaused = false;
        }

        musicQueue.remove(removedIndex);

        if (removedIndex < playingIndex && (isPlaying || isPaused)) {
          playingIndex--;
        }

        if (!musicQueue.empty() && queueCursor.current() >= musicQueue.size()) {
          queueCursor.moveUp(musicQueue.size());
        }

        if (musicQueue.empty()) {
          isPlaying = false;
          isPaused = false;
          playingIndex = 0;
        }
      }
    }

    else if (key == 'c') {
      player.stop();

      musicQueue.clear();

      isPlaying = false;
      isPaused = false;
      playingIndex = 0;
    }

    else if (key == 'q') {
      player.stop();

      musicQueue.save("queue.txt");

      break;
    }
  }
}

int main() {
  config settings;

  library musicLibrary;
  selection cursor;
  queue musicQueue;

  std::string music_folder = settings.get_music_folder();

  if (music_folder.empty()) {
    disableRawMode();

    std::cout << "Enter your music folder: ";

    std::getline(std::cin, music_folder);

    if (!settings.save_music_folder(music_folder)) {
      std::cerr << "Error: Could not save music folder\n";

      return 1;
    }
  }

  if (!musicLibrary.load(music_folder)) {
    std::cerr << "No music folders found\n";

    return 1;
  }

  musicQueue.load("queue.txt");

  enableRawMode();

  while (true) {
    showLibrary(musicLibrary, cursor);

    char key = getkey();

    if (key == 'j') {
      cursor.moveDown(musicLibrary.size());
    }

    else if (key == 'k') {
      cursor.moveUp(musicLibrary.size());
    }

    else if (key == '\n') {
      if (!musicLibrary.empty()) {
        std::string folder = musicLibrary.item(cursor.current());

        showSongs(folder, musicLibrary.itemName(cursor.current()), musicQueue);
      }
    }

    else if (key == 'o') {
      showQueue(musicQueue);
    }

    else if (key == 'q') {
      musicQueue.save("queue.txt");
      break;
    }
  }

  disableRawMode();

  return 0;
}
