// colours, keys, mouse, raw terminal, input reading
#include "muzx.hpp"

namespace muzx {

void applyTheme(int index) {
  index = ((index % kThemeCount) + kThemeCount) % kThemeCount;
  if (index == kCustomTheme) {
    auto code = [](int slot, bool bold) {
      const int c = kPalette[((g_custom[slot] % kPaletteCount) + kPaletteCount) % kPaletteCount].code;
      return std::string(bold ? "\033[1;38;5;" : "\033[38;5;") + std::to_string(c) + "m";
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

bool mouseWanted() {
  const char *env = std::getenv("MUZX_MOUSE");
  return !(env && std::string(env) == "0");
}

void onSignal(int) { quitSignal = 1; }

void installSignalHandlers() {
  struct sigaction sa {};
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

void addClick(int row0, int row1, int col0, int col1, int action, int arg) {
  g_clicks.push_back({row0, row1, col0, col1, action, arg});
}

int terminalRows() {
  winsize w{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 1)
    return static_cast<int>(w.ws_row);
  return 24;
}

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

} // namespace muzx
