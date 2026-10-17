#include "audio_player.hpp"
#include "config.hpp"
#include "cover.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <signal.h>
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
constexpr const char *kCyanBold = "\033[1;36m";

constexpr int kTabCount = 6;
constexpr int kFrameMs = 16;
constexpr int kEscapeTimeoutMs = 25;
// 1-based terminal column where the visualizer starts when album art is shown
// (the cover is 20 columns wide, starting at column 3).
constexpr int kVisColumnWithArt = 28;
constexpr std::size_t kVisBars = audio_player::kBandCount;

enum Key : int {
  KeyNone = 0,
  KeyEscape = 256,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyShiftTab,
  KeyEnter,
  KeyBackspace,
  KeyUnknown // recognised-but-ignored input (Delete, Alt+x, F-keys, ...)
};

termios originalTerminal{};
bool rawModeEnabled = false;
bool inputClosed = false;
volatile std::sig_atomic_t quitSignal = 0;

// ---------------------------------------------------------------------------
// Signals and terminal handling
// ---------------------------------------------------------------------------

void onSignal(int) { quitSignal = 1; }

void installSignalHandlers() {
  struct sigaction sa{};
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
}

void enableRawMode() {
  if (rawModeEnabled)
    return;
  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return;

  termios raw = originalTerminal;
  raw.c_iflag &= static_cast<tcflag_t>(~IXON); // Ctrl+S must not freeze the UI
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
    std::cout << "\033[?1049h\033[?25l\033[?7l\033[2J\033[H" << std::flush;
  }
  ~TerminalSession() {
    clearTerminalCover();
    disableRawMode();
    std::cout << "\033[?7h\033[?25h\033[?1049l" << std::flush;
  }
  TerminalSession(const TerminalSession &) = delete;
  TerminalSession &operator=(const TerminalSession &) = delete;
};

// Redirects std::cout into a string for the lifetime of the object.
class CoutCapture {
public:
  CoutCapture() : old_(std::cout.rdbuf(buffer_.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(old_); }
  CoutCapture(const CoutCapture &) = delete;
  CoutCapture &operator=(const CoutCapture &) = delete;
  std::string str() const { return buffer_.str(); }

private:
  std::ostringstream buffer_;
  std::streambuf *old_;
};

int terminalRows() {
  winsize w{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 1)
    return static_cast<int>(w.ws_row);
  return 24;
}

// ---------------------------------------------------------------------------
// Keyboard input
// ---------------------------------------------------------------------------

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
  const ssize_t n = ::read(STDIN_FILENO, &value, 1);
  if (n == 0) // readable but nothing to read: stdin was closed / hung up
    inputClosed = true;
  return n == 1;
}

int arrowKey(char finalByte) {
  switch (finalByte) {
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
    return KeyUnknown;
  }
}

// Consumes an OSC / APC / DCS string up to its terminator (BEL or ESC \).
void skipStringSequence() {
  char c = '\0';
  for (int i = 0; i < 512 && readByte(c, kEscapeTimeoutMs); ++i) {
    if (c == '\a')
      return;
    if (c == '\033') {
      readByte(c, kEscapeTimeoutMs);
      return;
    }
  }
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
  if (!readByte(second, kEscapeTimeoutMs))
    return KeyEscape; // a lone Esc press

  if (second == '[') {
    // CSI: parameters, then one final byte in 0x40..0x7E. Always consume the
    // whole sequence so its tail is never mistaken for key presses.
    char c = '\0';
    for (int i = 0; i < 16 && readByte(c, kEscapeTimeoutMs); ++i) {
      if (c >= 0x40 && c <= 0x7E)
        return arrowKey(c);
    }
    return KeyUnknown;
  }
  if (second == 'O') { // application-mode arrows: ESC O A
    char c = '\0';
    return readByte(c, kEscapeTimeoutMs) ? arrowKey(c) : KeyUnknown;
  }
  if (second == '_' || second == ']' || second == 'P') {
    skipStringSequence();
    return KeyUnknown;
  }
  return KeyUnknown; // Alt+key
}

