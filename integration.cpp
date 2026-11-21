// lyric offsets, MPRIS media keys, notifications
#include "muzx.hpp"

extern char **environ; // for posix_spawn (notifications)

namespace muzx {

// "SpotiDownloader.com - Title - Artist.mp3" -> title and artist.
void splitSongName(const std::string &path, std::string &title, std::string &artist) {
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
  app.lyricShift = delta == 0.0 ? 0.0 : std::clamp(app.lyricShift + delta, -60.0, 60.0);
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
    if (std::fabs(after - target) >= distance) { // stuck, or stepped past the target
      if (std::fabs(after - target) > distance) { // went past: undo that last step
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

  const bool stopped = !app.hasCurrentSong || (!app.playbackRunning && !app.paused);
  MprisState state;
  state.status = stopped ? "Stopped" : (app.paused ? "Paused" : "Playing");
  state.hasTrack = app.hasCurrentSong;
  state.trackId = trackId;
  if (app.hasCurrentSong) {
    splitSongName(app.currentSong, state.title, state.artist);
    float duration = app.player.getDuration().asSeconds();
    float position = app.player.getPlayingOffset().asSeconds();
    if (!std::isfinite(duration) || duration < 0.0F) duration = 0.0F;
    if (!std::isfinite(position) || position < 0.0F) position = 0.0F;
    state.lengthUs = static_cast<std::int64_t>(duration * 1e6F);
    state.positionUs = static_cast<std::int64_t>(position * 1e6F);
  }
  state.volume = std::clamp(static_cast<double>(app.player.getVolume()) / 100.0, 0.0, 1.0);
  state.shuffle = app.shuffle;
  state.loop = app.repeat == RepeatMode::One ? "Track" : app.repeat == RepeatMode::All ? "Playlist" : "None";
  state.canNext = state.canPrevious = app.hasCurrentSong && !app.playlist.empty();
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
      app.repeat = cmd.value >= 2.0 ? RepeatMode::One : cmd.value >= 1.0 ? RepeatMode::All : RepeatMode::Off;
      break;
    case MprisCmd::Quit:
      return true;
    }
  }
  return false;
}

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
  std::vector<std::string> args = {"notify-send", "-a", "MUZX", "-t", "4000",
                                   "-h", "string:x-canonical-private-synchronous:muzx",
                                   "--", title, body};
  std::vector<char *> argv;
  for (std::string &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions); // keep its output off the screen
  for (int fd = 0; fd <= 2; ++fd)
    posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0);
  pid_t pid = 0;
  if (posix_spawnp(&pid, "notify-send", &actions, nullptr, argv.data(), environ) == 0)
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

} // namespace muzx
