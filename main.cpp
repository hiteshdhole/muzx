#include "audio_player.hpp"
#include "config.hpp"
#include "library.hpp"
#include "selection.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

termios originalTerminal{};
bool rawModeEnabled = false;

enum class AppTab { Player, Folders, Queue, Search };

AppTab activeTab = AppTab::Folders;

char getkey() {
  char key = '\0';

  if (read(STDIN_FILENO, &key, 1) != 1)
    return '\0';

  return key;
}

bool keyAvailable() {
  timeval timeout{};
  fd_set readfds;

  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  return select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) > 0;
}

void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return;

  termios terminal = originalTerminal;
  terminal.c_lflag &= ~(ICANON | ECHO);
  terminal.c_cc[VMIN] = 0;
  terminal.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal) == 0)
    rawModeEnabled = true;
}

void disableRawMode() {
  if (rawModeEnabled) {
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTerminal);
    rawModeEnabled = false;
  }
}

bool isMp3(const fs::path &path) {
  std::string extension = path.extension().string();

  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return extension == ".mp3";
}

void showTabs(AppTab active) {
  const char *names[] = {"Player", "Folders", "Queue", "Search"};

  std::cout << "MUZX  ";

  for (int i = 0; i < 4; ++i) {
    if (static_cast<int>(active) == i)
      std::cout << "[" << names[i] << "]  ";
    else
      std::cout << names[i] << "  ";
  }

  std::cout << "\n----------------------------------------------\n\n";
}

void showLibrary(const library &musicLibrary, const selection &cursor) {
  std::cout << "\033[2J\033[H";
  showTabs(AppTab::Folders);

  std::cout << "MUSIC LIBRARY\n\n";

  if (musicLibrary.empty())
    std::cout << "No folders found.\n";

  for (std::size_t i = 0; i < musicLibrary.size(); ++i) {
    std::cout << (i == cursor.current() ? "> " : "  ");
    std::cout << musicLibrary.itemName(i) << '\n';
  }

  std::cout << "\n[j/k] Move  [Enter] Open Folder\n";
  std::cout << "[1] Player  [2] Folders  [3] Queue  [4] Search\n";
  std::cout << "[q] Quit\n";
}

void showPlaceholder(AppTab tab) {
  std::cout << "\033[2J\033[H";
  showTabs(tab);

  switch (tab) {
  case AppTab::Player:
    std::cout << "PLAYER\n\n";
    std::cout << "Open a folder and select a song to play it.\n";
    break;

  case AppTab::Queue:
    std::cout << "QUEUE\n\n";
    std::cout << "Queue functionality will be added next.\n";
    break;

  case AppTab::Search:
    std::cout << "SEARCH\n\n";
    std::cout << "Search functionality will be added next.\n";
    break;

  case AppTab::Folders:
    break;
  }

  std::cout << "\n[Tab] Next tab  [1-4] Select tab  [q] Quit\n";
}

void moveCursorTo(selection &cursor, std::size_t target, std::size_t count) {
  while (cursor.current() < target)
    cursor.moveDown(count);

  while (cursor.current() > target)
    cursor.moveUp(count);
}

