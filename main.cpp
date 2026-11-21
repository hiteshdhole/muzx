// run() and main()
#include "muzx.hpp"

namespace muzx {

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
  app.lastFile = configDirectory / "last.txt";

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

  if (app.resumeMode != 0)
    resumeLastPlayed(app);

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
      saveSettings(app); // picks up changes made with the hot keys (L, T, i, v)
      saveLastPlayed(app);
      statsSaveAccumulator = 0.0;
    }
    if (app.hasCurrentSong && app.currentSong != app.lastSavedSong)
      saveLastPlayed(app); // a new song started

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

  saveLastPlayed(app); // before stopping, so the position is still known
  app.player.stop();
  app.songQueue.save(queueFile.string());
  saveStats(app);
  saveSettings(app);
  return 0;
}

} // namespace muzx

#ifndef MUZX_TEST
int main() {
  // Any exception unwinds through run(), so TerminalSession always restores
  // the terminal before the message is printed.
  try {
    return muzx::run();
  } catch (const std::exception &e) {
    std::cerr << "muzx: fatal error: " << e.what() << "\n";
  } catch (...) {
    std::cerr << "muzx: unknown fatal error\n";
  }
  return 1;
}
#endif // MUZX_TEST
