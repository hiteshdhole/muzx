// drawing the tabs
#include "muzx.hpp"

namespace muzx {

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
                         "4 Search", "5 Status",  "6 Keys", "7 Settings"};

  const int tabRow = CoutCapture::rowsSoFar() + 1;
  int tabCol = 7; // after "MUZX  "
  for (int i = 0; i < kTabCount; ++i) {
    AppTab tab = static_cast<AppTab>(i);
    const int tabWidth = static_cast<int>(std::strlen(names[i])) + (tab == active ? 2 : 0);
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

    const std::string timeText =
        "  Time:    " + formatTime(position) + " / " + formatTime(duration) + "   |   ";
    const std::string volumeText =
        "Volume: " + std::to_string(static_cast<int>(app.player.getVolume())) + "%";
    {
      const int timeRow = CoutCapture::rowsSoFar() + 1;
      addClick(timeRow, timeRow, static_cast<int>(timeText.size()) + 1,
               static_cast<int>(timeText.size() + volumeText.size()), ClkVolume, 0);
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
  row("m  /  M", "LCD mode: next / previous (lyrics, clock, PC status ... needs muzxlcd)");
  row("[  /  ]  /  \\", "Lyrics earlier / later by 0.5 s (saved per song) / reset");
  row("+ / -", "Adjust volume");
  row("x", "Stop playback");
  row("Mouse", "Click tabs, buttons, list rows; drag the progress bar to seek;");
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

} // namespace muzx
