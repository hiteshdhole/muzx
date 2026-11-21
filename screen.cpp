// player buttons and the whole screen
#include "muzx.hpp"

namespace muzx {

int rightPanelColumn(const AppState &app, int cols) {
  if (app.lyricsMode != LyricsMode::Right || cols < kLyricsMinCols)
    return 0;
  return cols >= 140 ? 88 : 82;
}

std::string playerButtons(const AppState &app, int rows, int cols) {
  if (!app.showButtons || app.tab != AppTab::Player || !app.hasCurrentSong || g_buttonsRow <= 0)
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
      {app.repeat == RepeatMode::One ? "↻1" : "↻", app.repeat == RepeatMode::One ? 2 : 1,
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
    out += at(top + 1, col) + b.color + "│" + std::string(static_cast<std::size_t>(left), ' ') +
           b.glyph + std::string(static_cast<std::size_t>(right), ' ') + b.color + "│";
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
          // (short enough to stay left of the lyrics panel, which starts at column 82)
          << "  Tab/1-7 Tabs  |  Space Play/Pause  |  i Art  |  v Vis  |  L Lyrics  |  "
             "q Quit"
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

} // namespace muzx
