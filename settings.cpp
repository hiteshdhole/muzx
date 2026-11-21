// Settings tab and the LCD hub rows
#include "muzx.hpp"

namespace muzx {

std::vector<std::string> splitCommas(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Asks the hub for its state (at most every 2 seconds unless forced).
void refreshHub(const AppState &app, bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now - g_hub.asked < std::chrono::seconds(2))
    return;
  g_hub.asked = now;
  std::string reply;
  g_hub.running = app.lcd.query("status", reply, 150);
  if (!g_hub.running)
    return;
  std::istringstream in(reply.substr(0, reply.find(" lcd=")));
  std::string token;
  while (in >> token) {
    const std::size_t eq = token.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = token.substr(0, eq), v = token.substr(eq + 1);
    if (k == "mode") g_hub.mode = v;
    else if (k == "modes") g_hub.modes = splitCommas(v);
    else if (k == "volume") g_hub.volume = v == "1";
    else if (k == "notify") g_hub.notify = v == "1";
  }
}

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
    return "Font size failed: add 'allow_remote_control yes' to kitty.conf, restart Kitty.";
  return "";
}

std::string settingsToText(const AppState &app) {
  std::ostringstream out;
  out << "repeat=" << static_cast<int>(app.repeat) << "\n"
      << "resume=" << app.resumeMode << "\n"
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
  if (values.count("resume"))
    app.resumeMode = std::clamp(std::atoi(values["resume"].c_str()), 0, 2);
  if (values.count("lyrics_mode")) {
    const int mode = std::atoi(values["lyrics_mode"].c_str());
    if (mode >= 0 && mode <= 2)
      app.lyricsMode = static_cast<LyricsMode>(mode);
  }
  flag("album_art", app.showAlbumArt);
  flag("compact", app.compact);
  flag("buttons", app.showButtons);
  flag("notify", app.notify);
  static const char *const colorKeys[4] = {"color_title", "color_accent", "color_highlight", "color_bars"};
  for (int k = 0; k < 4; ++k)
    if (values.count(colorKeys[k]))
      g_custom[k] = std::clamp(std::atoi(values[colorKeys[k]].c_str()), 0, kPaletteCount - 1);
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
    app.fontSize = std::max(0, std::min(72, std::atoi(values["font_size"].c_str())));
  app.settingsSaved = onDisk; // what is on disk now; saveSettings() rewrites only on change
}

// Changes one setting by one step forward (+1) or backward (-1).
void changeSetting(AppState &app, int item, int dir) {
  app.settingsNote.clear();
  switch (item) {
  case SetRepeat:
    app.repeat = static_cast<RepeatMode>(cycleIndex(static_cast<int>(app.repeat), dir, 3));
    break;
  case SetResume:
    app.resumeMode = cycleIndex(app.resumeMode, dir, 3);
    break;
  case SetLyrics:
    app.lyricsMode = static_cast<LyricsMode>(cycleIndex(static_cast<int>(app.lyricsMode), dir, 3));
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
  case SetHubMode:
  case SetHubVolume:
  case SetHubNotify:
  case SetHubLyrics:
  case SetHubCaptions:
  case SetHubNowPlaying:
  case SetHubSpectrum:
  case SetHubClock:
  case SetHubStatus: {
    refreshHub(app, true);
    std::string reply;
    if (!g_hub.running) {
      app.settingsNote = "The LCD hub is not running (start it with: muzxlcd &)";
      break;
    }
    if (item == SetHubMode) {
      app.lcd.query(dir > 0 ? "mode next" : "mode prev", reply);
    } else if (item == SetHubVolume) {
      app.lcd.query(std::string("set volume ") + (g_hub.volume ? "0" : "1"), reply);
    } else if (item == SetHubNotify) {
      app.lcd.query(std::string("set notify ") + (g_hub.notify ? "0" : "1"), reply);
    } else {
      const std::string name = kHubModeNames[item - SetHubLyrics];
      // keep the order the person chose; a mode that is switched on goes in
      // after its usual neighbour
      std::vector<std::string> modes = g_hub.modes;
      const auto at = std::find(modes.begin(), modes.end(), name);
      if (at != modes.end()) {
        modes.erase(at);
      } else {
        auto rank = [](const std::string &m) {
          for (std::size_t i = 0; i < sizeof(kHubModeNames) / sizeof(kHubModeNames[0]); ++i)
            if (m == kHubModeNames[i]) return static_cast<int>(i);
          return 1000;
        };
        // after the nearest earlier mode of the usual order, or at the front
        int best = -1, bestRank = -1;
        for (std::size_t i = 0; i < modes.size(); ++i)
          if (rank(modes[i]) < rank(name) && rank(modes[i]) > bestRank) {
            best = static_cast<int>(i);
            bestRank = rank(modes[i]);
          }
        modes.insert(modes.begin() + (best + 1), name);
      }
      if (modes.empty()) {
        app.settingsNote = "At least one LCD mode has to stay on";
        break;
      }
      std::string list;
      for (const std::string &m : modes) list += (list.empty() ? "" : ",") + m;
      app.lcd.query("set modes " + list, reply);
    }
    refreshHub(app, true);
    break;
  }
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
      app.settingsNote = "notify-send not found: install libnotify (sudo pacman -S libnotify).";
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
    const int n = static_cast<int>(sizeof(kFontChoices) / sizeof(kFontChoices[0]));
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
  std::cout << kPurpleBold << "New music folder (empty = cancel): " << kReset << std::flush;
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
    return app.repeat == RepeatMode::Off ? "Off" : app.repeat == RepeatMode::All ? "All songs" : "One song";
  case SetResume:
    return app.resumeMode == 0 ? "Nothing (start empty)"
           : app.resumeMode == 1 ? "Last song, paused" : "Last song, playing";
  case SetLyrics:
    return app.lyricsMode == LyricsMode::Off ? "Off"
           : app.lyricsMode == LyricsMode::Right ? "Right of the player" : "Below the player";
  case SetPrefetch: {
    if (app.prefetchMode == 0)
      return "Off";
    const lyrics::Manager::PrefetchStatus st = app.lyricsMgr.prefetchStatus();
    return "On   (" + std::to_string(st.processed) + "/" + std::to_string(st.total) +
           " checked, " + std::to_string(st.found) + " found)";
  }
  case SetLcd:
    if (!app.lcd.enabled())
      return "Off";
    if (app.lcd.viaHub())
      return "On (through the LCD hub)";
    return app.lcd.connected() ? "On (connected)" : "On (waiting for the Arduino)";
  case SetLcdKana:
    return app.lcd.kana() ? "Katakana (Japanese ROM)" : "Romaji (any LCD)";
  case SetHubMode:
    refreshHub(app);
    return g_hub.running ? "Showing: " + g_hub.mode + "   (Enter = next)" : "hub not running";
  case SetHubVolume:
    return g_hub.running ? onOff(g_hub.volume) : "hub not running";
  case SetHubNotify:
    return g_hub.running ? onOff(g_hub.notify) : "hub not running";
  case SetHubLyrics:
  case SetHubCaptions:
  case SetHubNowPlaying:
  case SetHubSpectrum:
  case SetHubClock:
  case SetHubStatus:
    return g_hub.running
               ? onOff(std::find(g_hub.modes.begin(), g_hub.modes.end(),
                                 kHubModeNames[item - SetHubLyrics]) != g_hub.modes.end())
               : "hub not running";
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
    return std::string(kPalette[c].name) + "  \033[38;5;" + std::to_string(kPalette[c].code) + "m██\033[39m";
  }
  case SetFont:
    return app.fontSize == 0 ? "terminal default" : std::to_string(app.fontSize);
  case SetFolder:
    return app.musicRoot.string();
  default:
    return "";
  }
}

