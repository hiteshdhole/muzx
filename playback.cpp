// playback control, shuffle, resume
#include "muzx.hpp"

namespace muzx {

std::size_t folderMenuSize(const AppState &app) {
  return app.folderEntries.empty() ? 0 : app.folderEntries.size() + 1;
}

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
  if (sh.pos + 1 < sh.history.size()) // we stepped back earlier: go forward again
    return sh.history[++sh.pos];
  if (sh.bag.empty()) { // everything has played: a new round with every song again
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
    shuffleReset(app, app.playlistIndex); // a fresh round starting with this song
}

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index, bool switchToPlayer,
                      bool countPlay) {
  if (songs.empty() || index >= songs.size())
    return false;
  const bool newList = &songs != &app.playlist; // a different list than the current one

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
        if (countPlay) {
          app.playCounts[path]++;
          app.statsDirty = true;
          saveStats(app);
        }
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

bool playFolder(AppState &app, const fs::path &folder, bool recursive) {
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

// ---------------------------------------------------------------------------
// Start with the last played song: ~/.config/muzx/last.txt holds its path and
// the position (two lines). Written when the song changes, every few seconds
// and on quit.
// ---------------------------------------------------------------------------
void saveLastPlayed(AppState &app) {
  if (app.lastFile.empty() || !app.hasCurrentSong || app.currentSong.empty())
    return;
  double position = static_cast<double>(app.player.getPlayingOffset().asSeconds());
  if (!std::isfinite(position) || position < 0.0)
    position = 0.0;
  fs::path tmp = app.lastFile;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      return;
    out << app.currentSong << "\n" << static_cast<long long>(position) << "\n";
    out.flush();
    if (!out)
      return;
  }
  std::error_code ec;
  fs::rename(tmp, app.lastFile, ec);
  app.lastSavedSong = app.currentSong;
}

// Returns true when the last song was loaded.
bool resumeLastPlayed(AppState &app) {
  std::ifstream in(app.lastFile);
  std::string song;
  long long position = 0;
  if (!std::getline(in, song) || song.empty())
    return false;
  in >> position;
  std::error_code ec;
  if (!fs::is_regular_file(song, ec))
    return false; // moved or deleted: just start normally

  // The playlist is the folder of that song, like opening it from the browser.
  std::vector<std::string> songs = audioFilesIn(fs::path(song).parent_path(), false);
  std::size_t index = 0;
  const auto found = std::find(songs.begin(), songs.end(), song);
  if (found == songs.end())
    songs = {song};
  else
    index = static_cast<std::size_t>(std::distance(songs.begin(), found));
  if (!playFromPlaylist(app, songs, index, true, false))
    return false;

  // Continue where it stopped (not when it was almost over).
  const double duration = static_cast<double>(app.player.getDuration().asSeconds());
  if (position > 3 && (!(duration > 0.0) || static_cast<double>(position) < duration - 10.0))
    app.player.seekForward(static_cast<float>(position));
  if (app.resumeMode == 1) {
    app.player.pause();
    app.paused = true;
  }
  app.lastSavedSong = app.currentSong;
  return true;
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

} // namespace muzx
