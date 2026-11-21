// lyrics panel, LCD / prefetch updates
#include "muzx.hpp"

namespace muzx {

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
  std::size_t lastSpaceByte = std::string::npos; // position of last space in cur
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
  const std::string key = std::to_string(app.prefetchMode) + "|" + app.currentSong + "|" +
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

} // namespace muzx
