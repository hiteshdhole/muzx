#include "audio_player.hpp"
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// Key codes above 255 represent terminal escape-sequence keys.
enum Key : int {
  KeyNone = 0,
  KeyEscape = 256,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyShiftTab,
  KeyBackspace,
  KeyEnter
};

termios originalTerminal{};
bool rawModeEnabled = false;
bool alternateScreenEnabled = false;

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

  // An Escape key and arrow keys both start with ESC. Give the rest of
  // an escape sequence a short window to arrive before treating it as ESC.
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

void enableRawMode() {
  if (rawModeEnabled || !isatty(STDIN_FILENO))
    return;
  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return;

  termios terminal = originalTerminal;
  terminal.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
  terminal.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
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

class TerminalSession {
public:
  TerminalSession() {
    enableRawMode();
    std::cout << "\033[?1049h\033[?25l\033[2J\033[H" << std::flush;
    alternateScreenEnabled = true;
  }
  ~TerminalSession() {
    disableRawMode();
    if (alternateScreenEnabled) {
      std::cout << "\033[?25h\033[?1049l" << std::flush;
      alternateScreenEnabled = false;
    }
  }
  TerminalSession(const TerminalSession &) = delete;
  TerminalSession &operator=(const TerminalSession &) = delete;
};

std::string lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isAudioFile(const fs::path &path) {
  const std::string ext = lower(path.extension().string());
  return ext == ".mp3" || ext == ".ogg" || ext == ".oga" || ext == ".wav" ||
         ext == ".flac" || ext == ".aiff" || ext == ".aif";
}

bool pathLess(const fs::path &left, const fs::path &right) {
  const auto l = lower(left.filename().string());
  const auto r = lower(right.filename().string());
  if (l != r)
    return l < r;
  return left.string() < right.string();
}

std::vector<fs::path> audioFilesIn(const fs::path &folder, bool recursive) {
  std::vector<fs::path> songs;
  std::error_code ec;
  if (recursive) {
    fs::recursive_directory_iterator it(
        folder, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
      std::error_code itemError;
      if (it->is_regular_file(itemError) && !itemError &&
          isAudioFile(it->path()))
        songs.push_back(it->path());
      it.increment(ec);
      if (ec)
        ec.clear();
    }
  } else {
    fs::directory_iterator it(
        folder, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    while (!ec && it != end) {
      std::error_code itemError;
      if (it->is_regular_file(itemError) && !itemError &&
          isAudioFile(it->path()))
        songs.push_back(it->path());
      it.increment(ec);
      if (ec)
        ec.clear();
    }
  }
  std::sort(songs.begin(), songs.end(), pathLess);
  return songs;
}

struct FolderEntry {
  fs::path path;
  bool isDirectory = false;
};

std::vector<FolderEntry> folderEntries(const fs::path &folder) {
  std::vector<FolderEntry> dirs;
  std::vector<FolderEntry> files;
  std::error_code ec;
  fs::directory_iterator it(folder,
                            fs::directory_options::skip_permission_denied, ec);
  const fs::directory_iterator end;
  while (!ec && it != end) {
    std::error_code itemError;
    if (it->is_directory(itemError) && !itemError) {
      dirs.push_back({it->path(), true});
    } else {
      itemError.clear();
      if (it->is_regular_file(itemError) && !itemError &&
          isAudioFile(it->path()))
        files.push_back({it->path(), false});
    }
    it.increment(ec);
    if (ec)
      ec.clear();
  }
  auto entryLess = [](const FolderEntry &a, const FolderEntry &b) {
    return pathLess(a.path, b.path);
  };
  std::sort(dirs.begin(), dirs.end(), entryLess);
  std::sort(files.begin(), files.end(), entryLess);
  dirs.insert(dirs.end(), files.begin(), files.end());
  return dirs;
}

std::string formatTime(float seconds) {
  if (seconds < 0)
    seconds = 0;
  const int total = static_cast<int>(seconds);
  std::ostringstream out;
  out << total / 60 << ':' << std::setw(2) << std::setfill('0') << total % 60;
  return out.str();
}

enum class AppTab { Player = 0, Folders = 1, Queue = 2, Search = 3 };

struct QueueItem {
  fs::path path;
  std::uint64_t id = 0;
};

struct PlaybackRecord {
  fs::path path;
  bool fromQueue = false;
  std::uint64_t queueId = 0;
};

struct AppState {
  fs::path musicRoot;
  fs::path browseFolder;
  AppTab tab = AppTab::Player;
  audio_player player;

  bool shuffle = false;
  bool trackLoaded = false;
  bool playing = false;
  bool paused = false;
  bool queuePlayback = false;
  fs::path currentPath;
  std::vector<fs::path> playlist;
  std::optional<std::size_t> playlistIndex;
  std::vector<fs::path> shuffleRemaining;
  std::mt19937 random{std::random_device{}()};

  std::vector<PlaybackRecord> history;
  std::size_t historyIndex = 0;
  bool historyHasCurrent = false;

  std::vector<QueueItem> queue;
  std::size_t queueSelection = 0;
  std::uint64_t nextQueueId = 1;
  std::optional<std::uint64_t> nextQueueAfterRemoval;
  std::string queueFile;

  std::size_t folderSelection = 0;
  std::string searchQuery;
  std::vector<fs::path> allTracks;
  std::vector<fs::path> searchResults;
  std::size_t searchSelection = 0;
  std::string notice = "Ready";
};

void saveQueue(const AppState &app) {
  if (app.queueFile.empty())
    return;
  std::error_code ec;
  fs::create_directories(fs::path(app.queueFile).parent_path(), ec);
  std::ofstream file(app.queueFile, std::ios::trunc);
  if (!file)
    return;
  for (const auto &item : app.queue)
    file << item.path.string() << '\n';
}

void loadQueue(AppState &app) {
  if (app.queueFile.empty())
    return;
  std::ifstream file(app.queueFile);
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    std::error_code ec;
    if (fs::is_regular_file(line, ec) && isAudioFile(line))
      app.queue.push_back({fs::path(line), app.nextQueueId++});
  }
}

void rebuildSearch(AppState &app) {
  app.searchResults.clear();
  const std::string query = lower(app.searchQuery);
  if (query.empty())
    return;
  for (const auto &track : app.allTracks) {
    const std::string name = lower(track.filename().string());
    const std::string full = lower(track.string());
    if (name.find(query) != std::string::npos ||
        full.find(query) != std::string::npos)
      app.searchResults.push_back(track);
  }
  app.searchSelection = 0;
}

void resetShuffleBag(AppState &app) {
  app.shuffleRemaining.clear();
  if (!app.shuffle)
    return;
  for (const auto &path : app.playlist) {
    if (path != app.currentPath)
      app.shuffleRemaining.push_back(path);
  }
  std::shuffle(app.shuffleRemaining.begin(), app.shuffleRemaining.end(),
               app.random);
}

void updatePlaylistIndex(AppState &app) {
  app.playlistIndex.reset();
  for (std::size_t i = 0; i < app.playlist.size(); ++i) {
    if (app.playlist[i] == app.currentPath) {
      app.playlistIndex = i;
      return;
    }
  }
}

bool launchTrack(AppState &app, const fs::path &path, bool fromQueue = false,
                 std::uint64_t queueId = 0, bool addHistory = true) {
  if (!app.player.load(path.string())) {
    app.trackLoaded = false;
    app.playing = false;
    app.paused = false;
    app.notice = "Could not load: " + path.filename().string();
    return false;
  }
  app.player.play();
  app.currentPath = path;
  app.trackLoaded = true;
  app.playing = true;
  app.paused = false;
  app.queuePlayback = fromQueue;

  if (fromQueue) {
    app.historyHasCurrent = true;
  } else {
    updatePlaylistIndex(app);
  }

  if (addHistory) {
    if (app.historyHasCurrent && !app.history.empty() &&
        app.historyIndex + 1 < app.history.size())
      app.history.erase(app.history.begin() +
                            static_cast<std::ptrdiff_t>(app.historyIndex + 1),
                        app.history.end());
    app.history.push_back({path, fromQueue, queueId});
    app.historyIndex = app.history.size() - 1;
    app.historyHasCurrent = true;
  }
  app.notice = "Playing: " + path.filename().string();
  return true;
}

bool startPlaylistTrack(AppState &app, const std::vector<fs::path> &playlist,
                        std::size_t index) {
  if (playlist.empty() || index >= playlist.size())
    return false;
  app.playlist = playlist;
  app.currentPath = playlist[index];
  app.playlistIndex = index;
  app.queuePlayback = false;
  app.nextQueueAfterRemoval.reset();
  resetShuffleBag(app);
  return launchTrack(app, playlist[index], false, 0, true);
}

bool replayRecord(AppState &app, const PlaybackRecord &record) {
  const bool ok =
      launchTrack(app, record.path, record.fromQueue, record.queueId, false);
  if (ok)
    app.notice = "Playing: " + record.path.filename().string();
  return ok;
}

void stopTrack(AppState &app, const std::string &message = "Stopped") {
  app.player.stop();
  app.playing = false;
  app.paused = false;
  app.notice = message;
}

std::optional<std::size_t> queueIndexById(const AppState &app,
                                          std::uint64_t id) {
  for (std::size_t i = 0; i < app.queue.size(); ++i)
    if (app.queue[i].id == id)
      return i;
  return std::nullopt;
}

bool playQueueIndex(AppState &app, std::size_t index) {
  if (index >= app.queue.size())
    return false;
  const auto item = app.queue[index];
  app.queueSelection = index;
  app.nextQueueAfterRemoval.reset();
  return launchTrack(app, item.path, true, item.id, true);
}

bool previousTrack(AppState &app) {
  if (app.historyHasCurrent && !app.history.empty() && app.historyIndex > 0) {
    --app.historyIndex;
    return replayRecord(app, app.history[app.historyIndex]);
  }
  if (app.playlistIndex && *app.playlistIndex > 0) {
    return startPlaylistTrack(app, app.playlist, *app.playlistIndex - 1);
  }
  app.notice = "No previous track";
  return false;
}

bool nextPlaylistTrack(AppState &app) {
  if (app.playlist.empty()) {
    app.notice = "No active playlist";
    return false;
  }
  if (app.shuffle) {
    if (app.shuffleRemaining.empty()) {
      resetShuffleBag(app);
    }
    if (app.shuffleRemaining.empty()) {
      app.notice = "Only one track in playlist";
      return false;
    }
    std::uniform_int_distribution<std::size_t> dist(
        0, app.shuffleRemaining.size() - 1);
    const std::size_t chosen = dist(app.random);
    const fs::path next = app.shuffleRemaining[chosen];
    app.shuffleRemaining.erase(app.shuffleRemaining.begin() +
                               static_cast<std::ptrdiff_t>(chosen));
    return launchTrack(app, next, false, 0, true);
  }
  if (!app.playlistIndex)
    updatePlaylistIndex(app);
  if (!app.playlistIndex || *app.playlistIndex + 1 >= app.playlist.size()) {
    stopTrack(app, "End of playlist");
    return false;
  }
  const std::size_t nextIndex = *app.playlistIndex + 1;
  return startPlaylistTrack(app, app.playlist, nextIndex);
}

bool nextTrack(AppState &app) {
  // When the user has gone back in history, forward should revisit that
  // history before generating a brand-new next track.
  if (app.historyHasCurrent && !app.history.empty() &&
      app.historyIndex + 1 < app.history.size()) {
    ++app.historyIndex;
    return replayRecord(app, app.history[app.historyIndex]);
  }

  if (app.queuePlayback) {
    const auto currentIndex =
        queueIndexById(app, app.historyHasCurrent && !app.history.empty()
                                ? app.history[app.historyIndex].queueId
                                : 0);
    if (currentIndex && *currentIndex + 1 < app.queue.size())
      return playQueueIndex(app, *currentIndex + 1);
    if (!currentIndex && app.nextQueueAfterRemoval) {
      const auto nextIndex = queueIndexById(app, *app.nextQueueAfterRemoval);
      if (nextIndex)
        return playQueueIndex(app, *nextIndex);
    }
    stopTrack(app, "Queue finished");
    return false;
  }
  return nextPlaylistTrack(app);
}

void automaticNext(AppState &app) {
  if (!app.trackLoaded || !app.playing || app.paused ||
      !app.player.isFinished())
    return;
  app.playing = false;
  if (app.queuePlayback) {
    nextTrack(app);
    return;
  }
  // Folder/search playback automatically advances through its active list.
  // At a shuffle-cycle boundary the bag is refilled with every track except
  // the track that just ended, so a cycle never repeats a track early.
  nextPlaylistTrack(app);
}

void togglePause(AppState &app) {
  if (!app.trackLoaded) {
    if (!app.playlist.empty()) {
      startPlaylistTrack(app, app.playlist, 0);
    } else if (!app.allTracks.empty()) {
      startPlaylistTrack(app, app.allTracks, 0);
    } else {
      app.notice = "No songs available";
    }
    return;
  }
  if (app.playing) {
    app.player.pause();
    app.playing = false;
    app.paused = true;
    app.notice = "Paused";
  } else if (app.paused) {
    app.player.resume();
    app.playing = true;
    app.paused = false;
    app.notice = "Playing: " + app.currentPath.filename().string();
  } else {
    const PlaybackRecord current{app.currentPath, app.queuePlayback,
                                 app.historyHasCurrent && !app.history.empty()
                                     ? app.history[app.historyIndex].queueId
                                     : 0};
    replayRecord(app, current);
  }
}

void addQueuePath(AppState &app, const fs::path &path) {
  app.queue.push_back({path, app.nextQueueId++});
  if (app.queuePlayback &&
      !queueIndexById(app, app.historyHasCurrent && !app.history.empty()
                               ? app.history[app.historyIndex].queueId
                               : 0) &&
      !app.nextQueueAfterRemoval) {
    app.nextQueueAfterRemoval = app.queue.back().id;
  }
}

std::size_t addFolderToQueue(AppState &app, const fs::path &folder) {
  auto songs = audioFilesIn(folder, true);
  if (app.shuffle)
    std::shuffle(songs.begin(), songs.end(), app.random);
  for (const auto &song : songs)
    addQueuePath(app, song);
  if (!songs.empty()) {
    app.notice = "Added " + std::to_string(songs.size()) + " songs to Queue";
    saveQueue(app);
  } else {
    app.notice = "No supported songs in folder";
  }
  return songs.size();
}

void removeQueueSelection(AppState &app) {
  if (app.queue.empty() || app.queueSelection >= app.queue.size()) {
    app.notice = "Queue is empty";
    return;
  }
  const std::size_t removedIndex = app.queueSelection;
  const std::uint64_t removedId = app.queue[removedIndex].id;
  const bool removingCurrent =
      app.queuePlayback && app.historyHasCurrent && !app.history.empty() &&
      app.history[app.historyIndex].queueId == removedId;

  app.queue.erase(app.queue.begin() +
                  static_cast<std::ptrdiff_t>(removedIndex));
  if (removingCurrent) {
    if (removedIndex < app.queue.size())
      app.nextQueueAfterRemoval = app.queue[removedIndex].id;
    else
      app.nextQueueAfterRemoval.reset();
    app.notice = "Removed from Queue; current playback continues";
  } else {
    app.notice = "Removed from Queue";
  }
  if (app.queue.empty())
    app.queueSelection = 0;
  else if (app.queueSelection >= app.queue.size())
    app.queueSelection = app.queue.size() - 1;
  saveQueue(app);
}

void clearQueue(AppState &app) {
  app.queue.clear();
  app.queueSelection = 0;
  app.nextQueueAfterRemoval.reset();
  saveQueue(app);
  if (app.queuePlayback && app.trackLoaded && (app.playing || app.paused))
    app.notice = "Queue cleared; current song will finish, then stop";
  else
    app.notice = "Queue cleared";
}

void openSearchPrompt(AppState &app) {
  disableRawMode();
  std::cout << "\033[?25h\nSearch song name or path: " << std::flush;
  std::string query;
  std::getline(std::cin, query);
  app.searchQuery = query;
  rebuildSearch(app);
  enableRawMode();
  app.tab = AppTab::Search;
  app.notice = query.empty()
                   ? "Search cleared"
                   : "Found " + std::to_string(app.searchResults.size()) +
                         " matching songs";
}

void moveSelection(std::size_t &selection, std::size_t count, int delta) {
  if (count == 0) {
    selection = 0;
    return;
  }
  if (selection >= count)
    selection = count - 1;
  if (delta < 0) {
    selection = selection == 0 ? count - 1 : selection - 1;
  } else if (delta > 0) {
    selection = selection + 1 >= count ? 0 : selection + 1;
  }
}

std::string tabName(AppTab tab) {
  switch (tab) {
  case AppTab::Player:
    return "Player";
  case AppTab::Folders:
    return "Folders";
  case AppTab::Queue:
    return "Queue";
  case AppTab::Search:
    return "Search";
  }
  return "Player";
}

std::string relativeDisplay(const fs::path &root, const fs::path &path) {
  std::error_code ec;
  const auto relative = fs::relative(path, root, ec);
  return ec ? path.string() : relative.string();
}

void appendTabs(std::ostringstream &out, AppTab active) {
  out << "MUZX  ";
  for (AppTab tab :
       {AppTab::Player, AppTab::Folders, AppTab::Queue, AppTab::Search}) {
    if (tab == active)
      out << '[' << tabName(tab) << "]  ";
    else
      out << tabName(tab) << "  ";
  }
  out << "\n-------------------------------------------------------------------"
         "-------------\n";
}

void appendPlayerView(std::ostringstream &out, const AppState &app) {
  out << "PLAYER\n\n";
  out << "Track: "
      << (app.trackLoaded ? app.currentPath.filename().string()
                          : "(nothing playing)")
      << '\n';
  if (app.trackLoaded) {
    std::string status =
        app.playing ? "Playing" : (app.paused ? "Paused" : "Stopped");
    out << "Status: " << status << "\n";
    const float pos = app.player.getPlayingOffset().asSeconds();
    const float duration = app.player.getDuration().asSeconds();
    const int barWidth = 36;
    int filled =
        duration > 0 ? static_cast<int>((pos / duration) * barWidth) : 0;
    filled = std::clamp(filled, 0, barWidth);
    out << "Progress: [";
    for (int i = 0; i < barWidth; ++i)
      out << (i < filled ? '#' : '-');
    out << "] " << formatTime(pos) << " / " << formatTime(duration) << '\n';
  }
  out << "Volume: " << static_cast<int>(app.player.getVolume()) << "%\n";
  out << "Shuffle: " << (app.shuffle ? "ON" : "OFF") << "\n";
  out << "Active playlist: " << app.playlist.size() << " songs\n";
  out << "Queue: " << app.queue.size() << " songs\n\n";
  out << "Space  Play/Pause   x Stop   s Shuffle   +/- Volume\n";
  out << "Left/Right  Seek 5 sec (within 3 sec of an edge = Previous/Next)\n";
  out << "h Previous   l Next   n Next   b Previous\n";
}

void appendFoldersView(std::ostringstream &out, AppState &app) {
  const auto entries = folderEntries(app.browseFolder);
  if (app.folderSelection >= entries.size())
    app.folderSelection = entries.empty() ? 0 : entries.size() - 1;
  out << "FOLDERS\nPath: " << relativeDisplay(app.musicRoot, app.browseFolder)
      << "\n\n";
  if (entries.empty())
    out << "This folder has no supported audio files or subfolders.\n";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    out << (i == app.folderSelection ? "> " : "  ");
    out << (entries[i].isDirectory ? "[DIR]  " : "[SONG] ")
        << entries[i].path.filename().string();
    if (!entries[i].isDirectory && app.trackLoaded &&
        entries[i].path == app.currentPath)
      out << "  <PLAYING>";
    out << '\n';
  }
  out << "\nj/k move | Enter or l open folder / play song | h or Backspace "
         "parent\n";
  out << "e add song, or add folder + all subfolders to Queue\n";
}