// ---------------------------------------------------------------------------
// Strings and filesystem helpers
// ---------------------------------------------------------------------------

std::string lowerCase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool isAudioFile(const fs::path &path) {
  const std::string ext = lowerCase(path.extension().string());
  return ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" ||
         ext == ".aiff" || ext == ".au" || ext == ".m4a" || ext == ".opus" ||
         ext == ".aac";
}

// Turns user/config input into a clean absolute path without a trailing slash
// (a trailing slash made "is this the music root?" comparisons fail, which
// let Backspace walk above the music folder).
fs::path normalizeFolder(std::string text) {
  text = trim(text);
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
      text.back() == text.front())
    text = trim(text.substr(1, text.size() - 2));
  if (text.empty())
    return {};

  if (text[0] == '~' && (text.size() == 1 || text[1] == '/')) {
    if (const char *home = std::getenv("HOME"); home && *home)
      text = std::string(home) + text.substr(1);
  }

  std::error_code ec;
  fs::path path = fs::absolute(fs::path(text), ec);
  if (ec)
    return {};
  path = path.lexically_normal();
  if (!path.has_filename() && path.has_parent_path())
    path = path.parent_path();
  return path;
}

std::vector<fs::path> directoryEntries(const fs::path &folder) {
  struct Item {
    fs::path path;
    std::string sortKey;
    bool isDir;
  };
  std::vector<Item> items;

  std::error_code ec;
  fs::directory_iterator it(folder, ec);
  fs::directory_iterator end;

  while (!ec && it != end) {
    std::error_code entryEc;
    const bool isDir = it->is_directory(entryEc);
    if (isDir || isAudioFile(it->path())) {
      fs::path full = fs::absolute(it->path(), entryEc).lexically_normal();
      std::string key = lowerCase(full.filename().string());
      items.push_back({std::move(full), std::move(key), isDir});
    }
    it.increment(ec);
  }

  // Sort keys are computed once; the old comparator hit the disk on every
  // comparison.
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    if (a.isDir != b.isDir)
      return a.isDir;
    if (a.sortKey != b.sortKey)
      return a.sortKey < b.sortKey;
    return a.path < b.path;
  });

  std::vector<fs::path> entries;
  entries.reserve(items.size());
  for (auto &item : items)
    entries.push_back(std::move(item.path));
  return entries;
}

template <typename Iterator>
void collectAudioFiles(Iterator it, std::error_code &ec,
                       std::vector<std::pair<std::string, std::string>> &out) {
  Iterator end;
  while (!ec && it != end) {
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && isAudioFile(it->path())) {
      const fs::path absolute =
          fs::absolute(it->path(), entryEc).lexically_normal();
      out.emplace_back(lowerCase(absolute.filename().string()),
                       absolute.string());
    }
    it.increment(ec);
  }
}

std::vector<std::string> audioFilesIn(const fs::path &folder,
                                      bool recursive = false) {
  std::vector<std::pair<std::string, std::string>> keyed; // {lower name, path}
  std::error_code ec;

  if (recursive) {
    collectAudioFiles(
        fs::recursive_directory_iterator(
            folder, fs::directory_options::skip_permission_denied, ec),
        ec, keyed);
  } else {
    collectAudioFiles(fs::directory_iterator(folder, ec), ec, keyed);
  }

  // Name first (as before), full path as tie-breaker so the order is stable.
  std::sort(keyed.begin(), keyed.end());

  std::vector<std::string> songs;
  songs.reserve(keyed.size());
  for (auto &entry : keyed)
    songs.push_back(std::move(entry.second));
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
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && isAudioFile(it->path())) {
      const std::string filename = lowerCase(it->path().filename().string());
      if (filename.find(wanted) != std::string::npos)
        results.push_back(fs::absolute(it->path(), entryEc).lexically_normal());
    }
    it.increment(ec);
  }

  std::sort(results.begin(), results.end());
  return results;
}

std::string fileName(const std::string &path) {
  if (path.empty())
    return "";
  return fs::path(path).filename().string();
}

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

