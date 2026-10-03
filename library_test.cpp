#include "audio_player.hpp"
#include "library.hpp"
#include "queue.hpp"
#include "selection.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

char getKey() {
  char key = '\0';

  read(STDIN_FILENO, &key, 1);

  return key;
}

void enableRawMode() {
  termios terminal;

  tcgetattr(STDIN_FILENO, &terminal);

  terminal.c_lflag &= ~(ICANON | ECHO);

  terminal.c_cc[VMIN] = 0;
  terminal.c_cc[VTIME] = 1;

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

  std::cout
      << "\n[j/k] Move   [Enter] Open   [o] Queue   [d]delete  [q] Quit\n";
}
void showQueue(queue &musicQueue) {
  audio_player player;
  selection queueCursor;

  bool isPlaying = false;
  bool isPaused = false;
  std::size_t playingIndex = 0;

  while (true) {
    std::cout << "\033[2J\033[H";

    std::cout << "QUEUE\n\n";

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

        std::cout << '\n';
      }
    }
    std::cout << "\n--------------------------------\n";

    if (isPlaying) {
      std::cout << "Status: Playing\n";

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
        if (i < filled)
          std::cout << '#';
        else
          std::cout << '-';
      }

      std::cout << "] " << currentSeconds / 60 << ":"
                << (currentSeconds % 60 < 10 ? "0" : "") << currentSeconds % 60
                << " / " << totalSeconds / 60 << ":"
                << (totalSeconds % 60 < 10 ? "0" : "") << totalSeconds % 60
                << '\n';
    } else if (isPaused) {
      std::cout << "Status: Paused\n";
      std::cout << "Now Playing: "
                << std::filesystem::path(musicQueue.song(playingIndex))
                       .filename()
                       .string()
                << '\n';
    } else {
      std::cout << "Status: Stopped\n";
    }
    std::cout << "Queue: " << musicQueue.size() << " songs\n";

    std::cout << "--------------------------------\n";

    std::cout << "[j/k] Move  [Enter] Play  [p] Pause  [r] Resume  [n] Next  "
                 "[b] Previous  [d] Remove  [q] Back\n";

    // Check if current song finished
    if (isPlaying && player.isFinished()) {

      if (playingIndex + 1 < musicQueue.size()) {

        playingIndex++;

        queueCursor.moveDown(musicQueue.size());

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();
        }
      } else {
        isPlaying = false;
      }
    }

    char key = getKey();

    if (key == 'j') {
      queueCursor.moveDown(musicQueue.size());
    }

    else if (key == 'k') {
      queueCursor.moveUp(musicQueue.size());
    }

    else if (key == '\n') {

      if (!musicQueue.empty()) {

        player.stop();

        if (player.load(musicQueue.song(queueCursor.current()))) {

          player.play();

          playingIndex = queueCursor.current();

          isPlaying = true;
        }
      }
    }

    else if (key == 'p') {
      player.pause();
      isPlaying = false;
      isPaused = true;
    } else if (key == 'r') {
      player.resume();
      isPlaying = true;
      isPaused = false;
      playingIndex = queueCursor.current();
    } else if (key == 'n') {
      if (!musicQueue.empty() && playingIndex + 1 < musicQueue.size()) {

        player.stop();

        playingIndex++;

        queueCursor.moveDown(musicQueue.size());

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();
          isPlaying = true;
        }
      }
    } else if (key == 'b') {
      if (!musicQueue.empty() && playingIndex > 0) {

        player.stop();

        playingIndex--;

        queueCursor.moveUp(musicQueue.size());

        if (player.load(musicQueue.song(playingIndex))) {
          player.play();
          isPlaying = true;
        }
      }
    } else if (key == 'd') {
      if (!musicQueue.empty()) {

        std::size_t removedIndex = queueCursor.current();

        if (isPlaying && removedIndex == playingIndex) {
          player.stop();
          isPlaying = false;
        }

        musicQueue.remove(removedIndex);

        if (removedIndex < playingIndex && isPlaying) {
          playingIndex--;
        }

        if (!musicQueue.empty() && queueCursor.current() >= musicQueue.size()) {
          queueCursor.moveUp(musicQueue.size());
        }
      }
    } else if (key == 'q') {
      player.stop();
      break;
    }
  }
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
  audio_player player;

  std::size_t scrollOffset = 0;
  std::size_t visibleItems = 10;

  while (true) {
    std::cout << "\033[2J\033[H";

    std::cout << folderName << "\n\n";

    for (std::size_t i = scrollOffset;
         i < songs.size() && i < scrollOffset + visibleItems; i++) {

      if (i == songCursor.current()) {
        std::cout << "> ";
      } else {
        std::cout << "  ";
      }

      std::cout << std::filesystem::path(songs[i]).filename().string() << '\n';
    }

    std::cout << "\nQueue:\n";

    for (std::size_t i = 0; i < musicQueue.size(); i++) {

      std::cout << "  "
                << std::filesystem::path(musicQueue.song(i)).filename().string()
                << '\n';
    }

    std::cout << "\n[j/k] Move   [Enter] Select   [q] Back\n";

    char key = getKey();

    if (key == 'j') {
      songCursor.moveDown(songs.size());

      if (songCursor.current() >= scrollOffset + visibleItems) {
        scrollOffset++;
      }
    } else if (key == 'k') {
      songCursor.moveUp(songs.size());

      if (songCursor.current() < scrollOffset) {
        scrollOffset--;
      }
    } else if (key == '\n') {
      if (!songs.empty()) {
        std::string song = songs[songCursor.current()];

        musicQueue.add(song);

        std::cout << "\nAdded to queue: "
                  << std::filesystem::path(song).filename().string() << "\n";

        getKey();
      }
    } else if (key == 'q') {
      break;
    }
  }
}

int main() {
  library musicLibrary;
  selection cursor;
  queue musicQueue;

  if (!musicLibrary.load("/home/hitesh/Hitesh/Music")) {

    std::cout << "No folders found\n";

    return 1;
  }

  enableRawMode();

  bool running = true;

  while (running) {
    showLibrary(musicLibrary, cursor);

    char key = getKey();

    if (key == 'j') {
      cursor.moveDown(musicLibrary.size());
    } else if (key == 'k') {
      cursor.moveUp(musicLibrary.size());
    } else if (key == '\n') {
      std::string folder = musicLibrary.item(cursor.current());

      showSongs(folder, musicLibrary.itemName(cursor.current()), musicQueue);
    } else if (key == 'o') {
      showQueue(musicQueue);
    } else if (key == 'q') {
      running = false;
    }
  }

  disableRawMode();

  return 0;
}