void appendQueueView(std::ostringstream &out, AppState &app) {
  if (app.queueSelection >= app.queue.size())
    app.queueSelection = app.queue.empty() ? 0 : app.queue.size() - 1;
  out << "QUEUE\n\n";
  if (app.queue.empty())
    out << "Queue is empty. Add songs from Folders or Search with e.\n";
  for (std::size_t i = 0; i < app.queue.size(); ++i) {
    out << (i == app.queueSelection ? "> " : "  ") << i + 1 << ". "
        << app.queue[i].path.filename().string();
    if (app.trackLoaded && app.queuePlayback && app.historyHasCurrent &&
        !app.history.empty() &&
        app.history[app.historyIndex].queueId == app.queue[i].id)
      out << "  <PLAYING>";
    out << '\n';
  }
  out << "\nj/k select | h/l play previous/next Queue song | Enter play "
         "selected\n";
  out << "r remove selected | c clear Queue (current queued song finishes "
         "first)\n";
}

void appendSearchView(std::ostringstream &out, AppState &app) {
  if (app.searchSelection >= app.searchResults.size())
    app.searchSelection =
        app.searchResults.empty() ? 0 : app.searchResults.size() - 1;
  out << "SEARCH\nQuery: "
      << (app.searchQuery.empty() ? "(press / to search)" : app.searchQuery)
      << "\n";
  out << "Results: " << app.searchResults.size() << "\n\n";
  if (app.searchResults.empty())
    out << "No results. Press / to enter a search.\n";
  for (std::size_t i = 0; i < app.searchResults.size(); ++i) {
    out << (i == app.searchSelection ? "> " : "  ")
        << app.searchResults[i].filename().string() << "  ["
        << relativeDisplay(app.musicRoot, app.searchResults[i].parent_path())
        << "]";
    if (app.trackLoaded && app.searchResults[i] == app.currentPath)
      out << "  <PLAYING>";
    out << '\n';
  }
  out << "\n/ enter search | j/k move | Enter play selected | e add selected "
         "to Queue\n";
}

