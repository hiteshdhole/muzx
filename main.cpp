#include "audio_player.hpp"
#include "config.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// ANSI Color Codes for UI styling
constexpr const char *kReset = "\033[0m";
constexpr const char *kSelected = "\033[7m";      // Reverse video
constexpr const char *kMuted = "\033[90m";        // Gray text
constexpr const char *kPurpleBold = "\033[1;35m"; // Bold Purple text
constexpr const char *kPink = "\033[95m";         // Bright Pink text
constexpr const char *kGreenBold = "\033[1;32m";  // Bold Green for selection

// Key codes above 255 represent terminal escape-sequence keys.
enum Key : int {
  KeyNone = 0,
  KeyEscape = 256,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyEnter,
  KeyBackspace
};

// --------------------------------------------------
// Terminal input & Lifecycle
// --------------------------------------------------

termios originalTerminal{};
bool rawModeEnabled = false;

void enableRawMode() {
  if (rawModeEnabled)
    return;

  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return;

  termios raw = originalTerminal;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0; // Non-blocking read

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
    rawModeEnabled = true;
}

void disableRawMode() {
  if (!rawModeEnabled)
    return;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTerminal);
  rawModeEnabled = false;
}

// RAII wrapper for a clean, flicker-free alternate screen buffer
class TerminalSession {
public:
  TerminalSession() {
    enableRawMode();
    // Switch to alternate screen, hide cursor, clear screen, go home
    std::cout << "\033[?1049h\033[?25l\033[2J\033[H" << std::flush;
  }
  ~TerminalSession() {
    disableRawMode();
    // Show cursor, switch back to main screen buffer
    std::cout << "\033[?25h\033[?1049l" << std::flush;
  }
};

bool waitForInput(int timeoutMs) {
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(STDIN_FILENO, &readSet);
  timeval timeout{};
  timeout.tv_sec = timeoutMs / 1000;
  timeout.tv_usec = (timeoutMs % 1000) * 1000;
  return select(STDIN_FILENO + 1, &readSet, nullptr, nullptr, &timeout) > 0;
}

bool readByte(char &value, int timeoutMs) {
  if (!waitForInput(timeoutMs))
    return false;
  return ::read(STDIN_FILENO, &value, 1) == 1;
}

int readKey() {
  char value = '\0';
  if (!readByte(value, 0))
    return KeyNone;

  if (value == '\n' || value == '\r')
    return KeyEnter;
  if (value == 127 || value == 8)
    return KeyBackspace;
  if (value != '\033')
    return static_cast<unsigned char>(value);

  char second = '\0';
  if (!readByte(second, 35))
    return KeyEscape;
  if (second != '[')
    return KeyEscape;

  char third = '\0';
  if (!readByte(third, 35))
    return KeyEscape;
  switch (third) {
  case 'A':
    return KeyUp;
  case 'B':
    return KeyDown;
  case 'C':
    return KeyRight;
  case 'D':
    return KeyLeft;
  default:
    return KeyNone;
  }
}

// --------------------------------------------------
// File helpers
// --------------------------------------------------

bool isAudioFile(const fs::path &path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" ||
         ext == ".aiff" || ext == ".au";
}

std::string lowerCase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::vector<fs::path> directoryEntries(const fs::path &folder) {
  std::vector<fs::path> entries;
  std::error_code ec;
  fs::directory_iterator it(folder, ec);
  fs::directory_iterator end;

  while (!ec && it != end) {
    const auto &entry = *it;
    if (entry.is_directory(ec) || isAudioFile(entry.path()))
      entries.push_back(fs::absolute(entry.path()).lexically_normal());
    ec.clear();
    it.increment(ec);
  }

  std::sort(entries.begin(), entries.end(),
            [](const fs::path &a, const fs::path &b) {
              std::error_code ec1, ec2;
              bool aDir = fs::is_directory(a, ec1);
              bool bDir = fs::is_directory(b, ec2);
              if (aDir != bDir)
                return aDir;
              return lowerCase(a.filename().string()) <
                     lowerCase(b.filename().string());
            });
  return entries;
}

std::vector<std::string> audioFilesIn(const fs::path &folder) {
  std::vector<std::string> songs;
  std::error_code ec;
  fs::directory_iterator it(folder, ec);
  fs::directory_iterator end;

  while (!ec && it != end) {
    if (it->is_regular_file(ec) && isAudioFile(it->path())) {
      songs.push_back(fs::absolute(it->path()).lexically_normal().string());
    }
    ec.clear();
    it.increment(ec);
  }

  std::sort(songs.begin(), songs.end(),
            [](const std::string &a, const std::string &b) {
              return lowerCase(fs::path(a).filename().string()) <
                     lowerCase(fs::path(b).filename().string());
            });
  return songs;
}

