#include "audio_player.hpp"
#include "config.hpp"
#include "cover.hpp"
#include "lcd_link.hpp"
#include "lyrics.hpp"
#include "mpris.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

extern char **environ; // for posix_spawn (notifications)

namespace {

constexpr const char *kReset = "\033[0m";
constexpr const char *kSelected = "\033[7m";
// Theme colours: changed at run time by applyTheme() (Settings tab).
const char *kMuted = "\033[90m";
const char *kPurpleBold = "\033[1;35m";
const char *kPink = "\033[95m";
const char *kGreenBold = "\033[1;32m";
const char *kCyanBold = "\033[1;36m";
const char *kLyricCurrent = "\033[1;95m"; // bold accent: the line being sung
constexpr const char *kLyricNear = "\033[37m"; // light grey

struct Theme {
  const char *name, *title, *accent, *green, *cyan, *muted, *lyric;
};
const Theme kThemes[] = {
    {"Purple", "\033[1;35m", "\033[95m", "\033[1;32m", "\033[1;36m", "\033[90m",
     "\033[1;95m"},
    {"Ocean", "\033[1;34m", "\033[96m", "\033[1;36m", "\033[1;94m", "\033[90m",
     "\033[1;96m"},
    {"Forest", "\033[1;32m", "\033[93m", "\033[1;92m", "\033[1;33m", "\033[90m",
     "\033[1;93m"},
    {"Sunset", "\033[1;31m", "\033[93m", "\033[1;33m", "\033[1;91m", "\033[90m",
     "\033[1;93m"},
    {"Mono", "\033[1;37m", "\033[97m", "\033[1;37m", "\033[37m", "\033[90m",
     "\033[1;97m"},
    {"Custom", "", "", "", "", "\033[90m", ""}}; // filled in from g_custom
constexpr int kThemeCount = 6;
constexpr int kCustomTheme = 5;

// Colours the theme editor can pick from (256-colour codes).
struct PaletteColor {
  const char *name;
  int code;
};
const PaletteColor kPalette[] = {
    {"Red", 196},   {"Orange", 208}, {"Yellow", 220},  {"Lime", 118},
    {"Green", 40},  {"Teal", 37},    {"Cyan", 51},     {"Sky", 39},
    {"Blue", 33},   {"Purple", 135}, {"Magenta", 201}, {"Pink", 213},
    {"White", 255}, {"Grey", 250}};
constexpr int kPaletteCount = 14;
int g_custom[4] = {10, 11, 4, 6}; // title, accent, highlight, bars
std::string g_customCode[5];      // the escape codes built from g_custom

void applyTheme(int index) {
  index = ((index % kThemeCount) + kThemeCount) % kThemeCount;
  if (index == kCustomTheme) {
    auto code = [](int slot, bool bold) {
      const int c =
          kPalette[((g_custom[slot] % kPaletteCount) + kPaletteCount) %
                   kPaletteCount]
              .code;
      return std::string(bold ? "\033[1;38;5;" : "\033[38;5;") +
             std::to_string(c) + "m";
    };
    g_customCode[0] = code(0, true);  // title
    g_customCode[1] = code(1, false); // accent
    g_customCode[2] = code(2, true);  // highlight
    g_customCode[3] = code(3, true);  // bars (low part)
    g_customCode[4] = code(1, true);  // current lyric line
    kPurpleBold = g_customCode[0].c_str();
    kPink = g_customCode[1].c_str();
    kGreenBold = g_customCode[2].c_str();
    kCyanBold = g_customCode[3].c_str();
    kMuted = "\033[90m";
    kLyricCurrent = g_customCode[4].c_str();
    return;
  }
  const Theme &t = kThemes[index];
  kPurpleBold = t.title;
  kPink = t.accent;
  kGreenBold = t.green;
  kCyanBold = t.cyan;
  kMuted = t.muted;
  kLyricCurrent = t.lyric;
}

constexpr int kTabCount = 7;
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
  KeyMouse,  // a mouse event: see lastMouse
  KeyUnknown // recognised-but-ignored input (Delete, Alt+x, F-keys, ...)
};

// Mouse reporting (SGR mode). MUZX_MOUSE=0 turns the mouse off. While it is on,
// hold Shift to select text with the mouse as usual.
struct MouseEvent {
  int button = 0, x = 0, y = 0;
  bool press = false;
};
MouseEvent lastMouse;
bool mouseWanted() {
  const char *env = std::getenv("MUZX_MOUSE");
  return !(env && std::string(env) == "0");
}

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

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
    rawModeEnabled = true;
    if (mouseWanted())
      std::cout << "\033[?1002h\033[?1006h" << std::flush;
  }
}

void disableRawMode() {
  if (!rawModeEnabled)
    return;
  if (mouseWanted())
    std::cout << "\033[?1002l\033[?1006l" << std::flush;
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
  CoutCapture() : old_(std::cout.rdbuf(buffer_.rdbuf())), previous_(current_) {
    current_ = this;
  }
  ~CoutCapture() {
    current_ = previous_;
    std::cout.rdbuf(old_);
  }
  // Number of finished lines captured so far (the newest capture).
  static int rowsSoFar() {
    if (!current_)
      return 0;
    const std::string text = current_->buffer_.str();
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  }
  CoutCapture(const CoutCapture &) = delete;
  CoutCapture &operator=(const CoutCapture &) = delete;
  std::string str() const { return buffer_.str(); }

private:
  static inline CoutCapture *current_ = nullptr;
  std::ostringstream buffer_;
  std::streambuf *old_;
  CoutCapture *previous_;
};

// ---------------------------------------------------------------------------
// Clickable areas. The drawing code registers them while it draws; a mouse
// click is then matched against this list (rows and columns are 1-based).
// ---------------------------------------------------------------------------
enum ClickAction {
  ClkTab,
  ClkShuffle,
  ClkPrev,
  ClkPlayPause,
  ClkNext,
  ClkRepeat,
  ClkSeek,
  ClkListItem,
  ClkSetting,
  ClkVolume
};
struct ClickRegion {
  int row0, row1, col0, col1, action, arg;
};
std::vector<ClickRegion> g_clicks;
// Dragging the progress bar: remembers where the last seek went to, so every
// mouse movement only has to move by the difference.
struct DragState {
  bool active = false;
  double target = 0.0; // seconds
  double duration = 0.0;
  int col0 = 0, col1 = 0;
};
DragState g_drag;
int g_buttonsRow = 0; // screen row of the Player "Status:" line (0 = none)