void render(AppState &app) {
  std::ostringstream out;
  appendTabs(out, app.tab);
  switch (app.tab) {
  case AppTab::Player:
    appendPlayerView(out, app);
    break;
  case AppTab::Folders:
    appendFoldersView(out, app);
    break;
  case AppTab::Queue:
    appendQueueView(out, app);
    break;
  case AppTab::Search:
    appendSearchView(out, app);
    break;
  }
  out << "\n-------------------------------------------------------------------"
         "-------------\n";
  out << "Tab next | Shift+Tab previous | q/Esc quit | s shuffle | +/- "
         "volume\n";
  out << "Message: " << app.notice << "\n";
  std::cout << "\033[H" << out.str() << "\033[J" << std::flush;
}

void enterSelectedFolderItem(AppState &app) {
  const auto entries = folderEntries(app.browseFolder);
  if (entries.empty() || app.folderSelection >= entries.size())
    return;
  const auto selected = entries[app.folderSelection];
  if (selected.isDirectory) {
    app.browseFolder = selected.path;
    app.folderSelection = 0;
    app.notice = "Opened folder: " + selected.path.filename().string();
    return;
  }
  const auto songs = audioFilesIn(app.browseFolder, false);
  auto found = std::find(songs.begin(), songs.end(), selected.path);
  const std::size_t index =
      found == songs.end() ? 0
                           : static_cast<std::size_t>(found - songs.begin());
  startPlaylistTrack(app, songs, index);
}