std::vector<fs::path> searchAudioFiles(const fs::path &root,
                                       const std::string &query) {
  std::vector<fs::path> results;
  const std::string wanted = lowerCase(query);
  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;

  while (!ec && it != end) {
    if (it->is_regular_file(ec) && isAudioFile(it->path())) {
      std::string filename = lowerCase(it->path().filename().string());
      if (filename.find(wanted) != std::string::npos) {
        results.push_back(fs::absolute(it->path()).lexically_normal());
      }
    }
    ec.clear();
    it.increment(ec);
  }

  std::sort(results.begin(), results.end());
  return results;
}

std::string fileName(const std::string &path) {
  return fs::path(path).filename().string();
}

// --------------------------------------------------
// Application state
// --------------------------------------------------

enum class AppTab { Player, Folders, Queue, Search };

struct AppState {
  audio_player player;
  queue songQueue;
  AppTab tab = AppTab::Player;
  fs::path musicRoot;
  fs::path browseFolder;
  std::vector<fs::path> folderEntries;
  std::vector<fs::path> searchResults;
  selection folderCursor;
  selection queueCursor;
  selection searchCursor;
  std::string searchQuery;
  std::string currentSong;
  std::vector<std::string> playlist;
  std::size_t playlistIndex = 0;
  bool hasCurrentSong = false;
  bool playbackRunning = false;
  bool paused = false;
};

// --------------------------------------------------
// Playback
// --------------------------------------------------

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index) {
  if (songs.empty() || index >= songs.size())
    return false;

  std::vector<std::string> newPlaylist = songs;
  const std::string path = newPlaylist[index];

  app.player.stop();

  if (!app.player.load(path)) {
    app.hasCurrentSong = false;
    app.playbackRunning = false;
    app.paused = false;
    return false;
  }

  app.playlist = std::move(newPlaylist);
  app.playlistIndex = index;
  app.currentSong = path;
  app.player.play();
  app.hasCurrentSong = true;
  app.playbackRunning = true;
  app.paused = false;
  app.tab = AppTab::Player;
  return true;
}

bool playSongFromFolder(AppState &app, const fs::path &path) {
  std::vector<std::string> songs = audioFilesIn(path.parent_path());
  const std::string selectedPath =
      fs::absolute(path).lexically_normal().string();
  auto found = std::find(songs.begin(), songs.end(), selectedPath);

  if (found == songs.end())
    return false;

  std::size_t index =
      static_cast<std::size_t>(std::distance(songs.begin(), found));
  return playFromPlaylist(app, songs, index);
}

void nextSong(AppState &app) {
  if (!app.hasCurrentSong || app.playlist.empty())
    return;
  std::size_t next = (app.playlistIndex + 1) % app.playlist.size();
  playFromPlaylist(app, app.playlist, next);
}

void previousSong(AppState &app) {
  if (!app.hasCurrentSong || app.playlist.empty())
    return;
  std::size_t previous = (app.playlistIndex == 0) ? app.playlist.size() - 1
                                                  : app.playlistIndex - 1;
  playFromPlaylist(app, app.playlist, previous);
}

void checkAutomaticNext(AppState &app) {
  if (!app.hasCurrentSong || !app.playbackRunning || app.paused)
    return;
  if (!app.player.isFinished())
    return;

  if (app.playlistIndex + 1 < app.playlist.size()) {
    playFromPlaylist(app, app.playlist, app.playlistIndex + 1);
  } else {
    app.playbackRunning = false;
  }
}

void togglePause(AppState &app) {
  if (!app.hasCurrentSong)
    return;
  if (app.paused) {
    app.player.resume();
    app.paused = false;
    app.playbackRunning = true;
  } else {
    app.player.pause();
    app.paused = true;
  }
}

// --------------------------------------------------
// Queue helpers
// --------------------------------------------------

std::vector<std::string> queueAsList(const queue &songQueue) {
  std::vector<std::string> songs;
  for (std::size_t i = 0; i < songQueue.size(); ++i)
    songs.push_back(songQueue.song(i));
  return songs;
}

void addToQueue(AppState &app, const fs::path &path) {
  if (!isAudioFile(path))
    return;
  app.songQueue.add(fs::absolute(path).lexically_normal().string());
}

// --------------------------------------------------
// Search prompt
// --------------------------------------------------

