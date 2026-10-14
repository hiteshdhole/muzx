#include "audio_player.hpp"
#include "config.hpp"
#include "cover.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr const char *kReset = "\033[0m";
constexpr const char *kSelected = "\033[7m";
constexpr const char *kMuted = "\033[90m";
constexpr const char *kPurpleBold = "\033[1;35m";
constexpr const char *kPink = "\033[95m";
constexpr const char *kGreenBold = "\033[1;32m";

enum Key : int {
  KeyNone = 0,
  KeyEscape = 256,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyShiftTab,
  KeyEnter,
  KeyBackspace
};

termios originalTerminal{};
bool rawModeEnabled = false;

void enableRawMode() {
  if (rawModeEnabled)
    return;
  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return;

  termios raw = originalTerminal;
  raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
    rawModeEnabled = true;
}

void disableRawMode() {
  if (!rawModeEnabled)
    return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTerminal);
  rawModeEnabled = false;
}

class TerminalSession {
public:
  TerminalSession() {
    enableRawMode();
    // \033[?7l disables line wrapping so long song names never wrap and scroll
    // the screen
    std::cout << "\033[?1049h\033[?25l\033[?7l\033[2J\033[H" << std::flush;
  }
  ~TerminalSession() {
    clearTerminalCover();
    disableRawMode();
    std::cout << "\033[?7h\033[?25h\033[?1049l" << std::flush;
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
  case 'Z':
    return KeyShiftTab;
  default:
    return KeyNone;
  }
}