void folderParent(AppState &app) {
  if (app.browseFolder == app.musicRoot) {
    app.notice = "Already at the music root";
    return;
  }
  app.browseFolder = app.browseFolder.parent_path();
  if (app.browseFolder.empty())
    app.browseFolder = app.musicRoot;
  app.folderSelection = 0;
  app.notice = "Parent folder";
}

void addSelectedToQueue(AppState &app) {
  if (app.tab == AppTab::Player) {
    if (app.trackLoaded) {
      addQueuePath(app, app.currentPath);
      saveQueue(app);
      app.notice = "Current song added to Queue";
    } else
      app.notice = "No current song to add";
  } else if (app.tab == AppTab::Folders) {
    const auto entries = folderEntries(app.browseFolder);
    if (entries.empty() || app.folderSelection >= entries.size()) {
      app.notice = "Nothing selected";
      return;
    }
    const auto selected = entries[app.folderSelection];
    if (selected.isDirectory) {
      addFolderToQueue(app, selected.path);
    } else {
      addQueuePath(app, selected.path);
      saveQueue(app);
      app.notice = "Added to Queue: " + selected.path.filename().string();
    }
  } else if (app.tab == AppTab::Search) {
    if (app.searchResults.empty() ||
        app.searchSelection >= app.searchResults.size()) {
      app.notice = "No search result selected";
      return;
    }
    addQueuePath(app, app.searchResults[app.searchSelection]);
    saveQueue(app);
    app.notice = "Added to Queue: " +
                 app.searchResults[app.searchSelection].filename().string();
  } else {
    app.notice = "Use Folders or Search to add songs to Queue";
  }
}

