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
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
constexpr const char *kCyanBold = "\033[1;36m";

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
  if (!readByte(second, 8))
    return KeyEscape;
  if (second != '[')
    return KeyEscape;

  char third = '\0';
  if (!readByte(third, 8))
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
         ext == ".aiff" || ext == ".au" || ext == ".m4a" || ext == ".opus" ||
         ext == ".aac";
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
  if (path.empty())
    return "";
  return fs::path(path).filename().string();
}

enum class AppTab {
  Player = 0,
  Folders = 1,
  Queue = 2,
  Search = 3,
  Status = 4,
  Keybindings = 5
};

constexpr int kTabCount = 6;
constexpr std::size_t kVisBars = audio_player::kBandCount;

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

void saveStats(const AppState &app) {
  if (app.statsFile.empty())
    return;
  std::ofstream out(app.statsFile);
  if (!out)
    return;

  out << static_cast<long long>(app.totalPlayedSeconds) << "\n";
  for (const auto &[songPath, count] : app.playCounts) {
    out << count << "\t" << songPath << "\n";
  }
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

    const auto realBands = app.player.getSpectrumBands();
    const float riseAlpha =
        std::clamp(static_cast<float>(dtSeconds) * 28.0F, 0.1F, 0.85F);
    const float fallAlpha =
        std::clamp(static_cast<float>(dtSeconds) * 10.0F, 0.05F, 0.45F);

    for (std::size_t i = 0; i < kVisBars; ++i) {
      const float target = realBands[i];
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
    }
  }
}

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
        app.playCounts[path]++;
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

const char *subBlockGlyph(int eighths) {
  static constexpr const char *kBlocks[9] = {"  ", "▁▁", "▂▂", "▃▃", "▄▄",
                                             "▅▅", "▆▆", "▇▇", "██"};
  return kBlocks[std::clamp(eighths, 0, 8)];
}

void drawPlayerCoverAndVisualizer(const AppState &app) {
  if (app.tab != AppTab::Player || (!app.showAlbumArt && !app.showVisualizer))
    return;

  std::vector<std::string> coverLines;
  if (app.showAlbumArt && app.hasCurrentSong) {
    std::ostringstream coverStream;
    std::streambuf *oldBuf = std::cout.rdbuf(coverStream.rdbuf());
    renderPlayerCover(app.currentSong, app.showAlbumArt, true);
    std::cout.rdbuf(oldBuf);

    std::istringstream iss(coverStream.str());
    std::string line;
    while (std::getline(iss, line)) {
      coverLines.push_back(line);
    }
  }
  while (coverLines.size() < 10) {
    coverLines.emplace_back("");
  }

  constexpr int kTotalRows = 10;
  constexpr int kBarRows = 9;

  for (int r = 0; r < kTotalRows; ++r) {
    if (app.showAlbumArt && app.hasCurrentSong) {
      if (coverLines[static_cast<std::size_t>(r)].empty()) {
        // Push the visualizer to column 28 to leave a clean gap after the
        // 20-col cover art
        std::cout << "\033[28G";
      } else {
        std::cout << coverLines[static_cast<std::size_t>(r)] << "\033[28G";
      }
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
    if (std::isnan(duration) || std::isinf(duration) || duration < 0.0F)
      duration = 0.0F;
    if (std::isnan(position) || std::isinf(position) || position < 0.0F)
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
    std::cout << "  Shuffle: "
              << (app.shuffle ? std::string(kGreenBold) + "ON"
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
  std::sort(sortedPlays.begin(), sortedPlays.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });

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
  std::ostringstream frame;
  std::streambuf *terminalBuffer = std::cout.rdbuf(frame.rdbuf());

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

  std::cout << kMuted
            << "------------------------------------------------------------\n"
            << "  Tab/1-6 Tabs  |  Space Play/Pause  |  i Art  |  v Vis  |  "
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
  std::string output = "\033[?7l\033[H";

  std::size_t start = 0;
  int rowCount = 0;
  while (start < content.size() && rowCount < maxRows) {
    std::size_t end = content.find('\n', start);
    ++rowCount;

    if (end == std::string::npos) {
      output.append(content, start, std::string::npos);
      output += "\033[K";
      break;
    }

    output.append(content, start, end - start);
    output += "\033[K";
    if (rowCount < maxRows) {
      output += "\n";
    }
    start = end + 1;
  }

  output += "\033[J\033[H";
  std::cout << output << std::flush;

  drawConfirmedKittyCover(app.hasCurrentSong ? app.currentSong : "",
                          app.showAlbumArt && app.tab == AppTab::Player,
                          app.tab == AppTab::Player);
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
  app.statsFile = configDirectory / "stats.txt";

  app.songQueue.load(queueFile.string());
  loadStats(app);

  TerminalSession terminal;
  bool running = true;
  auto lastTick = std::chrono::steady_clock::now();
  double statsSaveAccumulator = 0.0;

  while (running) {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastTick).count();
    lastTick = now;

    app.player.update();
    checkAutomaticNext(app);
    updateVisualizerAndStats(app, dt);

    statsSaveAccumulator += dt;
    if (statsSaveAccumulator >= 5.0) {
      saveStats(app);
      statsSaveAccumulator = 0.0;
    }

    drawScreen(app);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));

    int key = KeyNone;
    while ((key = readKey()) != KeyNone) {
      if (key >= '1' && key <= '6') {
        app.tab = static_cast<AppTab>(key - '1');
        continue;
      }
      if (key == '\t') {
        app.tab =
            static_cast<AppTab>((static_cast<int>(app.tab) + 1) % kTabCount);
        continue;
      }
      if (key == KeyShiftTab) {
        app.tab = static_cast<AppTab>(
            (static_cast<int>(app.tab) + kTabCount - 1) % kTabCount);
        continue;
      }

      if (key == 'q' || key == KeyEscape) {
        running = false;
        break;
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
      if (key == 'v') {
        app.showVisualizer = !app.showVisualizer;
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
        break;
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
            app.songQueue.save(queueFile.string());
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
    }
  }

  clearTerminalCover();
  app.player.stop();
  app.songQueue.save(queueFile.string());
  saveStats(app);
  return 0;
}