void askSearchQuery(AppState &app) {
  disableRawMode();
  std::cout << "\033[?25h"; // Show cursor for typing
  std::cout << "\033[2J\033[H";
  std::cout << kPurpleBold << "Search songs by filename: " << kReset;
  std::getline(std::cin, app.searchQuery);

  app.searchResults = searchAudioFiles(app.musicRoot, app.searchQuery);
  app.searchCursor = selection{};
  app.tab = AppTab::Search;

  std::cout << "\033[?25l"; // Hide cursor again
  enableRawMode();
}

// --------------------------------------------------
// UI helpers
// --------------------------------------------------

void showTabs(AppTab active) {
  std::cout << kPurpleBold << "MUZX  " << kReset;
  const char *names[] = {"1 Player", "2 Folders", "3 Queue", "4 Search"};

  for (int i = 0; i < 4; ++i) {
    AppTab tab = static_cast<AppTab>(i);
    if (tab == active)
      std::cout << kSelected << " " << names[i] << " " << kReset << "  ";
    else
      std::cout << kMuted << names[i] << kReset << "  ";
  }
  std::cout
      << "\n------------------------------------------------------------\n";
}

void drawList(const std::vector<std::string> &labels,
              std::size_t selectedIndex) {
  if (labels.empty()) {
    std::cout << kMuted << "  Nothing to display.\n" << kReset;
    return;
  }
  if (selectedIndex >= labels.size())
    selectedIndex = labels.size() - 1;

  constexpr std::size_t visibleRows = 18;
  std::size_t start = 0;
  if (selectedIndex >= visibleRows)
    start = selectedIndex - visibleRows + 1;

  std::size_t end = std::min(labels.size(), start + visibleRows);

  if (start > 0)
    std::cout << kMuted << "  ...\n" << kReset;

  for (std::size_t i = start; i < end; ++i) {
    if (i == selectedIndex)
      std::cout << kGreenBold << " > " << labels[i] << kReset << "\n";
    else
      std::cout << "   " << labels[i] << "\n";
  }

  if (end < labels.size())
    std::cout << kMuted << "  ...\n" << kReset;
}

void drawPlayer(const AppState &app) {
  std::cout << "\n" << kPurpleBold << "PLAYER" << kReset << "\n\n";

  if (!app.hasCurrentSong) {
    std::cout << kMuted << "  No song selected.\n" << kReset;
  } else {
    float duration = app.player.getDuration().asSeconds();
    float position = app.player.getPlayingOffset().asSeconds();
    if (position < 0)
      position = 0;
    if (duration > 0 && position > duration)
      position = duration;

    int percent =
        duration > 0 ? static_cast<int>((position / duration) * 100) : 0;

    std::cout << "  Song:   " << kPink << fileName(app.currentSong) << kReset
              << "\n";
    std::cout << "  Folder: " << kMuted
              << fs::path(app.currentSong).parent_path().string() << kReset
              << "\n";

    if (app.paused)
      std::cout << "  Status: " << kMuted << "Paused" << kReset << "\n";
    else if (app.playbackRunning)
      std::cout << "  Status: " << kGreenBold << "Playing" << kReset << "\n";
    else
      std::cout << "  Status: Stopped / Finished\n";

    constexpr int width = 30;
    int filled =
        duration > 0
            ? static_cast<int>((static_cast<float>(percent) / 100) * width)
            : 0;

    std::cout << "\n  [";
    for (int i = 0; i < width; ++i)
      std::cout << (i < filled ? '#' : '-');
    std::cout << "] " << percent << "%\n";

    auto formatTime = [](float seconds) {
      int total = static_cast<int>(seconds);
      int minutes = total / 60;
      int secs = total % 60;
      std::string result = std::to_string(minutes) + ":";
      if (secs < 10)
        result += "0";
      result += std::to_string(secs);
      return result;
    };

    std::cout << "  Time: " << formatTime(position) << " / "
              << formatTime(duration) << "\n";
    std::cout << "  Volume: " << app.player.getVolume() << "%\n";

    if (!app.playlist.empty()) {
      std::cout << "  Track: " << (app.playlistIndex + 1) << " / "
                << app.playlist.size() << "\n";
    }
  }

  std::cout << kMuted << "\nControls: p pause/resume | n next | b previous\n";
  std::cout << "          x stop | +/- volume | a add current song to queue\n"
            << kReset;
}