void enterQueueSelection(AppState &app) {
  if (app.queue.empty() || app.queueSelection >= app.queue.size()) {
    app.notice = "Queue is empty";
    return;
  }
  playQueueIndex(app, app.queueSelection);
}

void seekLeft(AppState &app) {
  if (!app.trackLoaded)
    return;
  const float current = app.player.getPlayingOffset().asSeconds();
  if (current <= 3.0F) {
    if (!previousTrack(app))
      app.player.seekBackward(5.0F);
  } else {
    app.player.seekBackward(5.0F);
    app.notice = "Seeked backward 5 seconds";
  }
}

void seekRight(AppState &app) {
  if (!app.trackLoaded)
    return;
  const float current = app.player.getPlayingOffset().asSeconds();
  const float duration = app.player.getDuration().asSeconds();
  if (duration > 0.0F && duration - current <= 3.0F) {
    nextTrack(app);
  } else {
    app.player.seekForward(5.0F);
    app.notice = "Seeked forward 5 seconds";
  }
}

void handleKey(AppState &app, int key, bool &shouldQuit) {
  if (key == KeyNone)
    return;
  if (key == 'q' || key == KeyEscape) {
    shouldQuit = true;
    return;
  }
  if (key == '\t') {
    app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 1) % 4);
    return;
  }
  if (key == KeyShiftTab) {
    app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 3) % 4);
    return;
  }
  if (key == 's') {
    app.shuffle = !app.shuffle;
    resetShuffleBag(app);
    app.notice =
        std::string("Shuffle ") + (app.shuffle ? "enabled" : "disabled");
    return;
  }
  if (key == '+' || key == '=') {
    app.player.increaseVolume();
    app.notice = "Volume increased";
    return;
  }
  if (key == '-') {
    app.player.decreaseVolume();
    app.notice = "Volume decreased";
    return;
  }
  if (key == ' ') {
    togglePause(app);
    return;
  }
  if (key == 'x') {
    stopTrack(app);
    return;
  }
  if (key == KeyLeft) {
    seekLeft(app);
    return;
  }
  if (key == KeyRight) {
    seekRight(app);
    return;
  }
  if (key == 'n' || (app.tab == AppTab::Player && key == 'l')) {
    nextTrack(app);
    return;
  }
  if (key == 'b' || (app.tab == AppTab::Player && key == 'h')) {
    previousTrack(app);
    return;
  }
  if (key == '/') {
    openSearchPrompt(app);
    return;
  }

  if (app.tab == AppTab::Folders) {
    const auto entries = folderEntries(app.browseFolder);
    if (key == 'j' || key == KeyDown)
      moveSelection(app.folderSelection, entries.size(), 1);
    else if (key == 'k' || key == KeyUp)
      moveSelection(app.folderSelection, entries.size(), -1);
    else if (key == 'h' || key == KeyBackspace)
      folderParent(app);
    else if (key == 'l' || key == KeyEnter)
      enterSelectedFolderItem(app);
    else if (key == 'e')
      addSelectedToQueue(app);
  } else if (app.tab == AppTab::Queue) {
    if (key == 'j' || key == KeyDown)
      moveSelection(app.queueSelection, app.queue.size(), 1);
    else if (key == 'k' || key == KeyUp)
      moveSelection(app.queueSelection, app.queue.size(), -1);
    else if (key == KeyEnter)
      enterQueueSelection(app);
    else if (key == 'l') {
      if (app.queue.empty())
        app.notice = "Queue is empty";
      else {
        std::size_t target = app.queueSelection;
        if (app.queuePlayback && app.historyHasCurrent &&
            !app.history.empty()) {
          auto idx = queueIndexById(app, app.history[app.historyIndex].queueId);
          if (idx)
            target = *idx + 1;
        } else if (target + 1 < app.queue.size())
          ++target;
        if (target < app.queue.size())
          playQueueIndex(app, target);
        else
          app.notice = "No next Queue song";
      }
    } else if (key == 'h') {
      if (app.queue.empty())
        app.notice = "Queue is empty";
      else {
        std::size_t target = app.queueSelection;
        if (app.queuePlayback && app.historyHasCurrent &&
            !app.history.empty()) {
          auto idx = queueIndexById(app, app.history[app.historyIndex].queueId);
          if (idx && *idx > 0)
            target = *idx - 1;
          else {
            app.notice = "No previous Queue song";
            return;
          }
        } else if (target > 0)
          --target;
        if (target < app.queue.size())
          playQueueIndex(app, target);
        else
          app.notice = "No previous Queue song";
      }
    } else if (key == 'r')
      removeQueueSelection(app);
    else if (key == 'c')
      clearQueue(app);
  } else if (app.tab == AppTab::Search) {
    if (key == 'j' || key == KeyDown)
      moveSelection(app.searchSelection, app.searchResults.size(), 1);
    else if (key == 'k' || key == KeyUp)
      moveSelection(app.searchSelection, app.searchResults.size(), -1);
    else if (key == KeyEnter) {
      if (app.searchResults.empty() ||
          app.searchSelection >= app.searchResults.size())
        app.notice = "No search result selected";
      else
        startPlaylistTrack(app, app.searchResults, app.searchSelection);
    } else if (key == 'e')
      addSelectedToQueue(app);
  } else if (app.tab == AppTab::Player) {
    // Player controls are global above; h/l select previous/next playback.
  }
}

} // namespace