enum class AppTab {
  Player = 0,
  Folders = 1,
  Queue = 2,
  Search = 3,
  Status = 4,
  Keybindings = 5
};

struct AppState {
  audio_player player;
  queue songQueue;
  AppTab tab = AppTab::Player;
  fs::path musicRoot;
  fs::path browseFolder;
  fs::path statsFile;
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
  bool showVisualizer = false;

  double sessionPlayedSeconds = 0.0;
  double totalPlayedSeconds = 0.0;
  bool statsDirty = false;
  std::map<std::string, int> playCounts;
  std::array<float, kVisBars> visHeights{};
  std::array<float, kVisBars> visPeaks{};

  std::mt19937 rng{std::random_device{}()};
};

void loadStats(AppState &app) {
  std::ifstream in(app.statsFile);
  if (!in)
    return;

  std::string line;
  if (std::getline(in, line)) {
    try {
      app.totalPlayedSeconds = std::stod(line);
    } catch (...) {
      app.totalPlayedSeconds = 0.0;
    }
  }

  while (std::getline(in, line)) {
    auto tabPos = line.find('\t');
    if (tabPos != std::string::npos) {
      try {
        int count = std::stoi(line.substr(0, tabPos));
        std::string songPath = line.substr(tabPos + 1);
        if (!songPath.empty() && count > 0)
          app.playCounts[songPath] = count;
      } catch (...) {
      }
    }
  }
}

// Writes to a temp file and renames it over the real one, so a crash or full
// disk can no longer leave a truncated stats file behind.
void saveStats(AppState &app) {
  if (app.statsFile.empty())
    return;

  fs::path tmp = app.statsFile;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      return;
    out << static_cast<long long>(app.totalPlayedSeconds) << "\n";
    for (const auto &[songPath, count] : app.playCounts)
      out << count << "\t" << songPath << "\n";
    out.flush();
    if (!out) {
      std::error_code removeEc;
      fs::remove(tmp, removeEc);
      return;
    }
  }

  std::error_code ec;
  fs::rename(tmp, app.statsFile, ec);
  if (!ec)
    app.statsDirty = false;
}

std::string formatDurationHMS(double totalSeconds) {
  long long secs = static_cast<long long>(std::max(0.0, totalSeconds));
  long long hours = secs / 3600;
  long long minutes = (secs % 3600) / 60;
  long long remSecs = secs % 60;

  std::ostringstream oss;
  oss << hours << "h " << std::setw(2) << std::setfill('0') << minutes << "m "
      << std::setw(2) << std::setfill('0') << remSecs << "s";
  return oss.str();
}

std::string currentPcTimeString() {
  std::time_t now = std::time(nullptr);
  std::tm localTm{};
  localtime_r(&now, &localTm);
  char buf[64]{};
  std::strftime(buf, sizeof(buf), "%I:%M:%S %p  |  %a, %d %b %Y", &localTm);
  return std::string(buf);
}

void updateVisualizerAndStats(AppState &app, double dtSeconds) {
  const bool activelyPlaying =
      app.hasCurrentSong && app.playbackRunning && !app.paused;

  if (activelyPlaying) {
    app.sessionPlayedSeconds += dtSeconds;
    app.totalPlayedSeconds += dtSeconds;
    app.statsDirty = true;

    const auto realBands = app.player.getSpectrumBands();
    const float riseAlpha =
        std::clamp(static_cast<float>(dtSeconds) * 28.0F, 0.1F, 0.85F);
    const float fallAlpha =
        std::clamp(static_cast<float>(dtSeconds) * 10.0F, 0.05F, 0.45F);

    for (std::size_t i = 0; i < kVisBars; ++i) {
      // A single NaN/inf band would otherwise poison the bar forever.
      const float target = std::isfinite(realBands[i])
                               ? std::clamp(realBands[i], 0.0F, 1.0F)
                               : 0.0F;
      if (target > app.visHeights[i])
        app.visHeights[i] += (target - app.visHeights[i]) * riseAlpha;
      else
        app.visHeights[i] += (target - app.visHeights[i]) * fallAlpha;

      if (app.visHeights[i] >= app.visPeaks[i])
        app.visPeaks[i] = app.visHeights[i];
      else
        app.visPeaks[i] = std::max(
            0.0F, app.visPeaks[i] - static_cast<float>(dtSeconds) * 0.45F);
    }
  } else {
    for (std::size_t i = 0; i < kVisBars; ++i) {
      app.visHeights[i] *= 0.80F;
      app.visPeaks[i] *= 0.85F;
      if (app.visHeights[i] < 0.01F)
        app.visHeights[i] = 0.0F;
      if (app.visPeaks[i] < 0.01F)
        app.visPeaks[i] = 0.0F;
    }
  }
}