void showSongs(const std::string &folder, const std::string &folderName) {
  std::vector<std::string> songs;

  try {
    for (const auto &entry : fs::directory_iterator(folder)) {
      if (entry.is_regular_file() && isMp3(entry.path()))
        songs.push_back(entry.path().string());
    }
  } catch (const fs::filesystem_error &error) {
    std::cerr << "Could not open folder: " << error.what() << '\n';
    usleep(1500000);
    return;
  }

  std::sort(songs.begin(), songs.end());

  if (songs.empty()) {
    std::cout << "\033[2J\033[H";
    std::cout << "No MP3 files found in " << folderName << ".\n";
    std::cout << "Press any key to return.\n";
    while (!keyAvailable())
      usleep(10000);
    getkey();
    return;
  }

  selection cursor;
  audio_player player;

  const std::size_t menuCount = songs.size() + 1;

  bool isPlaying = false;
  bool isPaused = false;
  bool hasTrack = false;
  std::size_t playingIndex = 0;

  auto lastRefresh =
      std::chrono::steady_clock::now() - std::chrono::milliseconds(300);

  while (true) {
    if (isPlaying && player.isFinished()) {
      if (playingIndex + 1 < songs.size()) {
        ++playingIndex;
        moveCursorTo(cursor, playingIndex + 1, menuCount);

        if (player.load(songs[playingIndex])) {
          player.play();
          isPlaying = true;
          isPaused = false;
        } else {
          isPlaying = false;
          isPaused = false;
        }
      } else {
        isPlaying = false;
        isPaused = false;
      }

      lastRefresh =
          std::chrono::steady_clock::now() - std::chrono::milliseconds(300);
    }

    auto now = std::chrono::steady_clock::now();

    if (now - lastRefresh >= std::chrono::milliseconds(300)) {
      std::cout << "\033[2J\033[H";
      showTabs(AppTab::Player);
      std::cout << "FOLDER: " << folderName << "\n\n";

      std::cout << (cursor.current() == 0 ? "> " : "  ") << "[Play All]\n";

      for (std::size_t i = 0; i < songs.size(); ++i) {
        std::cout << (i + 1 == cursor.current() ? "> " : "  ");
        std::cout << fs::path(songs[i]).filename().string();

        if (hasTrack && i == playingIndex && isPlaying)
          std::cout << " [PLAYING]";
        else if (hasTrack && i == playingIndex && isPaused)
          std::cout << " [PAUSED]";

        std::cout << '\n';
      }

      std::cout << "\n----------------------------------------------\n";

      if (hasTrack) {
        std::cout << "Track: "
                  << fs::path(songs[playingIndex]).filename().string() << '\n';

        std::cout << "Status: "
                  << (isPlaying  ? "Playing"
                      : isPaused ? "Paused"
                                 : "Stopped")
                  << '\n';

        int currentSeconds =
            static_cast<int>(player.getPlayingOffset().asSeconds());

        int totalSeconds = static_cast<int>(player.getDuration().asSeconds());

        double progress =
            totalSeconds > 0
                ? static_cast<double>(currentSeconds) / totalSeconds
                : 0.0;

        if (progress > 1.0)
          progress = 1.0;

        const int barWidth = 20;
        int filled = static_cast<int>(progress * barWidth);

        std::cout << "Progress: [";
        for (int i = 0; i < barWidth; ++i)
          std::cout << (i < filled ? '#' : '-');

        std::cout << "] " << currentSeconds / 60 << ':'
                  << (currentSeconds % 60 < 10 ? "0" : "")
                  << currentSeconds % 60 << " / " << totalSeconds / 60 << ':'
                  << (totalSeconds % 60 < 10 ? "0" : "") << totalSeconds % 60
                  << '\n';
      }

      std::cout << "Volume: " << static_cast<int>(player.getVolume()) << "%\n";

      std::cout << "\n[j/k] Move  [Enter] Play Selected\n"
                << "[p] Pause  [r] Resume  [x] Stop\n"
                << "[n] Next  [b] Previous  [+/-] Volume\n"
                << "[q] Back to Folders\n";

      lastRefresh = now;
    }

    if (!keyAvailable()) {
      usleep(10000);
      continue;
    }

    char key = getkey();

    if (key == 'j') {
      cursor.moveDown(menuCount);
    } else if (key == 'k') {
      cursor.moveUp(menuCount);
    } else if (key == '\n' || key == '\r') {
      playingIndex = cursor.current() == 0 ? 0 : cursor.current() - 1;

      hasTrack = true;
      moveCursorTo(cursor, playingIndex + 1, menuCount);

      player.stop();

      if (player.load(songs[playingIndex])) {
        player.play();
        isPlaying = true;
        isPaused = false;
      } else {
        isPlaying = false;
        isPaused = false;
      }
    } else if (key == 'p' && isPlaying) {
      player.pause();
      isPlaying = false;
      isPaused = true;
    } else if (key == 'r' && isPaused) {
      player.resume();
      isPlaying = true;
      isPaused = false;
    } else if (key == 'x') {
      player.stop();
      isPlaying = false;
      isPaused = false;
    } else if (key == 'n') {
      if (playingIndex + 1 < songs.size()) {
        ++playingIndex;
        hasTrack = true;
        moveCursorTo(cursor, playingIndex + 1, menuCount);
        player.stop();

        if (player.load(songs[playingIndex])) {
          player.play();
          isPlaying = true;
          isPaused = false;
        }
      }
    } else if (key == 'b') {
      if (playingIndex > 0 && hasTrack) {
        --playingIndex;
        moveCursorTo(cursor, playingIndex + 1, menuCount);
        player.stop();

        if (player.load(songs[playingIndex])) {
          player.play();
          isPlaying = true;
          isPaused = false;
        }
      }
    } else if (key == '+') {
      player.increaseVolume();
    } else if (key == '-') {
      player.decreaseVolume();
    } else if (key == 'q') {
      player.stop();
      break;
    }

    lastRefresh =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(300);
  }
}

int main() {
  config settings;
  library musicLibrary;
  selection cursor;

  std::string musicFolder = settings.get_music_folder();

  if (musicFolder.empty()) {
    std::cout << "Enter your music folder: ";
    std::getline(std::cin, musicFolder);

    if (!settings.save_music_folder(musicFolder)) {
      std::cerr << "Could not save music folder.\n";
      return 1;
    }
  }

  if (!musicLibrary.load(musicFolder)) {
    std::cerr << "No folders found in: " << musicFolder << '\n';
    return 1;
  }

  enableRawMode();

  bool running = true;

  while (running) {
    if (activeTab == AppTab::Folders)
      showLibrary(musicLibrary, cursor);
    else
      showPlaceholder(activeTab);

    while (!keyAvailable())
      usleep(10000);

    char key = getkey();

    if (key == '\t') {
      activeTab = static_cast<AppTab>((static_cast<int>(activeTab) + 1) % 4);
    } else if (key >= '1' && key <= '4') {
      activeTab = static_cast<AppTab>(key - '1');
    } else if (key == 'j' && activeTab == AppTab::Folders) {
      cursor.moveDown(musicLibrary.size());
    } else if (key == 'k' && activeTab == AppTab::Folders) {
      cursor.moveUp(musicLibrary.size());
    } else if ((key == '\n' || key == '\r') && activeTab == AppTab::Folders) {
      if (!musicLibrary.empty()) {
        showSongs(musicLibrary.item(cursor.current()),
                  musicLibrary.itemName(cursor.current()));
      }
    } else if (key == 'q') {
      running = false;
    }
  }

  disableRawMode();
  return 0;
}