void drawFolders(const AppState &app) {
  std::cout << "\n" << kPurpleBold << "FOLDERS" << kReset << "\n";
  std::cout << "  Current: " << kPink << app.browseFolder.string() << kReset
            << "\n";
  std::cout << kMuted
            << "  Backspace/h/Left: parent folder | Enter/Right: open/play\n\n"
            << kReset;

  std::vector<std::string> labels;
  for (const auto &entry : app.folderEntries) {
    std::error_code ec;
    if (fs::is_directory(entry, ec))
      labels.push_back(std::string(kMuted) + "[DIR]   " + kReset +
                       entry.filename().string());
    else
      labels.push_back("[SONG]  " + entry.filename().string());
  }
  drawList(labels, app.folderCursor.current());
}

void drawQueue(const AppState &app) {
  std::cout << "\n"
            << kPurpleBold << "QUEUE" << kReset << "  (" << app.songQueue.size()
            << " songs)\n";
  std::cout << kMuted
            << "  Enter: play from here | d: remove | c: clear queue\n\n"
            << kReset;

  std::vector<std::string> labels;
  for (std::size_t i = 0; i < app.songQueue.size(); ++i)
    labels.push_back(fileName(app.songQueue.song(i)));
  drawList(labels, app.queueCursor.current());
}

void drawSearch(const AppState &app) {
  std::cout << "\n" << kPurpleBold << "SEARCH" << kReset << "\n";
  std::cout << "  Query: " << kPink << app.searchQuery << kReset << "\n";
  std::cout << kMuted
            << "  Press / to search again; Enter to play; a to queue\n\n"
            << kReset;

  std::vector<std::string> labels;
  for (const auto &path : app.searchResults)
    labels.push_back(path.filename().string() + kMuted + "  —  " +
                     path.parent_path().string() + kReset);
  drawList(labels, app.searchCursor.current());
}

void drawScreen(const AppState &app) {
  std::ostringstream frame;

  // Intercept std::cout to prevent screen flashing
  std::streambuf *terminalBuffer = std::cout.rdbuf(frame.rdbuf());

  std::cout << kPurpleBold << "M U Z X" << kReset
            << "  |  Terminal Music Player\n\n";

  showTabs(app.tab);

  switch (app.tab) {
  case AppTab::Player:
    drawPlayer(app);
    break;
  case AppTab::Folders:
    drawFolders(app);
    break;
  case AppTab::Queue:
    drawQueue(app);
    break;
  case AppTab::Search:
    drawSearch(app);
    break;
  }

  std::cout
      << kMuted
      << "\n------------------------------------------------------------\n"
      << "  1-4 Tabs | Tab Next | j/k or Arrows Navigate | q Quit\n"
      << kReset;

  std::cout.flush();

  // Restore normal terminal output
  std::cout.rdbuf(terminalBuffer);

  // Update existing lines instead of clearing the entire screen (YOUR smooth
  // method)
  const std::string content = frame.str();
  std::string output = "\033[H";

  std::size_t start = 0;
  while (start < content.size()) {
    std::size_t end = content.find('\n', start);

    if (end == std::string::npos) {
      output.append(content, start, std::string::npos);
      output += "\033[K\n";
      break;
    }

    output.append(content, start, end - start);
    output += "\033[K\n";
    start = end + 1;
  }

  output += "\033[J"; // Clear leftover lines
  std::cout << output << std::flush;
}

} // namespace

// --------------------------------------------------
// Main
// --------------------------------------------------