void drawSettings(const AppState &app) {
  static const char *const labels[kSettingCount] = {
      "Repeat",        "On start",       "Lyrics panel",     "Prepare lyrics", "LCD display",
      "LCD Japanese",  "Hub: show now", "Hub: volume popup", "Hub: notifications",
      "Hub mode: lyrics", "Hub mode: captions", "Hub mode: playing", "Hub mode: spectrum",
      "Hub mode: clock", "Hub mode: status", "Album art",
      "Visualizer",    "Layout",         "Control buttons",  "Notifications",
      "Theme",         "Custom: title",  "Custom: accent",   "Custom: highlight",
      "Custom: bars",  "Font size",      "Music folder"};
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
    if (i == SetRepeat) heading("PLAYBACK");
    else if (i == SetLyrics) heading("LYRICS");
    else if (i == SetLcd) heading("LCD (Arduino)");
    else if (i == SetHubMode) heading("LCD HUB (muzxlcd program)");
    else if (i == SetArt) heading("DISPLAY");
    else if (i == SetTheme) heading("THEME & FONT");
    else if (i == SetFolder) heading("GENERAL");
    std::string label = labels[i];
    label.resize(20, ' ');
    if (static_cast<std::size_t>(i) == selected) {
      selectedLine = static_cast<int>(lines.size());
      lines.push_back({"  " + std::string(kSelected) + " " + label + settingValue(app, i) + " " + kReset, i});
    } else {
      lines.push_back({"    " + label + kPink + settingValue(app, i) + kReset, i});
    }
  }

  // Rows above (4) and below (hint lines, our own footer of 3) are not ours.
  const int available = std::max(4, terminalRows() - 4 - (app.compact ? 0 : 2) - 3);
  const int total = static_cast<int>(lines.size());
  int first = 0;
  if (total > available)
    first = std::clamp(selectedLine - available / 2, 0, total - available);
  const int last = std::min(total, first + available);
  for (int k = first; k < last; ++k) {
    if (lines[static_cast<std::size_t>(k)].item >= 0) {
      const int row = CoutCapture::rowsSoFar() + 1;
      addClick(row, row, 1, 9999, ClkSetting, lines[static_cast<std::size_t>(k)].item);
    }
    std::cout << lines[static_cast<std::size_t>(k)].text << "\n";
  }
  std::cout << "\n  " << kMuted << "j/k move   Enter or →/l next value   ←/h previous value";
  if (total > available)
    std::cout << "   (" << (selected + 1) << "/" << kSettingCount << ")";
  std::cout << kReset << "\n";
  if (!app.settingsNote.empty())
    std::cout << "  " << kGreenBold << app.settingsNote << kReset << "\n";
}

} // namespace muzx