void addClick(int row0, int row1, int col0, int col1, int action, int arg = 0) {
  g_clicks.push_back({row0, row1, col0, col1, action, arg});
}

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
    std::string params;
    for (int i = 0; i < 24 && readByte(c, kEscapeTimeoutMs); ++i) {
      if (c >= 0x40 && c <= 0x7E) {
        if (!params.empty() && params[0] == '<' && (c == 'M' || c == 'm')) {
          int b = 0, x = 0, y = 0;
          if (std::sscanf(params.c_str() + 1, "%d;%d;%d", &b, &x, &y) == 3) {
            lastMouse = {b, x, y, c == 'M'};
            return KeyMouse;
          }
          return KeyUnknown;
        }
        return arrowKey(c);
      }
      params += c;
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

enum class LyricsMode { Off, Right, Bottom }; // "L" cycles these

enum class RepeatMode { Off, All, One }; // "R" and the loop button

enum class AppTab {
  Player = 0,
  Folders = 1,
  Queue = 2,
  Search = 3,
  Status = 4,
  Keybindings = 5,
  Settings = 6
};

// Shuffle plays every song once in random order, then reshuffles. The history
// lets Previous / Next walk through what was really played.
struct ShuffleState {
  std::vector<std::size_t> history; // playlist indices, oldest first
  std::size_t pos = 0;              // where we are in the history
  std::vector<std::size_t> bag;     // songs not played yet in this round
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
  RepeatMode repeat = RepeatMode::Off;
  bool compact = false;    // Settings: hide the hint lines
  bool showButtons = true; // Settings: the clickable control buttons
  bool notify = false;     // Settings: desktop notification on song change
  bool showAlbumArt = false;
  bool showVisualizer = false;
  LyricsMode lyricsMode = LyricsMode::Right; // "L": Off -> Right -> Bottom
  std::chrono::steady_clock::time_point lyricsSongSince{};
  lyrics::Manager lyricsMgr; // finds lyrics on a background thread
  Mpris mpris;               // desktop media keys / playerctl (D-Bus)
  LcdLink lcd; // "T": show the current lyric line on the Arduino LCD
  // Settings tab
  selection settingsCursor;
  int theme = 0;
  int fontSize = 0; // 0 = leave the terminal's font alone
  fs::path settingsFile;
  std::string settingsSaved; // what is on disk now
  std::string settingsNote;  // message shown at the bottom of the tab
  config *cfg = nullptr;
  std::string lyricsSong; // song the lyrics were last requested for
  // Lyrics timing: seconds the lyrics are shifted LATER (negative = earlier).
  double lyricShift = 0.0;
  std::string lyricShiftSong;                // song lyricShift belongs to
  std::map<std::string, double> lyricShifts; // saved per song
  fs::path lyricShiftFile;
  double volumeBeforeMute = 1.0; // 0..1, for click-to-mute
  // Background lyrics preparation (Settings: "Prepare lyrics")
  int prefetchMode =
      0; // 0 off, 1 look up lyrics of other songs in the background
  std::vector<std::string> library; // every song under musicRoot
  fs::path libraryRoot;
  std::string prefetchKey; // when this changes, the queue is rebuilt

  double sessionPlayedSeconds = 0.0;
  double totalPlayedSeconds = 0.0;
  bool statsDirty = false;
  std::map<std::string, int> playCounts;
  std::array<float, kVisBars> visHeights{};
  std::array<float, kVisBars> visPeaks{};

  std::mt19937 rng{std::random_device{}()};
  ShuffleState shuffleState;
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

// ---------------------------------------------------------------------------
// Shuffle: every song once per round (no repeats), with a history
// ---------------------------------------------------------------------------

void shuffleReset(AppState &app, std::size_t current) {
  ShuffleState &sh = app.shuffleState;
  sh.history.assign(1, current);
  sh.pos = 0;
  sh.bag.clear();
  for (std::size_t i = 0; i < app.playlist.size(); ++i)
    if (i != current)
      sh.bag.push_back(i);
}

// Keeps the shuffle state in step with the song that is really playing (a
// broken file is skipped, so the index can differ from what was asked for).
void shuffleSync(AppState &app, bool newList) {
  ShuffleState &sh = app.shuffleState;
  if (newList || sh.history.empty() || sh.pos >= sh.history.size()) {
    shuffleReset(app, app.playlistIndex);
    return;
  }
  sh.history[sh.pos] = app.playlistIndex;
  sh.bag.erase(std::remove(sh.bag.begin(), sh.bag.end(), app.playlistIndex),
               sh.bag.end());
}

void shuffleEnsure(AppState &app) {
  const ShuffleState &sh = app.shuffleState;
  if (sh.history.empty() || sh.pos >= sh.history.size() ||
      sh.history[sh.pos] != app.playlistIndex)
    shuffleReset(app, app.playlistIndex);
}

std::size_t shuffleNextIndex(AppState &app) {
  const std::size_t count = app.playlist.size();
  if (count <= 1)
    return 0;
  shuffleEnsure(app);
  ShuffleState &sh = app.shuffleState;
  if (sh.pos + 1 <
      sh.history.size()) // we stepped back earlier: go forward again
    return sh.history[++sh.pos];
  if (sh.bag.empty()) { // everything has played: a new round with every song
                        // again
    for (std::size_t i = 0; i < count; ++i)
      sh.bag.push_back(i);
  }
  std::uniform_int_distribution<std::size_t> dist(0, sh.bag.size() - 1);
  std::size_t pick = dist(app.rng);
  if (sh.bag[pick] == app.playlistIndex && sh.bag.size() > 1)
    pick = (pick + 1) % sh.bag.size(); // never the same song twice in a row
  const std::size_t target = sh.bag[pick];
  sh.bag.erase(sh.bag.begin() + static_cast<std::ptrdiff_t>(pick));
  sh.history.push_back(target);
  sh.pos = sh.history.size() - 1;
  constexpr std::size_t kMaxHistory = 500;
  if (sh.history.size() > kMaxHistory) {
    sh.history.erase(sh.history.begin());
    --sh.pos;
  }
  return target;
}

std::size_t shufflePreviousIndex(AppState &app) {
  if (app.playlist.size() <= 1)
    return 0;
  shuffleEnsure(app);
  ShuffleState &sh = app.shuffleState;
  if (sh.pos > 0)
    return sh.history[--sh.pos];
  return app.playlistIndex; // nothing before this song: play it again
}

void setShuffle(AppState &app, bool on) {
  app.shuffle = on;
  if (on && !app.playlist.empty())
    shuffleReset(app,
                 app.playlistIndex); // a fresh round starting with this song
}

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index, bool switchToPlayer = true) {
  if (songs.empty() || index >= songs.size())
    return false;
  const bool newList =
      &songs != &app.playlist; // a different list than the current one

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
        shuffleSync(app, newList);
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
  return shuffleNextIndex(app);
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
  if (app.shuffle)
    previous = shufflePreviousIndex(app); // back through what was really played
  playFromPlaylist(app, app.playlist, previous, false);
}

void checkAutomaticNext(AppState &app) {
  if (!app.hasCurrentSong || !app.playbackRunning || app.paused)
    return;
  if (!app.player.isFinished())
    return;

  if (app.repeat == RepeatMode::One) {
    playFromPlaylist(app, app.playlist, app.playlistIndex, false);
  } else if (app.shuffle && app.playlist.size() > 1) {
    playFromPlaylist(app, app.playlist, pickNextIndex(app), false);
  } else if (app.playlistIndex + 1 < app.playlist.size()) {
    playFromPlaylist(app, app.playlist, app.playlistIndex + 1, false);
  } else if (app.repeat == RepeatMode::All && !app.playlist.empty()) {
    playFromPlaylist(app, app.playlist, 0, false);
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
  const char *names[] = {"1 Player", "2 Folders", "3 Queue",   "4 Search",
                         "5 Status", "6 Keys",    "7 Settings"};

  const int tabRow = CoutCapture::rowsSoFar() + 1;
  int tabCol = 7; // after "MUZX  "
  for (int i = 0; i < kTabCount; ++i) {
    AppTab tab = static_cast<AppTab>(i);
    const int tabWidth =
        static_cast<int>(std::strlen(names[i])) + (tab == active ? 2 : 0);
    addClick(tabRow, tabRow, tabCol, tabCol + tabWidth - 1, ClkTab, i);
    tabCol += tabWidth + 2;
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

  int listRow = CoutCapture::rowsSoFar() + 1;
  for (std::size_t i = start; i < end; ++i) {
    addClick(listRow, listRow, 1, 9999, ClkListItem, static_cast<int>(i));
    ++listRow;
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

// The cover code prints a text placeholder such as
// "[No embedded or folder cover]" when a song has no picture. It is wider than
// the space left of the visualizer, so the bars cut it off. Show nothing
// instead: any placeholder line is replaced by an empty line.
// (Real cover rows never contain these words, so they are left untouched.)
std::string fitCoverLine(const std::string &line) {
  const std::string lower = lowerCase(line);
  if (lower.find("no embedded") != std::string::npos ||
      lower.find("folder cover") != std::string::npos ||
      lower.find("no cover") != std::string::npos)
    return "";
  return line;
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
      coverLines.push_back(fitCoverLine(line));
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

    g_buttonsRow = CoutCapture::rowsSoFar(); // the Status line just printed

    constexpr int width = 30;
    int filled =
        duration > 0.1F
            ? std::clamp(static_cast<int>((position / duration) * width), 0,
                         width)
            : 0;

    const int barRow = CoutCapture::rowsSoFar() + 1;
    addClick(barRow, barRow, 4, 3 + width, ClkSeek, 0); // click = jump there
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

    const std::string timeText = "  Time:    " + formatTime(position) + " / " +
                                 formatTime(duration) + "   |   ";
    const std::string volumeText =
        "Volume: " + std::to_string(static_cast<int>(app.player.getVolume())) +
        "%";
    {
      const int timeRow = CoutCapture::rowsSoFar() + 1;
      addClick(timeRow, timeRow, static_cast<int>(timeText.size()) + 1,
               static_cast<int>(timeText.size() + volumeText.size()), ClkVolume,
               0);
    }
    std::cout << timeText << volumeText << "\n";
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

  if (!app.compact)
    std::cout << kMuted << "  Space Play/Pause  |  ←/→ Seek  |  i Art ("
              << (app.showAlbumArt ? "ON" : "OFF") << ")  |  v Vis ("
              << (app.showVisualizer ? "ON" : "OFF") << ")  |  L Lyrics ("
              << (app.lyricsMode != LyricsMode::Off ? "ON" : "OFF") << ")\n"
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
  row("R", "Repeat: off / all songs / one song");
  row("i", "Toggle Album Art");
  row("v", "Toggle Audio Visualizer");
  row("L", "Lyrics: right panel / below / off");
  row("T", "Show the lyrics on the Arduino LCD (on / off)");
  row("m  /  M",
      "LCD mode: next / previous (lyrics, clock, PC status ... needs muzxlcd)");
  row("[  /  ]  /  \\",
      "Lyrics earlier / later by 0.5 s (saved per song) / reset");
  row("+ / -", "Adjust volume");
  row("x", "Stop playback");
  row("Mouse",
      "Click tabs, buttons, list rows; drag the progress bar to seek;");
  row("", "click the volume number to mute; wheel = scroll / volume");

  std::cout << "\n";
  section("NAVIGATION & TABS");
  row("Tab / Shift+Tab", "Cycle tabs (or keys 1-7)");
  row("5", "Open Status (Clock, Played Time, Top Songs)");
  row("7", "Settings (j/k move, Enter or ←/→ change)");
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

// ---------------------------------------------------------------------------
// Desktop media keys (MPRIS). Set MUZX_MPRIS=0 to turn this off.
// ---------------------------------------------------------------------------

// "SpotiDownloader.com - Title - Artist.mp3" -> title and artist.
void splitSongName(const std::string &path, std::string &title,
                   std::string &artist) {
  std::string name = fs::path(path).stem().string();
  auto isSite = [](const std::string &part) {
    for (const char *tld : {".com", ".org", ".net", ".io", ".to", ".cc"})
      if (part.find(tld) != std::string::npos)
        return true;
    return false;
  };
  std::vector<std::string> parts;
  for (std::size_t pos = 0;;) {
    const std::size_t at = name.find(" - ", pos);
    if (at == std::string::npos) {
      parts.push_back(name.substr(pos));
      break;
    }
    parts.push_back(name.substr(pos, at - pos));
    pos = at + 3;
  }
  if (parts.size() > 1 && isSite(parts.front()))
    parts.erase(parts.begin());
  title = parts.front();
  artist = parts.size() > 1 ? parts.back() : "";
}

// ---------------------------------------------------------------------------
// Lyrics timing offset, saved per song: ~/.config/muzx/lyrics-offsets.txt
// (one line per song:  <path> TAB <seconds>)
// ---------------------------------------------------------------------------
void loadLyricShifts(AppState &app) {
  std::ifstream in(app.lyricShiftFile);
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t tab = line.rfind('\t');
    if (tab == std::string::npos)
      continue;
    try {
      const double v = std::stod(line.substr(tab + 1));
      if (std::isfinite(v) && v != 0.0)
        app.lyricShifts[line.substr(0, tab)] = std::clamp(v, -60.0, 60.0);
    } catch (...) {
    }
  }
}

void saveLyricShifts(const AppState &app) {
  if (app.lyricShiftFile.empty())
    return;
  std::ofstream out(app.lyricShiftFile, std::ios::trunc);
  for (const auto &entry : app.lyricShifts)
    out << entry.first << '\t' << entry.second << '\n';
}

// Picks up the saved shift when the song changes.
void syncLyricShift(AppState &app) {
  if (app.currentSong == app.lyricShiftSong)
    return;
  app.lyricShiftSong = app.currentSong;
  const auto it = app.lyricShifts.find(app.currentSong);
  app.lyricShift = it == app.lyricShifts.end() ? 0.0 : it->second;
}

// delta = 0 resets. Keys: [ earlier, ] later, \ reset.
void adjustLyricShift(AppState &app, double delta) {
  if (!app.hasCurrentSong)
    return;
  syncLyricShift(app);
  app.lyricShift =
      delta == 0.0 ? 0.0 : std::clamp(app.lyricShift + delta, -60.0, 60.0);
  if (std::fabs(app.lyricShift) < 0.01)
    app.lyricShifts.erase(app.currentSong);
  else
    app.lyricShifts[app.currentSong] = app.lyricShift;
  saveLyricShifts(app);
}

// The player only has "louder" and "quieter" steps, so walk towards the target.
void setPlayerVolume(AppState &app, double target01) {
  const float target = static_cast<float>(target01 * 100.0);
  for (int i = 0; i < 30; ++i) {
    const float before = app.player.getVolume();
    const float distance = std::fabs(before - target);
    if (distance < 2.5F)
      break;
    if (before < target)
      app.player.increaseVolume();
    else
      app.player.decreaseVolume();
    const float after = app.player.getVolume();
    if (std::fabs(after - target) >=
        distance) { // stuck, or stepped past the target
      if (std::fabs(after - target) >
          distance) { // went past: undo that last step
        if (after > before)
          app.player.decreaseVolume();
        else
          app.player.increaseVolume();
      }
      break;
    }
  }
}

// Tells the desktop what is playing and does what it asked. Returns true when
// the desktop asked MUZX to quit.
bool serviceMpris(AppState &app) {
  static std::string lastSong;
  static unsigned trackId = 0;
  if (app.currentSong != lastSong) {
    lastSong = app.currentSong;
    ++trackId;
  }

  const bool stopped =
      !app.hasCurrentSong || (!app.playbackRunning && !app.paused);
  MprisState state;
  state.status = stopped ? "Stopped" : (app.paused ? "Paused" : "Playing");
  state.hasTrack = app.hasCurrentSong;
  state.trackId = trackId;
  if (app.hasCurrentSong) {
    splitSongName(app.currentSong, state.title, state.artist);
    float duration = app.player.getDuration().asSeconds();
    float position = app.player.getPlayingOffset().asSeconds();
    if (!std::isfinite(duration) || duration < 0.0F)
      duration = 0.0F;
    if (!std::isfinite(position) || position < 0.0F)
      position = 0.0F;
    state.lengthUs = static_cast<std::int64_t>(duration * 1e6F);
    state.positionUs = static_cast<std::int64_t>(position * 1e6F);
  }
  state.volume =
      std::clamp(static_cast<double>(app.player.getVolume()) / 100.0, 0.0, 1.0);
  state.shuffle = app.shuffle;
  state.loop = app.repeat == RepeatMode::One   ? "Track"
               : app.repeat == RepeatMode::All ? "Playlist"
                                               : "None";
  state.canNext = state.canPrevious =
      app.hasCurrentSong && !app.playlist.empty();
  app.mpris.publish(state);

  for (const MprisCommand &cmd : app.mpris.takeCommands()) {
    switch (cmd.type) {
    case MprisCmd::Play:
      if (stopped || app.paused)
        togglePause(app);
      break;
    case MprisCmd::Pause:
      if (!stopped && !app.paused)
        togglePause(app);
      break;
    case MprisCmd::PlayPause:
      togglePause(app);
      break;
    case MprisCmd::Stop:
      app.player.stop();
      app.playbackRunning = false;
      app.paused = false;
      break;
    case MprisCmd::Next:
      nextSong(app);
      break;
    case MprisCmd::Previous:
      previousSong(app);
      break;
    case MprisCmd::SetVolume:
      setPlayerVolume(app, cmd.value);
      break;
    case MprisCmd::SeekBy:
      if (app.hasCurrentSong) {
        if (cmd.value >= 0)
          app.player.seekForward(static_cast<float>(cmd.value));
        else
          app.player.seekBackward(static_cast<float>(-cmd.value));
      }
      break;
    case MprisCmd::SeekTo:
      if (app.hasCurrentSong) {
        const double now = app.player.getPlayingOffset().asSeconds();
        if (cmd.value >= now)
          app.player.seekForward(static_cast<float>(cmd.value - now));
        else
          app.player.seekBackward(static_cast<float>(now - cmd.value));
      }
      break;
    case MprisCmd::SetShuffle:
      setShuffle(app, cmd.value != 0.0);
      break;
    case MprisCmd::SetLoop:
      app.repeat = cmd.value >= 2.0   ? RepeatMode::One
                   : cmd.value >= 1.0 ? RepeatMode::All
                                      : RepeatMode::Off;
      break;
    case MprisCmd::Quit:
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Desktop notification when a new song starts (needs notify-send / libnotify)
// ---------------------------------------------------------------------------

bool commandExists(const std::string &name) {
  const char *path = std::getenv("PATH");
  if (!path)
    return false;
  std::istringstream in(path);
  std::string dir;
  while (std::getline(in, dir, ':')) {
    if (!dir.empty() && ::access((dir + "/" + name).c_str(), X_OK) == 0)
      return true;
  }
  return false;
}

void sendNotification(const std::string &title, const std::string &body) {
  std::vector<std::string> args = {
      "notify-send",
      "-a",
      "MUZX",
      "-t",
      "4000",
      "-h",
      "string:x-canonical-private-synchronous:muzx",
      "--",
      title,
      body};
  std::vector<char *> argv;
  for (std::string &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions); // keep its output off the screen
  for (int fd = 0; fd <= 2; ++fd)
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null",
                                     fd == 0 ? O_RDONLY : O_WRONLY, 0);
  pid_t pid = 0;
  if (posix_spawnp(&pid, "notify-send", &actions, nullptr, argv.data(),
                   environ) == 0)
    std::thread([pid] {
      int status = 0;
      waitpid(pid, &status, 0); // reap it so no zombie is left behind
    }).detach();
  posix_spawn_file_actions_destroy(&actions);
}

void notifySongChange(AppState &app) {
  static std::string lastSong;
  if (app.currentSong == lastSong)
    return;
  lastSong = app.currentSong;
  if (!app.notify || !app.hasCurrentSong || !app.playbackRunning)
    return;
  std::string title, artist;
  splitSongName(app.currentSong, title, artist);
  sendNotification(title, artist.empty() ? "MUZX" : artist);
}

// ---------------------------------------------------------------------------
// Settings tab
// ---------------------------------------------------------------------------

enum SettingItem {
  SetRepeat = 0, // playback
  SetLyrics,
  SetPrefetch, // lyrics
  SetLcd,
  SetLcdKana, // LCD
  SetArt,
  SetVisualizer,
  SetLayout,
  SetButtons,
  SetNotify, // display
  SetTheme,
  SetColTitle,
  SetColAccent,
  SetColHighlight,
  SetColBars,
  SetFont,   // look
  SetFolder, // general
  kSettingCount
};

const int kFontChoices[] = {0, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28};

int cycleIndex(int current, int dir, int count) {
  return ((current + dir) % count + count) % count;
}

// Font size can only be changed by Kitty itself, through its remote control.
// Returns an empty string on success, otherwise a message for the user.
std::string applyFontSize(int size) {
  if (!std::getenv("KITTY_WINDOW_ID"))
    return "Font size only works inside the Kitty terminal.";
  const std::string command = "timeout 2 kitty @ set-font-size " +
                              std::to_string(size) + " >/dev/null 2>&1";
  if (std::system(command.c_str()) != 0)
    return "Font size failed: add 'allow_remote_control yes' to kitty.conf, "
           "restart Kitty.";
  return "";
}

std::string settingsToText(const AppState &app) {
  std::ostringstream out;
  out << "repeat=" << static_cast<int>(app.repeat) << "\n"
      << "lyrics_mode=" << static_cast<int>(app.lyricsMode) << "\n"
      << "prefetch=" << app.prefetchMode << "\n"
      << "lcd=" << (app.lcd.enabled() ? 1 : 0) << "\n"
      << "lcd_kana=" << (app.lcd.kana() ? 1 : 0) << "\n"
      << "album_art=" << (app.showAlbumArt ? 1 : 0) << "\n"
      << "visualizer=" << (app.showVisualizer ? 1 : 0) << "\n"
      << "compact=" << (app.compact ? 1 : 0) << "\n"
      << "buttons=" << (app.showButtons ? 1 : 0) << "\n"
      << "notify=" << (app.notify ? 1 : 0) << "\n"
      << "color_title=" << g_custom[0] << "\n"
      << "color_accent=" << g_custom[1] << "\n"
      << "color_highlight=" << g_custom[2] << "\n"
      << "color_bars=" << g_custom[3] << "\n"
      << "theme=" << kThemes[app.theme].name << "\n"
      << "font_size=" << app.fontSize << "\n";
  return out.str();
}

// Writes the settings file, but only when something changed.
void saveSettings(AppState &app) {
  if (app.settingsFile.empty())
    return;
  const std::string text = settingsToText(app);
  if (text == app.settingsSaved)
    return;
  std::ofstream file(app.settingsFile, std::ios::trunc);
  if (file << text) {
    file.close();
    app.settingsSaved = text;
  }
}

void loadSettings(AppState &app) {
  std::ifstream file(app.settingsFile);
  std::map<std::string, std::string> values;
  std::string line, onDisk;
  while (std::getline(file, line)) {
    onDisk += line + "\n";
    const std::size_t eq = line.find('=');
    if (eq != std::string::npos)
      values[line.substr(0, eq)] = line.substr(eq + 1);
  }
  auto flag = [&](const char *key, bool &out) {
    auto it = values.find(key);
    if (it != values.end())
      out = it->second == "1";
  };
  if (values.count("repeat")) {
    const int mode = std::atoi(values["repeat"].c_str());
    if (mode >= 0 && mode <= 2)
      app.repeat = static_cast<RepeatMode>(mode);
  }
  if (values.count("lyrics_mode")) {
    const int mode = std::atoi(values["lyrics_mode"].c_str());
    if (mode >= 0 && mode <= 2)
      app.lyricsMode = static_cast<LyricsMode>(mode);
  }
  flag("album_art", app.showAlbumArt);
  flag("compact", app.compact);
  flag("buttons", app.showButtons);
  flag("notify", app.notify);
  static const char *const colorKeys[4] = {"color_title", "color_accent",
                                           "color_highlight", "color_bars"};
  for (int k = 0; k < 4; ++k)
    if (values.count(colorKeys[k]))
      g_custom[k] = std::clamp(std::atoi(values[colorKeys[k]].c_str()), 0,
                               kPaletteCount - 1);
  flag("visualizer", app.showVisualizer);
  if (values.count("lcd"))
    app.lcd.setEnabled(values["lcd"] == "1");
  if (values.count("lcd_kana"))
    app.lcd.setKana(values["lcd_kana"] == "1");
  if (values.count("prefetch"))
    app.prefetchMode = std::clamp(std::atoi(values["prefetch"].c_str()), 0, 1);
  if (values.count("theme"))
    for (int i = 0; i < kThemeCount; ++i)
      if (values["theme"] == kThemes[i].name)
        app.theme = i;
  if (values.count("font_size"))
    app.fontSize =
        std::max(0, std::min(72, std::atoi(values["font_size"].c_str())));
  app.settingsSaved =
      onDisk; // what is on disk now; saveSettings() rewrites only on change
}

// Changes one setting by one step forward (+1) or backward (-1).
void changeSetting(AppState &app, int item, int dir) {
  app.settingsNote.clear();
  switch (item) {
  case SetRepeat:
    app.repeat = static_cast<RepeatMode>(
        cycleIndex(static_cast<int>(app.repeat), dir, 3));
    break;
  case SetLyrics:
    app.lyricsMode = static_cast<LyricsMode>(
        cycleIndex(static_cast<int>(app.lyricsMode), dir, 3));
    break;
  case SetPrefetch:
    app.prefetchMode = app.prefetchMode == 0 ? 1 : 0;
    app.lyricsMgr.setPrefetch(app.prefetchMode == 1);
    break;
  case SetLcd:
    app.lcd.toggle();
    break;
  case SetLcdKana:
    app.lcd.setKana(!app.lcd.kana());
    break;
  case SetArt:
    app.showAlbumArt = !app.showAlbumArt;
    break;
  case SetVisualizer:
    app.showVisualizer = !app.showVisualizer;
    break;
  case SetLayout:
    app.compact = !app.compact;
    break;
  case SetButtons:
    app.showButtons = !app.showButtons;
    break;
  case SetNotify:
    app.notify = !app.notify;
    if (app.notify && !commandExists("notify-send"))
      app.settingsNote = "notify-send not found: install libnotify (sudo "
                         "pacman -S libnotify).";
    break;
  case SetTheme:
    app.theme = cycleIndex(app.theme, dir, kThemeCount);
    applyTheme(app.theme);
    break;
  case SetColTitle:
  case SetColAccent:
  case SetColHighlight:
  case SetColBars: {
    int &slot = g_custom[item - SetColTitle];
    slot = cycleIndex(slot, dir, kPaletteCount);
    app.theme = kCustomTheme; // picking a colour switches to the Custom theme
    applyTheme(app.theme);
    break;
  }
  case SetFont: {
    const int n =
        static_cast<int>(sizeof(kFontChoices) / sizeof(kFontChoices[0]));
    int at = 0;
    for (int i = 0; i < n; ++i)
      if (kFontChoices[i] == app.fontSize)
        at = i;
    app.fontSize = kFontChoices[cycleIndex(at, dir, n)];
    clearTerminalCover();
    app.settingsNote = applyFontSize(app.fontSize);
    break;
  }
  default:
    break;
  }
}

// Asks for a new music folder (same style as the search prompt).
void askMusicFolder(AppState &app) {
  clearTerminalCover();
  disableRawMode();
  std::cout << "\033[?7h\033[?25h\033[2J\033[H";
  std::cout << kPurpleBold << "New music folder (empty = cancel): " << kReset
            << std::flush;
  std::string input;
  if (!std::getline(std::cin, input)) {
    std::cin.clear();
    input.clear();
  }
  input = trim(input);
  if (input.empty()) {
    app.settingsNote = "Music folder not changed.";
  } else {
    std::error_code ec;
    const fs::path folder = normalizeFolder(input);
    if (!folder.empty() && fs::is_directory(folder, ec)) {
      app.musicRoot = folder;
      openFolder(app, folder);
      if (app.cfg)
        app.cfg->save_music_folder(folder.string());
      app.settingsNote = "Music folder set to " + folder.string();
    } else {
      app.settingsNote = "Not a folder: " + input;
    }
  }
  std::cout << "\033[?7l\033[?25l" << std::flush;
  enableRawMode();
}

std::string settingValue(const AppState &app, int item) {
  auto onOff = [](bool v) { return std::string(v ? "On" : "Off"); };
  switch (item) {
  case SetRepeat:
    return app.repeat == RepeatMode::Off   ? "Off"
           : app.repeat == RepeatMode::All ? "All songs"
                                           : "One song";
  case SetLyrics:
    return app.lyricsMode == LyricsMode::Off     ? "Off"
           : app.lyricsMode == LyricsMode::Right ? "Right of the player"
                                                 : "Below the player";
  case SetPrefetch: {
    if (app.prefetchMode == 0)
      return "Off";
    const lyrics::Manager::PrefetchStatus st = app.lyricsMgr.prefetchStatus();
    return "On   (" + std::to_string(st.processed) + "/" +
           std::to_string(st.total) + " checked, " + std::to_string(st.found) +
           " found)";
  }
  case SetLcd:
    if (!app.lcd.enabled())
      return "Off";
    if (app.lcd.viaHub())
      return "On (through the LCD hub)";
    return app.lcd.connected() ? "On (connected)"
                               : "On (waiting for the Arduino)";
  case SetLcdKana:
    return app.lcd.kana() ? "Katakana (Japanese ROM)" : "Romaji (any LCD)";
  case SetArt:
    return onOff(app.showAlbumArt);
  case SetVisualizer:
    return onOff(app.showVisualizer);
  case SetLayout:
    return app.compact ? "Compact (no hint lines)" : "Normal";
  case SetButtons:
    return onOff(app.showButtons);
  case SetNotify:
    return onOff(app.notify);
  case SetTheme:
    return kThemes[app.theme].name;
  case SetColTitle:
  case SetColAccent:
  case SetColHighlight:
  case SetColBars: {
    const int c = g_custom[item - SetColTitle];
    return std::string(kPalette[c].name) + "  \033[38;5;" +
           std::to_string(kPalette[c].code) + "m██\033[39m";
  }
  case SetFont:
    return app.fontSize == 0 ? "terminal default"
                             : std::to_string(app.fontSize);
  case SetFolder:
    return app.musicRoot.string();
  default:
    return "";
  }
}

void drawSettings(const AppState &app) {
  static const char *const labels[kSettingCount] = {
      "Repeat",          "Lyrics panel",      "Prepare lyrics", "LCD display",
      "LCD Japanese",    "Album art",         "Visualizer",     "Layout",
      "Control buttons", "Notifications",     "Theme",          "Custom: title",
      "Custom: accent",  "Custom: highlight", "Custom: bars",   "Font size",
      "Music folder"};
  std::cout << kPurpleBold << "SETTINGS" << kReset << "\n";
  const std::size_t selected = app.settingsCursor.current();

  // Build every line first, then show only the part that fits on the screen
  // (centred on the selected row), so no setting is ever out of reach.
  struct Line {
    std::string text;
    int item; // -1 for headings and blank lines
  };
  std::vector<Line> lines;
  auto heading = [&](const char *title) {
    lines.push_back({"", -1});
    lines.push_back({std::string("  ") + kMuted + title + kReset, -1});
  };
  int selectedLine = 0;
  for (int i = 0; i < kSettingCount; ++i) {
    if (i == SetRepeat)
      heading("PLAYBACK");
    else if (i == SetLyrics)
      heading("LYRICS");
    else if (i == SetLcd)
      heading("LCD (Arduino)");
    else if (i == SetArt)
      heading("DISPLAY");
    else if (i == SetTheme)
      heading("THEME & FONT");
    else if (i == SetFolder)
      heading("GENERAL");
    std::string label = labels[i];
    label.resize(20, ' ');
    if (static_cast<std::size_t>(i) == selected) {
      selectedLine = static_cast<int>(lines.size());
      lines.push_back({"  " + std::string(kSelected) + " " + label +
                           settingValue(app, i) + " " + kReset,
                       i});
    } else {
      lines.push_back(
          {"    " + label + kPink + settingValue(app, i) + kReset, i});
    }
  }

  // Rows above (4) and below (hint lines, our own footer of 3) are not ours.
  const int available =
      std::max(4, terminalRows() - 4 - (app.compact ? 0 : 2) - 3);
  const int total = static_cast<int>(lines.size());
  int first = 0;
  if (total > available)
    first = std::clamp(selectedLine - available / 2, 0, total - available);
  const int last = std::min(total, first + available);
  for (int k = first; k < last; ++k) {
    if (lines[static_cast<std::size_t>(k)].item >= 0) {
      const int row = CoutCapture::rowsSoFar() + 1;
      addClick(row, row, 1, 9999, ClkSetting,
               lines[static_cast<std::size_t>(k)].item);
    }
    std::cout << lines[static_cast<std::size_t>(k)].text << "\n";
  }
  std::cout << "\n  " << kMuted
            << "j/k move   Enter or →/l next value   ←/h previous value";
  if (total > available)
    std::cout << "   (" << (selected + 1) << "/" << kSettingCount << ")";
  std::cout << kReset << "\n";
  if (!app.settingsNote.empty())
    std::cout << "  " << kGreenBold << app.settingsNote << kReset << "\n";
}

// ---------------------------------------------------------------------------
// Lyrics panel (right side of the Player tab)
//
// lyrics.cpp finds the lyrics on a background thread. Here we only DRAW them:
// the line that is being sung is highlighted and kept in the middle, like
// Spotify. Rows are positioned with absolute cursor codes ("\033[row;colH") on
// top of the finished frame, so the rest of the layout is not touched.
// ---------------------------------------------------------------------------

constexpr int kLyricsMinCols = 101;         // narrower terminals: no panel
constexpr double kLyricsLeadSeconds = 0.25; // highlight a line a bit early

// Where we are in the song, as far as the lyrics are concerned.
double lyricsPosition(const AppState &app) {
  return static_cast<double>(app.player.getPlayingOffset().asSeconds()) +
         kLyricsLeadSeconds - app.lyricShift;
}

int terminalCols() {
  winsize w{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
    return static_cast<int>(w.ws_col);
  return 80;
}

// Reads one UTF-8 character at text[i]; returns how many bytes it uses.
std::size_t decodeUtf8(const std::string &text, std::size_t i, char32_t &cp) {
  const unsigned char c = static_cast<unsigned char>(text[i]);
  std::size_t len = 1;
  cp = c;
  if (c >= 0xF0) {
    len = 4;
    cp = c & 0x07;
  } else if (c >= 0xE0) {
    len = 3;
    cp = c & 0x0F;
  } else if (c >= 0xC0) {
    len = 2;
    cp = c & 0x1F;
  } else {
    return 1;
  }
  if (i + len > text.size()) {
    cp = 0xFFFD;
    return 1;
  }
  for (std::size_t k = 1; k < len; ++k) {
    const unsigned char n = static_cast<unsigned char>(text[i + k]);
    if ((n & 0xC0) != 0x80) {
      cp = 0xFFFD;
      return 1;
    }
    cp = (cp << 6) | (n & 0x3F);
  }
  return len;
}

// Terminal columns a character occupies: Japanese/Chinese/Korean and emoji
// take 2, combining marks take 0, everything else 1.
int charColumns(char32_t cp) {
  if (cp == 0 || (cp >= 0x0300 && cp <= 0x036F) ||
      (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F))
    return 0;
  if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
      (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
      (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1FAFF) ||
      (cp >= 0x20000 && cp <= 0x3FFFD))
    return 2;
  return 1;
}

// Splits a lyric line into rows that fit `width` columns. Breaks at spaces
// when possible; text without spaces (Japanese) is broken between characters.
std::vector<std::string> wrapLyric(const std::string &text, std::size_t width) {
  std::vector<std::string> rows;
  if (width == 0)
    return rows;

  std::string cur;
  std::size_t curW = 0;
  std::size_t lastSpaceByte =
      std::string::npos; // position of last space in cur
  std::size_t widthBeforeSpace = 0;

  for (std::size_t i = 0; i < text.size();) {
    char32_t cp = 0;
    const std::size_t len = decodeUtf8(text, i, cp);
    const std::size_t w = static_cast<std::size_t>(charColumns(cp));

    if (cp == U' ' && curW == 0) { // no leading spaces on a row
      i += len;
      continue;
    }
    if (curW + w > width) {
      if (lastSpaceByte != std::string::npos && cp != U' ') {
        rows.push_back(cur.substr(0, lastSpaceByte)); // break at the space
        cur = cur.substr(lastSpaceByte + 1);
        curW = curW - widthBeforeSpace - 1;
      } else {
        rows.push_back(cur); // no space to break at: break here
        cur.clear();
        curW = 0;
      }
      lastSpaceByte = std::string::npos;
      if (cp == U' ') {
        i += len;
        continue;
      }
    }
    if (cp == U' ') {
      lastSpaceByte = cur.size();
      widthBeforeSpace = curW;
    }
    cur.append(text, i, len);
    curW += w;
    i += len;
  }
  if (!cur.empty())
    rows.push_back(cur);
  if (rows.empty())
    rows.emplace_back();
  return rows;
}

// Terminal columns taken by a whole string (see charColumns above).
std::size_t displayWidth(const std::string &text) {
  std::size_t width = 0;
  for (std::size_t i = 0; i < text.size();) {
    char32_t cp = 0;
    i += decodeUtf8(text, i, cp);
    width += static_cast<std::size_t>(charColumns(cp));
  }
  return width;
}

// Cuts a string to at most `maxCols` terminal columns (never inside a
// character).
std::string clipColumns(const std::string &text, std::size_t maxCols) {
  std::string out;
  std::size_t cols = 0;
  for (std::size_t i = 0; i < text.size();) {
    char32_t cp = 0;
    const std::size_t len = decodeUtf8(text, i, cp);
    const std::size_t w = static_cast<std::size_t>(charColumns(cp));
    if (cols + w > maxCols)
      break;
    out.append(text, i, len);
    cols += w;
    i += len;
  }
  return out;
}

// Asks the lyrics thread for the lyrics of the song that is playing now.
// Called every frame; it only does work when the song changes.
void updateLyricsRequest(AppState &app) {
  if ((app.lyricsMode == LyricsMode::Off && !app.lcd.enabled()) ||
      !app.hasCurrentSong)
    return;

  const auto now = std::chrono::steady_clock::now();
  float duration = app.player.getDuration().asSeconds();
  if (!std::isfinite(duration) || duration < 0.0F)
    duration = 0.0F;

  if (app.currentSong != app.lyricsSong) {
    app.lyricsSong = app.currentSong;
    app.lyricsSongSince = now;
    app.lyricsMgr.request(app.currentSong, static_cast<double>(duration));
    return;
  }
}

// Tells the lyrics module which songs to prepare in the background: the rest
// of the playlist first (in play order), then everything else in the library.
void updatePrefetchQueue(AppState &app) {
  if (app.prefetchMode == 0)
    return;
  const std::string key =
      std::to_string(app.prefetchMode) + "|" + app.currentSong + "|" +
      std::to_string(app.playlist.size()) + "|" +
      std::to_string(app.playlistIndex) + "|" + app.musicRoot.string();
  if (key == app.prefetchKey)
    return;
  app.prefetchKey = key;

  if (app.libraryRoot != app.musicRoot || app.library.empty()) {
    app.libraryRoot = app.musicRoot;
    app.library = audioFilesIn(app.musicRoot, true);
  }
  std::vector<std::string> queue;
  std::unordered_set<std::string> seen;
  auto add = [&](const std::string &song) {
    if (!song.empty() && song != app.currentSong && seen.insert(song).second)
      queue.push_back(song);
  };
  const std::size_t n = app.playlist.size();
  if (n > 0)
    for (std::size_t i = 1; i <= n; ++i)
      add(app.playlist[(app.playlistIndex + i) % n]);
  for (const std::string &song : app.library)
    add(song);
  app.lyricsMgr.setPrefetchQueue(queue);
}

// Sends the line being sung (or the song name) to the Arduino LCD.
void updateLcd(AppState &app) {
  if (!app.lcd.enabled())
    return;
  if (!app.hasCurrentSong) {
    app.lcd.show("MUZX");
    return;
  }
  std::string text = fs::path(app.currentSong).stem().string(); // fallback
  const std::shared_ptr<const lyrics::Result> result = app.lyricsMgr.result();
  if (app.lyricsMgr.state() == lyrics::State::Ready && result &&
      result->synced && !result->lines.empty()) {
    const double position = lyricsPosition(app);
    const auto &lines = result->lines;
    const auto it = std::upper_bound(
        lines.begin(), lines.end(), position,
        [](double p, const lyrics::Line &l) { return p < l.time; });
    const int cur = static_cast<int>(it - lines.begin()) - 1;
    if (cur >= 0)
      text = lines[static_cast<std::size_t>(cur)].text.empty()
                 ? "~ music ~"
                 : lines[static_cast<std::size_t>(cur)].text;
  }
  app.lcd.show(text);
}

// Where the lyrics are drawn.
struct LyricsBox {
  int col = 1;      // left column (1-based)
  int titleRow = 1; // row of the "LYRICS" title
  int top = 1;      // first lyric row
  int height = 0;   // number of lyric rows
  std::size_t width = 0;
  bool centered = false; // bottom placement centres every line
};

// Works out the box for the current placement. Returns false when it does not
// fit on the screen.
//   Right  = the empty area to the right of the player
//   Bottom = the empty area below the player (needs a few free rows)
bool lyricsBox(const AppState &app, int rows, int cols, int usedRows,
               LyricsBox &box) {
  LyricsMode mode = app.lyricsMode;
  if (mode == LyricsMode::Off)
    return false;
  if (mode == LyricsMode::Right && cols < kLyricsMinCols)
    mode = LyricsMode::Bottom; // too narrow for a side panel

  if (mode == LyricsMode::Right) {
    box.col = cols >= 140 ? 88 : 82;
    box.width = static_cast<std::size_t>(cols - box.col - 1);
    box.titleRow = 4;
    box.top = 5;
    box.height = std::min(std::clamp(rows - 8, 8, 24), rows - box.top + 1);
    box.centered = false;
  } else {
    box.col = 3;
    box.width = static_cast<std::size_t>(std::max(0, std::min(cols - 4, 90)));
    box.titleRow = usedRows + 2;
    box.top = usedRows + 3;
    box.height = std::min(rows - box.top + 1, 9);
    box.centered = true;
  }
  return box.height >= 3 && box.width >= 12;
}

// Builds the escape codes that draw the lyrics. Returns "" when they should
// not be shown (other tab, switched off, nothing playing, no room).
std::string lyricsPanel(const AppState &app, int rows, int cols, int usedRows) {
  if (app.tab != AppTab::Player || !app.hasCurrentSong)
    return "";
  LyricsBox box;
  if (!lyricsBox(app, rows, cols, usedRows, box))
    return "";

  const std::size_t width = box.width;
  const int top = box.top;
  const int height = box.height;
  const int mid = height / 2;

  // Moves to a row of the box and clears the rest of that screen row.
  auto at = [&](int row) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(box.col) +
           "H\033[K";
  };
  // One line of text, centred when the box is centred.
  auto put = [&](int row, const std::string &color, const std::string &text) {
    std::string line = at(row);
    if (box.centered) {
      const std::size_t w = displayWidth(text);
      if (w < width)
        line += std::string((width - w) / 2, ' ');
    }
    return line + color + text + kReset;
  };

  const lyrics::State state = app.lyricsMgr.state();
  const std::shared_ptr<const lyrics::Result> result = app.lyricsMgr.result();

  // Title: "LYRICS  <source>   <key hints>". The hints are shortened or left
  // out so the line never runs past the right edge (the terminal would pile
  // the last characters up in the final column).
  std::string detail;
  if (result)
    detail = "  " + result->source + (result->synced ? "" : "  (not synced)");
  if (result && std::fabs(app.lyricShift) >= 0.01) {
    char shift[24];
    std::snprintf(shift, sizeof(shift), "  sync %+.1fs", app.lyricShift);
    detail += shift;
  }
  const std::string hints[] = {"   L move/hide  |  [ ] timing", "   [L]", ""};
  const std::size_t titleRoom = width > 6 ? width - 6 : 0;
  for (const std::string &hint : hints) {
    if (displayWidth(detail + hint) <= titleRoom) {
      detail += hint;
      break;
    }
  }
  std::string out = at(box.titleRow) + kPurpleBold + "LYRICS" + kReset +
                    kMuted + clipColumns(detail, titleRoom) + kReset;

  // No lyrics (yet): show a short message in the middle of the box.
  if (!result) {
    std::string message;
    if (state == lyrics::State::NotFound) {
      message = "No lyrics found. Put a .lrc file with the same name next to "
                "the song, or check your internet connection.";
    } else {
      message = "Searching for lyrics...";
    }
    const std::vector<std::string> parts = wrapLyric(message, width);
    int row = top + std::max(0, mid - static_cast<int>(parts.size()) / 2);
    for (const std::string &part : parts) {
      if (row >= top + height)
        break;
      out += put(row++, kMuted, part);
    }
    return out;
  }

  struct Row {
    int line;
    std::string text;
  };
  std::vector<Row> display;
  const auto &lines = result->lines;
  const int count = static_cast<int>(lines.size());
  int curLine = -1;
  long startRow = 0;

  if (result->synced) {
    const double position = lyricsPosition(app);
    // The line being sung = the last one whose time has already passed.
    const auto it = std::upper_bound(
        lines.begin(), lines.end(), position,
        [](double p, const lyrics::Line &l) { return p < l.time; });
    curLine = static_cast<int>(it - lines.begin()) - 1;

    // Only lay out the lines around the current one.
    const int focus = std::max(curLine, 0);
    const int lo = std::max(0, focus - height);
    const int hi = std::min(count, focus + height + 1);
    long anchorRow = 0;
    for (int i = lo; i < hi; ++i) {
      std::string text = lines[static_cast<std::size_t>(i)].text;
      if (text.empty() && i == curLine)
        text = "\xE2\x99\xAA"; // ♪ during an instrumental break
      if (i == focus)
        anchorRow = static_cast<long>(display.size());
      for (std::string &part : wrapLyric(text, width))
        display.push_back({i, std::move(part)});
    }
    startRow = anchorRow - mid; // keep the current line in the middle
  } else {
    // Unsynced: plain list that slowly scrolls as the song progresses.
    const int limit = std::min(count, 500);
    for (int i = 0; i < limit; ++i) {
      for (std::string &part :
           wrapLyric(lines[static_cast<std::size_t>(i)].text, width))
        display.push_back({i, std::move(part)});
    }
    const double duration =
        static_cast<double>(app.player.getDuration().asSeconds());
    const double position =
        static_cast<double>(app.player.getPlayingOffset().asSeconds());
    const long maxStart =
        std::max(0L, static_cast<long>(display.size()) - height);
    if (duration > 1.0 && std::isfinite(duration) && std::isfinite(position))
      startRow = static_cast<long>(
          std::lround(std::clamp(position / duration, 0.0, 1.0) *
                      static_cast<double>(maxStart)));
  }

  for (int r = 0; r < height; ++r) {
    const long idx = startRow + r;
    if (idx < 0 || idx >= static_cast<long>(display.size()))
      continue;
    const Row &row = display[static_cast<std::size_t>(idx)];
    if (row.text.empty())
      continue;

    const char *color = "";
    if (result->synced) {
      if (row.line == curLine)
        color = kLyricCurrent;
      else if (std::abs(row.line - curLine) <= 1)
        color = kLyricNear;
      else
        color = kMuted;
    }
    out += put(top + r, color, row.text);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Player buttons: shuffle, previous, play/pause, next, repeat. Drawn on top of
// the finished frame, to the right of the Status / progress / Time lines.
// ---------------------------------------------------------------------------

int rightPanelColumn(const AppState &app, int cols) {
  if (app.lyricsMode != LyricsMode::Right || cols < kLyricsMinCols)
    return 0;
  return cols >= 140 ? 88 : 82;
}

std::string playerButtons(const AppState &app, int rows, int cols) {
  if (!app.showButtons || app.tab != AppTab::Player || !app.hasCurrentSong ||
      g_buttonsRow <= 0)
    return "";
  const int startCol = 46;
  const int panel = rightPanelColumn(app, cols);
  const int room = (panel > 0 ? panel - 2 : cols - 1) - startCol + 1;
  const int w = room >= 39 ? 7 : (room >= 29 ? 5 : 0); // button width
  const int top = g_buttonsRow;
  if (w == 0 || top + 2 > rows)
    return "";

  const bool playing = app.playbackRunning && !app.paused;
  struct Button {
    const char *glyph;
    int glyphWidth;
    std::string color;
    int action;
  };
  const Button buttons[5] = {
      {"⇄", 1, app.shuffle ? kGreenBold : kMuted, ClkShuffle},
      {"◀◀", 2, kPink, ClkPrev},
      {playing ? "▮▮" : "▶", playing ? 2 : 1, kPurpleBold, ClkPlayPause},
      {"▶▶", 2, kPink, ClkNext},
      {app.repeat == RepeatMode::One ? "↻1" : "↻",
       app.repeat == RepeatMode::One ? 2 : 1,
       app.repeat == RepeatMode::Off ? kMuted : kGreenBold, ClkRepeat}};

  std::string out;
  auto at = [](int row, int col) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
  };
  for (int k = 0; k < 5; ++k) {
    const Button &b = buttons[k];
    const int col = startCol + k * (w + 1);
    const int inner = w - 2;
    const int left = (inner - b.glyphWidth) / 2;
    const int right = inner - b.glyphWidth - left;
    std::string line;
    for (int i = 0; i < inner; ++i)
      line += "─";
    out += at(top, col) + b.color + "╭" + line + "╮";
    out += at(top + 1, col) + b.color + "│" +
           std::string(static_cast<std::size_t>(left), ' ') + b.glyph +
           std::string(static_cast<std::size_t>(right), ' ') + b.color + "│";
    out += at(top + 2, col) + b.color + "╰" + line + "╯" + kReset;
    addClick(top, top + 2, col, col + w - 1, b.action, 0);
  }
  return out;
}

void drawScreen(const AppState &app) {
  g_clicks.clear();
  g_buttonsRow = 0;
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
    case AppTab::Settings:
      drawSettings(app);
      break;
    }

    if (!app.compact)
      std::cout
          << kMuted
          << "------------------------------------------------------------\n"
          << "  Tab/1-7 Tabs  |  Space Play/Pause  |  i Art  |  v Vis  |  L "
             "Lyrics  |  "
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

  // Lyrics go on top of the finished frame, in the same write (no flicker).
  output += lyricsPanel(app, maxRows, terminalCols(), rowCount);
  output += playerButtons(app, maxRows, terminalCols());
  output += "\033[H";
  std::cout << output << std::flush;

  static const std::string kNoSong;
  const std::string &coverSong = app.hasCurrentSong ? app.currentSong : kNoSong;
  const bool wantCover = app.showAlbumArt && app.tab == AppTab::Player;

  // The cover code only draws an image when a song HAS one; it never removes
  // the previous picture. So when the song (or the art on/off state) changes,
  // delete the old image first. A song without a cover then shows nothing
  // instead of the previous song's picture.
  static std::string lastCoverSong;
  static bool lastWantCover = false;
  if (coverSong != lastCoverSong || wantCover != lastWantCover) {
    clearTerminalCover();
    lastCoverSong = coverSong;
    lastWantCover = wantCover;
  }

  drawConfirmedKittyCover(coverSong, wantCover, app.tab == AppTab::Player);
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

enum class KeyResult { Handled, FlushInput, Quit };

KeyResult handleKey(AppState &app, int key, const fs::path &queueFile);

KeyResult handleMouse(AppState &app, const fs::path &queueFile) {
  const MouseEvent m = lastMouse;

  if (m.button == 64 || m.button == 65) { // wheel up / down
    const bool up = m.button == 64;
    auto move = [&](selection &cursor, std::size_t count) {
      if (count == 0)
        return;
      if (up)
        cursor.moveUp(count);
      else
        cursor.moveDown(count);
    };
    switch (app.tab) {
    case AppTab::Player:
      if (up)
        app.player.increaseVolume();
      else
        app.player.decreaseVolume();
      break;
    case AppTab::Folders:
      move(app.folderCursor, folderMenuSize(app));
      break;
    case AppTab::Queue:
      move(app.queueCursor, app.songQueue.size());
      break;
    case AppTab::Search:
      move(app.searchCursor, app.searchResults.size());
      break;
    case AppTab::Settings:
      move(app.settingsCursor, kSettingCount);
      break;
    default:
      break;
    }
    return KeyResult::Handled;
  }
  if (m.button == 32) { // the mouse moved with the left button held down
    if (g_drag.active && app.hasCurrentSong) {
      const double fraction = static_cast<double>(m.x - g_drag.col0) /
                              std::max(1, g_drag.col1 - g_drag.col0);
      const double target = std::clamp(fraction, 0.0, 1.0) * g_drag.duration;
      const double delta = target - g_drag.target;
      if (std::fabs(delta) >= 0.05) {
        if (delta > 0)
          app.player.seekForward(static_cast<float>(delta));
        else
          app.player.seekBackward(static_cast<float>(-delta));
        g_drag.target = target;
      }
    }
    return KeyResult::Handled;
  }
  if (!m.press) { // button released: the drag is over
    g_drag.active = false;
    return KeyResult::Handled;
  }
  g_drag.active = false;
  if (m.button != 0) // only a plain left click
    return KeyResult::Handled;

  for (auto it = g_clicks.rbegin(); it != g_clicks.rend(); ++it) {
    if (m.y < it->row0 || m.y > it->row1 || m.x < it->col0 || m.x > it->col1)
      continue;
    switch (it->action) {
    case ClkTab:
      app.tab = static_cast<AppTab>(it->arg);
      break;
    case ClkShuffle:
      setShuffle(app, !app.shuffle);
      break;
    case ClkPrev:
      previousSong(app);
      break;
    case ClkPlayPause:
      togglePause(app);
      break;
    case ClkNext:
      nextSong(app);
      break;
    case ClkRepeat:
      app.repeat =
          static_cast<RepeatMode>((static_cast<int>(app.repeat) + 1) % 3);
      break;
    case ClkSeek: {
      if (!app.hasCurrentSong)
        break;
      const float duration = app.player.getDuration().asSeconds();
      const float position = app.player.getPlayingOffset().asSeconds();
      if (!std::isfinite(duration) || duration <= 0.1F)
        break;
      const float fraction =
          static_cast<float>(m.x - it->col0) /
          static_cast<float>(std::max(1, it->col1 - it->col0));
      const float target = std::clamp(fraction, 0.0F, 1.0F) * duration;
      const float delta = target - position;
      if (delta >= 0.0F)
        app.player.seekForward(delta);
      else
        app.player.seekBackward(-delta);
      // Keep the button held and move: the bar follows the mouse.
      g_drag = {true, static_cast<double>(target),
                static_cast<double>(duration), it->col0, it->col1};
      break;
    }
    case ClkVolume: {
      const double now = app.player.getVolume() / 100.0;
      if (now > 0.02) {
        app.volumeBeforeMute = now;
        setPlayerVolume(app, 0.0);
      } else {
        setPlayerVolume(app, app.volumeBeforeMute > 0.02 ? app.volumeBeforeMute
                                                         : 0.5);
      }
      break;
    }
    case ClkListItem: {
      selection *cursor = nullptr;
      std::size_t count = 0;
      if (app.tab == AppTab::Folders) {
        cursor = &app.folderCursor;
        count = folderMenuSize(app);
      } else if (app.tab == AppTab::Queue) {
        cursor = &app.queueCursor;
        count = app.songQueue.size();
      } else if (app.tab == AppTab::Search) {
        cursor = &app.searchCursor;
        count = app.searchResults.size();
      }
      const std::size_t target = static_cast<std::size_t>(it->arg);
      if (!cursor || target >= count)
        break;
      if (target == cursor->current())
        return handleKey(
            app, KeyEnter,
            queueFile); // click the selected row again = open / play
      while (cursor->current() < target)
        cursor->moveDown(count);
      while (cursor->current() > target)
        cursor->moveUp(count);
      break;
    }
    case ClkSetting:
      if (static_cast<std::size_t>(it->arg) == app.settingsCursor.current())
        return handleKey(app, KeyEnter,
                         queueFile); // change / open the selected setting
      while (app.settingsCursor.current() < static_cast<std::size_t>(it->arg))
        app.settingsCursor.moveDown(kSettingCount);
      while (app.settingsCursor.current() > static_cast<std::size_t>(it->arg))
        app.settingsCursor.moveUp(kSettingCount);
      break;
    }
    break; // the first (topmost) region wins
  }
  return KeyResult::Handled;
}

KeyResult handleKey(AppState &app, int key, const fs::path &queueFile) {
  std::error_code ec;

  if (key == KeyMouse)
    return handleMouse(app, queueFile);

  if (key >= '1' && key <= '7') {
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

  if (app.tab == AppTab::Settings) {
    if (key == 'j' || key == KeyDown) {
      app.settingsCursor.moveDown(kSettingCount);
      return KeyResult::Handled;
    }
    if (key == 'k' || key == KeyUp) {
      app.settingsCursor.moveUp(kSettingCount);
      return KeyResult::Handled;
    }
    const bool forward = key == KeyEnter || key == KeyRight || key == 'l';
    if (forward || key == KeyLeft || key == 'h') {
      const int item = static_cast<int>(app.settingsCursor.current());
      if (item == SetFolder) {
        if (forward)
          askMusicFolder(app);
        return KeyResult::FlushInput;
      }
      changeSetting(app, item, forward ? 1 : -1);
      saveSettings(app);
      return KeyResult::Handled;
    }
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
    setShuffle(app, !app.shuffle);
    return KeyResult::Handled;
  }
  if (key == 'R') {
    app.repeat =
        static_cast<RepeatMode>((static_cast<int>(app.repeat) + 1) % 3);
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
  if (key == 'L') {
    // Off -> Right panel -> Below the player -> Off
    app.lyricsMode =
        app.lyricsMode == LyricsMode::Off
            ? LyricsMode::Right
            : (app.lyricsMode == LyricsMode::Right ? LyricsMode::Bottom
                                                   : LyricsMode::Off);
    return KeyResult::Handled;
  }
  if (key == '[' || key == ']' || key == '\\') {
    // Lyrics timing for this song: [ earlier, ] later, \ reset
    adjustLyricShift(app, key == '[' ? -0.5 : key == ']' ? 0.5 : 0.0);
    return KeyResult::Handled;
  }
  if (key == 'T') {
    app.lcd.toggle(); // LCD on/off
    return KeyResult::Handled;
  }
  if (key == 'm' || key == 'M') {
    // Change what the Arduino LCD shows (needs the muzxlcd program)
    app.lcd.command(key == 'm' ? "mode next" : "mode prev");
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
  app.cfg = &settings;
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

  // Lyrics are cached in the same folder. MUZX_LYRICS_ONLINE=0 turns the
  // web lookup off (local .lrc files and tags still work).
  app.lyricsMgr.setCacheDir(configDirectory);
  if (const char *online = std::getenv("MUZX_LYRICS_ONLINE");
      online && std::string(online) == "0")
    app.lyricsMgr.setOnlineEnabled(false);

  app.settingsFile = configDirectory / "settings.conf";
  app.lyricShiftFile = configDirectory / "lyrics-offsets.txt";
  loadLyricShifts(app);
  loadSettings(app);
  app.lyricsMgr.setPrefetch(app.prefetchMode >= 1);
  applyTheme(app.theme);
  if (app.fontSize > 0)
    applyFontSize(app.fontSize); // before the screen switches to raw mode

  if (const char *mp = std::getenv("MUZX_MPRIS");
      !(mp && std::string(mp) == "0"))
    app.mpris.start(); // media keys, playerctl, desktop media widget

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
      saveSettings(
          app); // picks up changes made with the hot keys (L, A, T, i, v)
      statsSaveAccumulator = 0.0;
    }

    updateLyricsRequest(app); // start a lyrics lookup when the song changed
    syncLyricShift(app);
    updatePrefetchQueue(app);
    updateLcd(app);
    notifySongChange(app);
    if (serviceMpris(app))
      running = false; // the desktop asked MUZX to quit
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
  saveSettings(app);
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