int main() {
  config settings;
  std::string musicFolder = settings.get_music_folder();
  if (musicFolder.empty()) {
    std::cout << "Enter your music folder: ";
    std::getline(std::cin, musicFolder);
    if (musicFolder.empty()) {
      std::cerr << "Music folder cannot be empty.\n";
      return 1;
    }
    if (!settings.save_music_folder(musicFolder)) {
      std::cerr << "Could not save music folder.\n";
      return 1;
    }
  }

  std::error_code ec;
  fs::path musicRoot = fs::weakly_canonical(fs::path(musicFolder), ec);
  if (ec || !fs::is_directory(musicRoot)) {
    std::cerr << "Music folder does not exist or is not a directory: "
              << musicFolder << '\n';
    return 1;
  }

  AppState app;
  app.musicRoot = musicRoot;
  app.browseFolder = musicRoot;
  const char *home = std::getenv("HOME");
  if (home != nullptr)
    app.queueFile =
        (fs::path(home) / ".config" / "muzx" / "queue.txt").string();
  app.allTracks = audioFilesIn(app.musicRoot, true);
  loadQueue(app);
  if (app.allTracks.empty())
    app.notice = "No supported audio files found under the music folder";

  TerminalSession terminal;
  bool shouldQuit = false;
  auto lastDraw = Clock::now() - std::chrono::milliseconds(250);

  while (!shouldQuit) {
    automaticNext(app);
    const auto now = Clock::now();
    if (now - lastDraw >= std::chrono::milliseconds(250)) {
      render(app);
      lastDraw = now;
    }
    const int key = readKey();
    if (key != KeyNone) {
      handleKey(app, key, shouldQuit);
      lastDraw = Clock::now() - std::chrono::milliseconds(250);
    } else {
      usleep(10000);
    }
  }

  app.player.stop();
  saveQueue(app);
  return 0;
}