std::string lowerCase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isAudioFile(const fs::path &path) {
  const std::string ext = lowerCase(path.extension().string());
  return ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" ||
         ext == ".aiff" || ext == ".au";
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

std::vector<std::string> audioFilesIn(const fs::path &folder,
                                      bool recursive = false) {
  std::vector<std::string> songs;
  std::error_code ec;

  if (recursive) {
    fs::recursive_directory_iterator it(
        folder, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end) {
      if (it->is_regular_file(ec) && isAudioFile(it->path()))
        songs.push_back(fs::absolute(it->path()).lexically_normal().string());
      ec.clear();
      it.increment(ec);
    }
  } else {
    fs::directory_iterator it(folder, ec);
    fs::directory_iterator end;
    while (!ec && it != end) {
      if (it->is_regular_file(ec) && isAudioFile(it->path()))
        songs.push_back(fs::absolute(it->path()).lexically_normal().string());
      ec.clear();
      it.increment(ec);
    }
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
  if (wanted.empty())
    return results;

  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;

  while (!ec && it != end) {
    if (it->is_regular_file(ec) && isAudioFile(it->path())) {
      std::string filename = lowerCase(it->path().filename().string());
      if (filename.find(wanted) != std::string::npos)
        results.push_back(fs::absolute(it->path()).lexically_normal());
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

enum class AppTab {
  Player = 0,
  Folders = 1,
  Queue = 2,
  Search = 3,
  Keybindings = 4
};

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
  bool shuffle = false;
  bool showAlbumArt = false;
  std::mt19937 rng{std::random_device{}()};
};

std::size_t folderMenuSize(const AppState &app) {
  return app.folderEntries.empty() ? 0 : app.folderEntries.size() + 1;
}

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index, bool switchToPlayer = true) {
  if (songs.empty() || index >= songs.size())
    return false;

  std::vector<std::string> newPlaylist = songs;
  std::size_t currentIndex = index;
  bool loadedSuccessfully = false;

  for (std::size_t attempts = 0; attempts < newPlaylist.size(); ++attempts) {
    const std::string path = newPlaylist[currentIndex];
    app.player.stop();

    try {
      if (app.player.load(path)) {
        app.player.play();
        app.playlist = std::move(newPlaylist);
        app.playlistIndex = currentIndex;
        app.currentSong = path;
        app.hasCurrentSong = true;
        app.playbackRunning = true;
        app.paused = false;
        if (switchToPlayer)
          app.tab = AppTab::Player;
        loadedSuccessfully = true;
        break;
      }
    } catch (...) {
    }

    currentIndex = (currentIndex + 1) % newPlaylist.size();
  }

  if (!loadedSuccessfully) {
    app.hasCurrentSong = false;
    app.playbackRunning = false;
    app.paused = false;
    return false;
  }

  return true;
}

bool playSongFromFolder(AppState &app, const fs::path &path) {
  std::vector<std::string> songs = audioFilesIn(path.parent_path(), false);
  const std::string selectedPath =
      fs::absolute(path).lexically_normal().string();
  auto found = std::find(songs.begin(), songs.end(), selectedPath);
  if (found == songs.end())
    return false;

  std::size_t index =
      static_cast<std::size_t>(std::distance(songs.begin(), found));
  return playFromPlaylist(app, songs, index);
}

bool playFolder(AppState &app, const fs::path &folder, bool recursive = true) {
  std::vector<std::string> songs = audioFilesIn(folder, recursive);
  if (songs.empty())
    return false;

  std::size_t startIndex = 0;
  if (app.shuffle && songs.size() > 1) {
    std::uniform_int_distribution<std::size_t> dist(0, songs.size() - 1);
    startIndex = dist(app.rng);
  }
  return playFromPlaylist(app, songs, startIndex);
}

std::size_t pickNextIndex(AppState &app) {
  if (app.playlist.size() <= 1)
    return 0;
  if (!app.shuffle)
    return (app.playlistIndex + 1) % app.playlist.size();

  std::uniform_int_distribution<std::size_t> dist(0, app.playlist.size() - 2);
  std::size_t next = dist(app.rng);
  if (next >= app.playlistIndex)
    ++next;
  return next;
}

void nextSong(AppState &app) {
  if (!app.hasCurrentSong || app.playlist.empty())
    return;
  playFromPlaylist(app, app.playlist, pickNextIndex(app), false);
}

void previousSong(AppState &app) {
  if (!app.hasCurrentSong || app.playlist.empty())
    return;
  std::size_t previous = (app.playlistIndex == 0) ? app.playlist.size() - 1
                                                  : app.playlistIndex - 1;
  playFromPlaylist(app, app.playlist, previous, false);
}

void checkAutomaticNext(AppState &app) {
  if (!app.hasCurrentSong || !app.playbackRunning || app.paused)
    return;
  if (!app.player.isFinished())
    return;

  if (app.shuffle && app.playlist.size() > 1) {
    playFromPlaylist(app, app.playlist, pickNextIndex(app), false);
  } else if (app.playlistIndex + 1 < app.playlist.size()) {
    playFromPlaylist(app, app.playlist, app.playlistIndex + 1, false);
  } else {
    app.playbackRunning = false;
  }
}

void togglePause(AppState &app) {
  if (!app.hasCurrentSong) {
    playFolder(app, app.musicRoot, true);
    return;
  }
  if (app.paused) {
    app.player.resume();
    app.paused = false;
    app.playbackRunning = true;
  } else {
    app.player.pause();
    app.paused = true;
  }
}

void seekLeft(AppState &app) {
  if (!app.hasCurrentSong)
    return;
  app.player.seekBackward(5.0F);
}

void seekRight(AppState &app) {
  if (!app.hasCurrentSong)
    return;
  app.player.seekForward(5.0F);
}

std::vector<std::string> queueAsList(const queue &songQueue) {
  std::vector<std::string> songs;
  for (std::size_t i = 0; i < songQueue.size(); ++i)
    songs.push_back(songQueue.song(i));
  return songs;
}

void addToQueue(AppState &app, const fs::path &path) {
  std::error_code ec;
  if (fs::is_directory(path, ec)) {
    auto songs = audioFilesIn(path, true);
    if (app.shuffle)
      std::shuffle(songs.begin(), songs.end(), app.rng);
    for (const auto &song : songs)
      app.songQueue.add(song);
  } else if (isAudioFile(path)) {
    app.songQueue.add(fs::absolute(path).lexically_normal().string());
  }
}

void askSearchQuery(AppState &app) {
  clearTerminalCover();
  disableRawMode();
  std::cout << "\033[?7h\033[?25h\033[2J\033[H";
  std::cout << kPurpleBold << "Search songs by filename: " << kReset;
  std::getline(std::cin, app.searchQuery);

  app.searchResults = searchAudioFiles(app.musicRoot, app.searchQuery);
  app.searchCursor = selection{};
  app.tab = AppTab::Search;

  std::cout << "\033[?7l\033[?25l";
  enableRawMode();
}

std::size_t visualLength(const std::string &text) {
  std::size_t length = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80)
      ++length;
  }
  return length;
}

void showTabs(AppTab active) {
  std::cout << kPurpleBold << "MUZX  " << kReset;
  const char *names[] = {"1 Player", "2 Folders", "3 Queue", "4 Search",
                         "5 Keys"};

  for (int i = 0; i < 5; ++i) {
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

  constexpr std::size_t visibleRows = 14;
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
  // Row 4 (1-indexed): PLAYER header, followed by 1 empty line (Row 5)
  std::cout << kPurpleBold << "PLAYER" << kReset << "\n\n";

  // Rows 6..15 (1-indexed): 10 rows reserved for Album Art
  if (app.showAlbumArt && app.hasCurrentSong) {
    renderPlayerCover(app.currentSong, app.showAlbumArt,
                      app.tab == AppTab::Player);
  }

  if (!app.hasCurrentSong) {
    std::cout << kMuted
              << "  No track loaded. Press Space or P to play all folders.\n"
              << kReset;
  } else {
    float duration = app.player.getDuration().asSeconds();
    float position = app.player.getPlayingOffset().asSeconds();
    if (position < 0)
      position = 0;
    if (duration > 0 && position > duration)
      position = duration;

    int percent =
        duration > 0 ? static_cast<int>((position / duration) * 100) : 0;

    std::cout << "  Song:    " << kPink << fileName(app.currentSong) << kReset
              << "\n";
    std::cout << "  Folder:  " << kMuted
              << fs::path(app.currentSong).parent_path().string() << kReset
              << "\n";

    if (app.paused)
      std::cout << "  Status:  " << kMuted << "Paused" << kReset << "\n";
    else if (app.playbackRunning)
      std::cout << "  Status:  " << kGreenBold << "Playing" << kReset << "\n";
    else
      std::cout << "  Status:  Stopped / Finished\n";

    constexpr int width = 30;
    int filled =
        duration > 0
            ? static_cast<int>((static_cast<float>(percent) / 100) * width)
            : 0;

    std::cout << "  [";
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

    std::cout << "  Time:    " << formatTime(position) << " / "
              << formatTime(duration)
              << "   |   Volume: " << app.player.getVolume() << "%\n";
    std::cout << "  Shuffle: "
              << (app.shuffle ? std::string(kGreenBold) + "ON"
                              : std::string(kMuted) + "OFF")
              << kReset;
    if (!app.playlist.empty())
      std::cout << "          |   Track:  " << (app.playlistIndex + 1) << " / "
                << app.playlist.size();
    std::cout << "\n";
  }

  std::cout << kMuted
            << "  Space Play/Pause  |  ←/→ Seek 5s  |  h/l Prev/Next  |  s "
               "Shuffle  |  i Art ("
            << (app.showAlbumArt ? "ON" : "OFF") << ")\n"
            << kReset;
}

void drawFolders(const AppState &app) {
  std::cout << kPurpleBold << "FOLDERS" << kReset << "\n";
  std::cout << "  Path: " << kPink << app.browseFolder.string() << kReset
            << "\n";
  std::cout << kMuted
            << "  j/k Move  |  l/Enter Open/Play  |  Backspace Parent  |  p "
               "Play Folder  |  e Queue\n\n"
            << kReset;

  std::vector<std::string> labels;
  if (!app.folderEntries.empty()) {
    labels.push_back(std::string(kPink) + "[PLAY ALL] " + kReset +
                     "Play all songs in this folder");
  }

  for (const auto &entry : app.folderEntries) {
    std::error_code ec;
    if (fs::is_directory(entry, ec))
      labels.push_back(std::string(kMuted) + "[DIR]      " + kReset +
                       entry.filename().string());
    else
      labels.push_back("[SONG]     " + entry.filename().string());
  }
  drawList(labels, app.folderCursor.current());
}

void drawQueue(const AppState &app) {
  std::cout << kPurpleBold << "QUEUE" << kReset << "  (" << app.songQueue.size()
            << " songs)\n";
  std::cout << kMuted
            << "  j/k Move  |  Enter Play Selected  |  r Remove Selected  |  c "
               "Clear Queue\n\n"
            << kReset;

  std::vector<std::string> labels;
  for (std::size_t i = 0; i < app.songQueue.size(); ++i)
    labels.push_back(fileName(app.songQueue.song(i)));
  drawList(labels, app.queueCursor.current());
}

void drawSearch(const AppState &app) {
  std::cout << kPurpleBold << "SEARCH" << kReset << "\n";
  std::cout << "  Query: " << kPink
            << (app.searchQuery.empty() ? "(press / to search)"
                                        : app.searchQuery)
            << kReset << "\n";
  std::cout << kMuted
            << "  / Search  |  j/k Navigate Results  |  Enter Play Selected  | "
               " e Add to Queue\n\n"
            << kReset;

  std::vector<std::string> labels;
  for (const auto &path : app.searchResults)
    labels.push_back(path.filename().string() + kMuted + "  —  " +
                     path.parent_path().string() + kReset);
  drawList(labels, app.searchCursor.current());
}

void drawKeybindings() {
  constexpr std::size_t kColumnWidth = 20;

  auto section = [](const char *title) {
    std::cout << "  " << kMuted << title << kReset << "\n";
  };

  auto row = [](const std::string &key, const char *action) {
    const std::size_t len = visualLength(key);
    const std::size_t pad = len < kColumnWidth ? kColumnWidth - len : 1;
    std::cout << "    " << kPink << key << kReset << std::string(pad, ' ')
              << action << "\n";
  };

  std::cout << kPurpleBold << "KEYBINDINGS" << kReset << "\n\n";

  section("PLAYBACK");
  row("Space", "Play / pause");
  row("← / →  or  a / d", "Seek 5 seconds backward / forward");
  row("h / l", "Previous / next song on Player");
  row("n / b", "Next / previous song (global)");
  row("s", "Toggle shuffle");
  row("i", "Toggle album art (default hidden)");
  row("+ / -", "Adjust volume");
  row("x", "Stop playback");

  std::cout << "\n";
  section("NAVIGATION & FOLDERS");
  row("Tab / Shift+Tab", "Change tabs (or 1-5)");
  row("j / k  or  ↑ / ↓", "Navigate lists");
  row("l / Enter", "Open folder / play selected song");
  row("Backspace / h", "Parent folder");
  row("p / P", "Play selected folder / Play all folders");

  std::cout << "\n";
  section("QUEUE & SEARCH");
  row("e", "Add song or folder to Queue");
  row("r", "Remove selected Queue item");
  row("c", "Clear Queue");
  row("/", "Search songs by filename");
  row("Enter", "Play selected Search / Queue item");
  row("q / Esc", "Quit MUZX");
}

void drawScreen(const AppState &app) {
  std::ostringstream frame;
  std::streambuf *terminalBuffer = std::cout.rdbuf(frame.rdbuf());

  // Row 1: Title, Row 2: Tabs, Row 3: Separator
  std::cout << kPurpleBold << "M U Z X" << kReset
            << "  |  Terminal Music Player\n";
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
  case AppTab::Keybindings:
    drawKeybindings();
    break;
  }

  std::cout << kMuted
            << "------------------------------------------------------------\n"
            << "  Tab/Shift+Tab Tabs  |  Space Play/Pause  |  i Art Toggle  |  "
               "q/Esc Quit"
            << kReset;
  std::cout.flush();
  std::cout.rdbuf(terminalBuffer);

  winsize w{};
  int maxRows = 24;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 1) {
    maxRows = static_cast<int>(w.ws_row);
  }

  const std::string content = frame.str();
  // Begin synchronized update (\033[?2026h) and disable line wrapping
  // (\033[?7l)
  std::string output = "\033[?2026h\033[?7l";

  // Position every row explicitly via \033[<row>;1H so no '\n' ever scrolls the
  // terminal
  std::size_t start = 0;
  int row = 1;
  while (start < content.size() && row <= maxRows) {
    std::size_t end = content.find('\n', start);
    output += "\033[" + std::to_string(row) + ";1H";
    if (end == std::string::npos) {
      output.append(content, start, std::string::npos);
      output += "\033[K";
      ++row;
      break;
    }
    output.append(content, start, end - start);
    output += "\033[K";
    start = end + 1;
    ++row;
  }

  // Clear remaining rows below without scrolling
  while (row <= maxRows) {
    output += "\033[" + std::to_string(row) + ";1H\033[K";
    ++row;
  }

  output += "\033[H";
  std::cout << output << std::flush;

  drawConfirmedKittyCover(app.hasCurrentSong ? app.currentSong : "",
                          app.showAlbumArt, app.tab == AppTab::Player);

  // End synchronized update
  std::cout << "\033[?2026l" << std::flush;
}

} // namespace

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

  TerminalSession terminal;
  bool running = true;

  while (running) {
    checkAutomaticNext(app);
    drawScreen(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int key = readKey();
    if (key == KeyNone)
      continue;

    if (key >= '1' && key <= '5') {
      app.tab = static_cast<AppTab>(key - '1');
      continue;
    }
    if (key == '\t') {
      app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 1) % 5);
      continue;
    }
    if (key == KeyShiftTab) {
      app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 4) % 5);
      continue;
    }

    if (key == 'q' || key == KeyEscape) {
      running = false;
      continue;
    }
    if (key == ' ') {
      togglePause(app);
      continue;
    }
    if (key == 'P') {
      playFolder(app, app.musicRoot, true);
      continue;
    }
    if (key == 's') {
      app.shuffle = !app.shuffle;
      continue;
    }
    if (key == 'i') {
      app.showAlbumArt = !app.showAlbumArt;
      continue;
    }
    if (key == 'n' || (app.tab == AppTab::Player && key == 'l')) {
      nextSong(app);
      continue;
    }
    if (key == 'b' || (app.tab == AppTab::Player && key == 'h')) {
      previousSong(app);
      continue;
    }
    if (key == KeyLeft || key == 'a') {
      seekLeft(app);
      continue;
    }
    if (key == KeyRight || key == 'd') {
      seekRight(app);
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
    if (key == '/') {
      askSearchQuery(app);
      continue;
    }

    if (key == 'j' || key == KeyDown) {
      if (app.tab == AppTab::Folders && !app.folderEntries.empty())
        app.folderCursor.moveDown(folderMenuSize(app));
      else if (app.tab == AppTab::Queue && !app.songQueue.empty())
        app.queueCursor.moveDown(app.songQueue.size());
      else if (app.tab == AppTab::Search && !app.searchResults.empty())
        app.searchCursor.moveDown(app.searchResults.size());
      continue;
    }
    if (key == 'k' || key == KeyUp) {
      if (app.tab == AppTab::Folders && !app.folderEntries.empty())
        app.folderCursor.moveUp(folderMenuSize(app));
      else if (app.tab == AppTab::Queue && !app.songQueue.empty())
        app.queueCursor.moveUp(app.songQueue.size());
      else if (app.tab == AppTab::Search && !app.searchResults.empty())
        app.searchCursor.moveUp(app.searchResults.size());
      continue;
    }

    if (app.tab == AppTab::Folders) {
      if (key == KeyBackspace || key == 'h') {
        if (app.browseFolder != app.musicRoot) {
          app.browseFolder = app.browseFolder.parent_path();
          app.folderEntries = directoryEntries(app.browseFolder);
          app.folderCursor = selection{};
        }
      } else if (key == 'p') {
        if (!app.folderEntries.empty()) {
          std::size_t index = app.folderCursor.current();
          if (index > 0 && index - 1 < app.folderEntries.size() &&
              fs::is_directory(app.folderEntries[index - 1], ec)) {
            playFolder(app, app.folderEntries[index - 1], true);
          } else {
            playFolder(app, app.browseFolder, true);
          }
        }
      } else if (key == 'e') {
        if (!app.folderEntries.empty()) {
          std::size_t index = app.folderCursor.current();
          if (index == 0) {
            addToQueue(app, app.browseFolder);
          } else if (index - 1 < app.folderEntries.size()) {
            addToQueue(app, app.folderEntries[index - 1]);
          }
        }
      } else if (key == KeyEnter || key == 'l') {
        if (!app.folderEntries.empty()) {
          std::size_t index = app.folderCursor.current();
          if (index == 0) {
            playFolder(app, app.browseFolder, true);
          } else if (index - 1 < app.folderEntries.size()) {
            fs::path chosen = app.folderEntries[index - 1];
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
    } else if (app.tab == AppTab::Queue) {
      if ((key == 'r' || key == 'd') && !app.songQueue.empty()) {
        std::size_t index = app.queueCursor.current();
        if (index < app.songQueue.size())
          app.songQueue.remove(index);
        app.queueCursor = selection{};
      } else if (key == 'c') {
        app.songQueue.clear();
        app.queueCursor = selection{};
      } else if ((key == KeyEnter || key == 'l') && !app.songQueue.empty()) {
        std::size_t index = app.queueCursor.current();
        if (index < app.songQueue.size())
          playFromPlaylist(app, queueAsList(app.songQueue), index);
      }
    } else if (app.tab == AppTab::Search) {
      if (key == 'e' && !app.searchResults.empty()) {
        std::size_t index = app.searchCursor.current();
        if (index < app.searchResults.size())
          addToQueue(app, app.searchResults[index]);
      } else if (key == KeyEnter && !app.searchResults.empty()) {
        std::size_t index = app.searchCursor.current();
        if (index < app.searchResults.size())
          playSongFromFolder(app, app.searchResults[index]);
      }
    } else if (app.tab == AppTab::Player) {
      if (key == 'e' && app.hasCurrentSong)
        addToQueue(app, app.currentSong);
    }

    app.songQueue.save(queueFile.string());
  }

  clearTerminalCover();
  app.player.stop();
  app.songQueue.save(queueFile.string());
  return 0;
}