// ---------------------------------------------------------------------------
// Playback control
// ---------------------------------------------------------------------------

std::size_t folderMenuSize(const AppState &app) {
  return app.folderEntries.empty() ? 0 : app.folderEntries.size() + 1;
}

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index, bool switchToPlayer = true) {
  if (songs.empty() || index >= songs.size())
    return false;

  // `songs` may alias app.playlist, so work on a copy.
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
        app.playCounts[path]++;
        app.statsDirty = true;
        saveStats(app);
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
  if (!app.playbackRunning && !app.paused) {
    // Stopped with 'x' or finished: Space restarts the track. Previously it
    // "paused" a stream that was not playing and nothing happened.
    if (!app.playlist.empty())
      playFromPlaylist(app, app.playlist, app.playlistIndex, false);
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
  app.player.seekBackward(2.5F);
}

void seekRight(AppState &app) {
  if (!app.hasCurrentSong)
    return;
  app.player.seekForward(2.5F);
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

void openFolder(AppState &app, const fs::path &folder) {
  app.browseFolder = folder;
  app.folderEntries = directoryEntries(folder);
  app.folderCursor = selection{};
}

void askSearchQuery(AppState &app) {
  clearTerminalCover();
  disableRawMode();
  std::cout << "\033[?7h\033[?25h\033[2J\033[H";
  std::cout << kPurpleBold << "Search songs by filename: " << kReset
            << std::flush;

  std::string query;
  if (!std::getline(std::cin, query)) {
    // Ctrl+D / interrupted read: treat as an empty query and keep cin usable.
    std::cin.clear();
    query.clear();
  }
  app.searchQuery = trim(query);

  app.searchResults = searchAudioFiles(app.musicRoot, app.searchQuery);
  app.searchCursor = selection{};
  app.tab = AppTab::Search;

  std::cout << "\033[?7l\033[?25l" << std::flush;
  enableRawMode();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

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
  const char *names[] = {"1 Player", "2 Folders", "3 Queue",
                         "4 Search", "5 Status",  "6 Keys"};

  for (int i = 0; i < kTabCount; ++i) {
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

  // 14 rows on normal terminals; fewer on short ones so the footer is not
  // pushed off the bottom of the screen.
  const std::size_t visibleRows =
      static_cast<std::size_t>(std::clamp(terminalRows() - 11, 3, 14));
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

const char *subBlockGlyph(int eighths) {
  static constexpr const char *kBlocks[9] = {"  ", "▁▁", "▂▂", "▃▃", "▄▄",
                                             "▅▅", "▆▆", "▇▇", "██"};
  return kBlocks[std::clamp(eighths, 0, 8)];
}

void drawPlayerCoverAndVisualizer(const AppState &app) {
  if (app.tab != AppTab::Player || (!app.showAlbumArt && !app.showVisualizer))
    return;

  constexpr int kTotalRows = 10;
  constexpr int kBarRows = 9;

  const bool showArt = app.showAlbumArt && app.hasCurrentSong;

  std::vector<std::string> coverLines;
  if (showArt) {
    std::string captured;
    {
      CoutCapture capture;
      renderPlayerCover(app.currentSong, app.showAlbumArt, true);
      captured = capture.str();
    }
    std::istringstream iss(captured);
    std::string line;
    while (std::getline(iss, line))
      coverLines.push_back(line);
  }
  if (coverLines.size() < static_cast<std::size_t>(kTotalRows))
    coverLines.resize(static_cast<std::size_t>(kTotalRows));

  for (int r = 0; r < kTotalRows; ++r) {
    if (showArt) {
      // Jump to a fixed column so the bars never sit underneath the cover.
      std::cout << coverLines[static_cast<std::size_t>(r)] << "\033["
                << kVisColumnWithArt << "G";
    } else {
      std::cout << "  ";
    }

    if (app.showVisualizer) {
      if (r < kBarRows) {
        const int rowFromBottom = (kBarRows - 1) - r;
        const char *barColor =
            (rowFromBottom >= 6)
                ? kPink
                : ((rowFromBottom >= 3) ? kPurpleBold : kCyanBold);

        std::cout << barColor;
        for (std::size_t b = 0; b < kVisBars; ++b) {
          const float scaled = app.visHeights[b] * static_cast<float>(kBarRows);
          const float rowFill = std::clamp(
              scaled - static_cast<float>(rowFromBottom), 0.0F, 1.0F);
          const int eighths = static_cast<int>(std::round(rowFill * 8.0F));

          const int peakRow = static_cast<int>(
              app.visPeaks[b] * static_cast<float>(kBarRows - 1));
          if (eighths == 0 && peakRow == rowFromBottom &&
              app.visPeaks[b] > 0.06F) {
            std::cout << "── ";
          } else {
            std::cout << subBlockGlyph(eighths) << " ";
          }
        }
        std::cout << kReset << "\n";
      } else {
        std::cout << kMuted;
        for (std::size_t b = 0; b < kVisBars; ++b)
          std::cout << "══ ";
        std::cout << kReset << "\n";
      }
    } else {
      std::cout << "\n";
    }
  }
}

void drawPlayer(const AppState &app) {
  std::cout << kPurpleBold << "PLAYER" << kReset << "\n\n";

  if ((app.showAlbumArt || app.showVisualizer) && app.hasCurrentSong) {
    drawPlayerCoverAndVisualizer(app);
  }

  if (!app.hasCurrentSong) {
    std::cout << kMuted
              << "  No track loaded. Press Space or P to play all folders.\n"
              << kReset;
  } else {
    float duration = app.player.getDuration().asSeconds();
    float position = app.player.getPlayingOffset().asSeconds();
    if (!std::isfinite(duration) || duration < 0.0F)
      duration = 0.0F;
    if (!std::isfinite(position) || position < 0.0F)
      position = 0.0F;
    if (duration > 0.0F && position > duration)
      position = duration;

    int percent =
        duration > 0.1F ? static_cast<int>((position / duration) * 100.0F) : 0;
    percent = std::clamp(percent, 0, 100);

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
        duration > 0.1F
            ? std::clamp(static_cast<int>((position / duration) * width), 0,
                         width)
            : 0;

    std::cout << "  [";
    for (int i = 0; i < width; ++i)
      std::cout << (i < filled ? '#' : '-');
    std::cout << "] " << percent << "%\n";

    auto formatTime = [](float seconds) {
      int total = std::max(0, static_cast<int>(seconds));
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
              << "   |   Volume: " << static_cast<int>(app.player.getVolume())
              << "%\n";
    // "ON " is padded to the width of "OFF" so the "|" does not jump.
    std::cout << "  Shuffle: "
              << (app.shuffle ? std::string(kGreenBold) + "ON "
                              : std::string(kMuted) + "OFF")
              << kReset;
    if (!app.playlist.empty())
      std::cout << "          |   Track:  " << (app.playlistIndex + 1) << " / "
                << app.playlist.size();
    std::cout << "\n";
  }

  std::cout << kMuted << "  Space Play/Pause  |  ←/→ Seek  |  i Art ("
            << (app.showAlbumArt ? "ON" : "OFF") << ")  |  v Vis ("
            << (app.showVisualizer ? "ON" : "OFF") << ")\n"
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

void drawStatus(const AppState &app) {
  std::cout << kPurpleBold << "STATUS & LISTENING STATS" << kReset << "\n\n";
  std::cout << "  PC Clock:          " << kCyanBold << currentPcTimeString()
            << kReset << "\n";
  std::cout << "  Session Played:    " << kGreenBold
            << formatDurationHMS(app.sessionPlayedSeconds) << kReset << "\n";
  std::cout << "  All-Time Played:   " << kPink
            << formatDurationHMS(app.totalPlayedSeconds) << kReset << "\n";

  std::vector<std::pair<std::string, int>> sortedPlays(app.playCounts.begin(),
                                                       app.playCounts.end());
  // Ties are ordered by name so the list does not shuffle between frames.
  std::sort(sortedPlays.begin(), sortedPlays.end(),
            [](const auto &a, const auto &b) {
              if (a.second != b.second)
                return a.second > b.second;
              return a.first < b.first;
            });

  std::cout << "\n  " << kPurpleBold << "MOST PLAYED TRACKS" << kReset << "\n";
  if (sortedPlays.empty()) {
    std::cout << kMuted << "    No tracks played yet.\n" << kReset;
  } else {
    std::size_t count = std::min<std::size_t>(8, sortedPlays.size());
    for (std::size_t i = 0; i < count; ++i) {
      std::cout << "    " << (i + 1) << ". " << kPink
                << fileName(sortedPlays[i].first) << kReset << kMuted << "  ("
                << sortedPlays[i].second
                << (sortedPlays[i].second == 1 ? " play)" : " plays)") << kReset
                << "\n";
    }
  }
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
  row("← / →  or  a / d", "Smooth seek backward / forward");
  row("h / l", "Previous / next song on Player");
  row("n / b", "Next / previous song (global)");
  row("s", "Toggle shuffle");
  row("i", "Toggle Album Art");
  row("v", "Toggle Audio Visualizer");
  row("+ / -", "Adjust volume");
  row("x", "Stop playback");

  std::cout << "\n";
  section("NAVIGATION & TABS");
  row("Tab / Shift+Tab", "Cycle tabs (or keys 1-6)");
  row("5", "Open Status (Clock, Played Time, Top Songs)");
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
  row("q / Esc", "Quit MUZX");
}

void drawScreen(const AppState &app) {
  std::string content;
  {
    CoutCapture capture;

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
    case AppTab::Status:
      drawStatus(app);
      break;
    case AppTab::Keybindings:
      drawKeybindings();
      break;
    }

    std::cout
        << kMuted
        << "------------------------------------------------------------\n"
        << "  Tab/1-6 Tabs  |  Space Play/Pause  |  i Art  |  v Vis  |  "
           "q/Esc Quit"
        << kReset;
    std::cout.flush();
    content = capture.str();
  }

  const int maxRows = terminalRows();

  std::string output = "\033[?7l\033[H";

  std::size_t start = 0;
  int rowCount = 0;
  while (start < content.size() && rowCount < maxRows) {
    std::size_t end = content.find('\n', start);
    ++rowCount;

    // Erase the whole row first. Rows that only jump the cursor (the cover
    // area) never wrote columns 1-27, so text from older frames stayed there.
    output += "\033[2K";

    if (end == std::string::npos) {
      output.append(content, start, std::string::npos);
      break;
    }

    output.append(content, start, end - start);
    if (rowCount < maxRows) {
      output += "\n";
    }
    start = end + 1;
  }

  output += "\033[J\033[H";
  std::cout << output << std::flush;

  static const std::string kNoSong;
  drawConfirmedKittyCover(app.hasCurrentSong ? app.currentSong : kNoSong,
                          app.showAlbumArt && app.tab == AppTab::Player,
                          app.tab == AppTab::Player);
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

enum class KeyResult { Handled, FlushInput, Quit };

KeyResult handleKey(AppState &app, int key, const fs::path &queueFile) {
  std::error_code ec;

  if (key >= '1' && key <= '6') {
    app.tab = static_cast<AppTab>(key - '1');
    return KeyResult::Handled;
  }
  if (key == '\t') {
    app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 1) % kTabCount);
    return KeyResult::Handled;
  }
  if (key == KeyShiftTab) {
    app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + kTabCount - 1) %
                                  kTabCount);
    return KeyResult::Handled;
  }

  if (key == 'q' || key == KeyEscape)
    return KeyResult::Quit;

  if (key == ' ') {
    togglePause(app);
    return KeyResult::Handled;
  }
  if (key == 'P') {
    playFolder(app, app.musicRoot, true);
    return KeyResult::Handled;
  }
  if (key == 's') {
    app.shuffle = !app.shuffle;
    return KeyResult::Handled;
  }
  if (key == 'i') {
    app.showAlbumArt = !app.showAlbumArt;
    return KeyResult::Handled;
  }
  if (key == 'v') {
    app.showVisualizer = !app.showVisualizer;
    return KeyResult::Handled;
  }
  if (key == 'n' || (app.tab == AppTab::Player && key == 'l')) {
    nextSong(app);
    return KeyResult::Handled;
  }
  if (key == 'b' || (app.tab == AppTab::Player && key == 'h')) {
    previousSong(app);
    return KeyResult::Handled;
  }
  if (key == KeyLeft || key == 'a') {
    seekLeft(app);
    return KeyResult::Handled;
  }
  if (key == KeyRight || key == 'd') {
    seekRight(app);
    return KeyResult::Handled;
  }
  if (key == 'x') {
    app.player.stop();
    app.playbackRunning = false;
    app.paused = false;
    return KeyResult::Handled;
  }
  if (key == '+' || key == '=') {
    app.player.increaseVolume();
    return KeyResult::Handled;
  }
  if (key == '-') {
    app.player.decreaseVolume();
    return KeyResult::Handled;
  }
  if (key == '/') {
    askSearchQuery(app);
    return KeyResult::FlushInput;
  }

  if (key == 'j' || key == KeyDown) {
    if (app.tab == AppTab::Folders && !app.folderEntries.empty())
      app.folderCursor.moveDown(folderMenuSize(app));
    else if (app.tab == AppTab::Queue && !app.songQueue.empty())
      app.queueCursor.moveDown(app.songQueue.size());
    else if (app.tab == AppTab::Search && !app.searchResults.empty())
      app.searchCursor.moveDown(app.searchResults.size());
    return KeyResult::Handled;
  }
  if (key == 'k' || key == KeyUp) {
    if (app.tab == AppTab::Folders && !app.folderEntries.empty())
      app.folderCursor.moveUp(folderMenuSize(app));
    else if (app.tab == AppTab::Queue && !app.songQueue.empty())
      app.queueCursor.moveUp(app.songQueue.size());
    else if (app.tab == AppTab::Search && !app.searchResults.empty())
      app.searchCursor.moveUp(app.searchResults.size());
    return KeyResult::Handled;
  }

  if (app.tab == AppTab::Folders) {
    if (key == KeyBackspace || key == 'h') {
      if (app.browseFolder != app.musicRoot) {
        const fs::path parent = app.browseFolder.parent_path();
        if (!parent.empty() && parent != app.browseFolder)
          openFolder(app, parent);
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
        app.songQueue.save(queueFile.string());
      }
    } else if (key == KeyEnter || key == 'l') {
      if (!app.folderEntries.empty()) {
        std::size_t index = app.folderCursor.current();
        if (index == 0) {
          playFolder(app, app.browseFolder, true);
        } else if (index - 1 < app.folderEntries.size()) {
          const fs::path chosen = app.folderEntries[index - 1];
          if (fs::is_directory(chosen, ec))
            openFolder(app, chosen);
          else
            playSongFromFolder(app, chosen);
        }
      }
    }
  } else if (app.tab == AppTab::Queue) {
    // ('d' used to be listed here too, but it is always taken by seek-right
    // above, so that branch could never run.)
    if (key == 'r' && !app.songQueue.empty()) {
      std::size_t index = app.queueCursor.current();
      if (index < app.songQueue.size())
        app.songQueue.remove(index);
      app.queueCursor = selection{};
      app.songQueue.save(queueFile.string());
    } else if (key == 'c') {
      app.songQueue.clear();
      app.queueCursor = selection{};
      app.songQueue.save(queueFile.string());
    } else if ((key == KeyEnter || key == 'l') && !app.songQueue.empty()) {
      std::size_t index = app.queueCursor.current();
      if (index < app.songQueue.size())
        playFromPlaylist(app, queueAsList(app.songQueue), index);
    }
  } else if (app.tab == AppTab::Search) {
    if (key == 'e' && !app.searchResults.empty()) {
      std::size_t index = app.searchCursor.current();
      if (index < app.searchResults.size()) {
        addToQueue(app, app.searchResults[index]);
        app.songQueue.save(queueFile.string());
      }
    } else if (key == KeyEnter && !app.searchResults.empty()) {
      std::size_t index = app.searchCursor.current();
      if (index < app.searchResults.size())
        playSongFromFolder(app, app.searchResults[index]);
    }
  } else if (app.tab == AppTab::Player) {
    if (key == 'e' && app.hasCurrentSong) {
      addToQueue(app, app.currentSong);
      app.songQueue.save(queueFile.string());
    }
  }

  return KeyResult::Handled;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int run() {
  config settings;
  std::error_code ec;

  fs::path root = normalizeFolder(settings.get_music_folder());
  if (root.empty() || !fs::is_directory(root, ec)) {
    // First run, or the saved folder no longer exists. The old code saved
    // whatever was typed *before* validating it, so one typo broke every
    // later start; now only a valid folder is stored.
    for (;;) {
      std::cout << "Enter your music folder path: ";
      std::string input;
      if (!std::getline(std::cin, input) || trim(input).empty()) {
        std::cerr << "No music folder provided.\n";
        return 1;
      }
      root = normalizeFolder(input);
      if (!root.empty() && fs::is_directory(root, ec)) {
        settings.save_music_folder(root.string());
        break;
      }
      std::cerr << "Invalid music folder: " << trim(input) << "\n";
    }
  }

  AppState app;
  app.musicRoot = root;
  app.browseFolder = root;
  app.folderEntries = directoryEntries(root);

  const char *home = std::getenv("HOME");
  fs::path configDirectory = (home && *home)
                                 ? fs::path(home) / ".config" / "muzx"
                                 : fs::current_path(ec) / ".muzx";
  fs::create_directories(configDirectory, ec);
  const fs::path queueFile = configDirectory / "queue.txt";
  app.statsFile = configDirectory / "stats.txt";

  app.songQueue.load(queueFile.string());
  loadStats(app);

  installSignalHandlers();
  TerminalSession terminal;
  bool running = true;
  auto lastTick = std::chrono::steady_clock::now();
  double statsSaveAccumulator = 0.0;

  while (running && !quitSignal && !inputClosed) {
    const auto now = std::chrono::steady_clock::now();
    // Clamp so a stall (laptop sleep, SIGSTOP, long prompt) cannot add hours
    // of "listening time" in a single frame.
    const double dt = std::clamp(
        std::chrono::duration<double>(now - lastTick).count(), 0.0, 0.25);
    lastTick = now;

    app.player.update();
    checkAutomaticNext(app);
    updateVisualizerAndStats(app, dt);

    statsSaveAccumulator += dt;
    if (statsSaveAccumulator >= 5.0) {
      if (app.statsDirty)
        saveStats(app);
      statsSaveAccumulator = 0.0;
    }

    drawScreen(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));

    int key = KeyNone;
    while ((key = readKey()) != KeyNone) {
      if (key == KeyUnknown)
        continue;
      const KeyResult result = handleKey(app, key, queueFile);
      if (result == KeyResult::Quit) {
        running = false;
        break;
      }
      if (result == KeyResult::FlushInput)
        break;
    }
  }

  app.player.stop();
  app.songQueue.save(queueFile.string());
  saveStats(app);
  return 0;
}

} // namespace

int main() {
  // Any exception unwinds through run(), so TerminalSession always restores
  // the terminal before the message is printed.
  try {
    return run();
  } catch (const std::exception &e) {
    std::cerr << "muzx: fatal error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "muzx: unknown fatal error\n";
  }
  return 1;
}