int main() {
  config settings;
  std::string musicFolder = settings.get_music_folder();

  if (musicFolder.empty()) {
    std::cout << "Enter your music folder path: ";
    std::getline(std::cin, musicFolder);
    if (musicFolder.empty()) {
      std::cerr << "No music folder provided.\n";
      return 1;
    }
    settings.save_music_folder(musicFolder);
  }

  std::error_code ec;
  fs::path root = fs::absolute(musicFolder).lexically_normal();

  if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
    std::cerr << "Invalid music folder: " << root.string() << "\n";
    return 1;
  }

  AppState app;
  app.musicRoot = root;
  app.browseFolder = root;
  app.folderEntries = directoryEntries(root);

  const char *home = std::getenv("HOME");
  fs::path configDirectory =
      home ? fs::path(home) / ".config" / "muzx" : fs::current_path() / ".muzx";
  fs::create_directories(configDirectory, ec);
  const fs::path queueFile = configDirectory / "queue.txt";
  app.songQueue.load(queueFile.string());

  TerminalSession terminal; // Starts RAII full screen mode
  bool running = true;

  while (running) {
    checkAutomaticNext(app);
    drawScreen(app);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int key = readKey();
    if (key == KeyNone)
      continue;

    if (key >= '1' && key <= '4') {
      app.tab = static_cast<AppTab>(key - '1');
      continue;
    }

    if (key == '\t') {
      app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 1) % 4);
      continue;
    }

    if (key == 'q' || key == KeyEscape) {
      running = false;
      continue;
    }

    if (key == 'p') {
      togglePause(app);
      continue;
    }

    if (key == 'n') {
      nextSong(app);
      continue;
    }
    if (key == 'b') {
      previousSong(app);
      continue;
    }

    if (key == 'x') {
      app.player.stop();
      app.playbackRunning = false;
      app.paused = false;
      continue;
    }

    if (key == '+' || key == '=') {
      app.player.increaseVolume();
      continue;
    }

    if (key == '-') {
      app.player.decreaseVolume();
      continue;
    }

    if (key == 'j' || key == KeyDown) {
      switch (app.tab) {
      case AppTab::Folders:
        if (!app.folderEntries.empty())
          app.folderCursor.moveDown(app.folderEntries.size());
        break;
      case AppTab::Queue:
        if (!app.songQueue.empty())
          app.queueCursor.moveDown(app.songQueue.size());
        break;
      case AppTab::Search:
        if (!app.searchResults.empty())
          app.searchCursor.moveDown(app.searchResults.size());
        break;
      case AppTab::Player:
        break;
      }
      continue;
    }

    if (key == 'k' || key == KeyUp) {
      switch (app.tab) {
      case AppTab::Folders:
        if (!app.folderEntries.empty())
          app.folderCursor.moveUp(app.folderEntries.size());
        break;
      case AppTab::Queue:
        if (!app.songQueue.empty())
          app.queueCursor.moveUp(app.songQueue.size());
        break;
      case AppTab::Search:
        if (!app.searchResults.empty())
          app.searchCursor.moveUp(app.searchResults.size());
        break;
      case AppTab::Player:
        break;
      }
      continue;
    }

    if (app.tab == AppTab::Folders) {
      if (key == 'h' || key == KeyBackspace || key == KeyLeft) {
        if (app.browseFolder != app.musicRoot) {
          app.browseFolder = app.browseFolder.parent_path();
          app.folderEntries = directoryEntries(app.browseFolder);
          app.folderCursor = selection{};
        }
      } else if (key == 'a') {
        if (!app.folderEntries.empty()) {
          std::size_t index = app.folderCursor.current();
          if (index < app.folderEntries.size())
            addToQueue(app, app.folderEntries[index]);
        }
      } else if (key == KeyEnter || key == 'l' || key == KeyRight) {
        if (!app.folderEntries.empty()) {
          std::size_t index = app.folderCursor.current();
          if (index < app.folderEntries.size()) {
            fs::path chosen = app.folderEntries[index];
            if (fs::is_directory(chosen, ec)) {
              app.browseFolder = chosen;
              app.folderEntries = directoryEntries(chosen);
              app.folderCursor = selection{};
            } else {
              playSongFromFolder(app, chosen);
            }
          }
        }
      }
    }

    if (app.tab == AppTab::Queue) {
      if (key == 'd' && !app.songQueue.empty()) {
        std::size_t index = app.queueCursor.current();
        if (index < app.songQueue.size())
          app.songQueue.remove(index);
        app.queueCursor = selection{};
        app.songQueue.save(queueFile.string());
      } else if (key == 'c') {
        app.songQueue.clear();
        app.queueCursor = selection{};
        app.songQueue.save(queueFile.string());
      } else if (key == KeyEnter && !app.songQueue.empty()) {
        std::size_t index = app.queueCursor.current();
        if (index < app.songQueue.size()) {
          playFromPlaylist(app, queueAsList(app.songQueue), index);
        }
      }
    }

    if (app.tab == AppTab::Search) {
      if (key == '/') {
        askSearchQuery(app);
      } else if (key == 'a' && !app.searchResults.empty()) {
        std::size_t index = app.searchCursor.current();
        if (index < app.searchResults.size())
          addToQueue(app, app.searchResults[index]);
      } else if (key == KeyEnter && !app.searchResults.empty()) {
        std::size_t index = app.searchCursor.current();
        if (index < app.searchResults.size())
          playSongFromFolder(app, app.searchResults[index]);
      }
    }

    if (app.tab == AppTab::Player && key == 'a' && app.hasCurrentSong) {
      addToQueue(app, app.currentSong);
    }

    app.songQueue.save(queueFile.string());
  }

  app.player.stop();
  app.songQueue.save(queueFile.string());

  return 0; // TerminalSession handles cleanup automatically
}
