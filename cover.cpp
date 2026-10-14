#include "cover.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

std::string toLowerStr(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string shellEscape(const std::string &input) {
  std::string out = "'";
  for (char c : input) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

bool isKittyTerminal() {
  const char *kittyId = std::getenv("KITTY_WINDOW_ID");
  const char *term = std::getenv("TERM");
  return (kittyId != nullptr && *kittyId != '\0') ||
         (term != nullptr &&
          std::string(term).find("kitty") != std::string::npos);
}

bool hasCommand(const char *cmd) {
  std::string check = "command -v " + std::string(cmd) + " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}

std::string captureCommand(const std::string &cmd) {
  std::string output;
  std::array<char, 4096> buffer{};
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return output;
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    output += buffer.data();
  }
  pclose(pipe);
  return output;
}

std::string cachedSongPath;
fs::path cachedCoverPath;
std::string cachedAnsiArt;
bool kittyImageDrawn = false;
std::string lastDrawnKittyCover;
int lastTermRows = 0;
int lastTermCols = 0;

void updateCacheIfNeeded(const std::string &songPath) {
  if (songPath == cachedSongPath)
    return;

  cachedSongPath = songPath;
  cachedCoverPath = resolveCoverArt(songPath);
  cachedAnsiArt.clear();
  clearTerminalCover();

  if (!cachedCoverPath.empty() && !isKittyTerminal() && hasCommand("chafa")) {
    std::string cmd = "chafa --format symbols -s 20x10 " +
                      shellEscape(cachedCoverPath.string());
    std::string raw = captureCommand(cmd);
    std::istringstream iss(raw);
    std::string line;
    while (std::getline(iss, line)) {
      cachedAnsiArt += "  " + line + "\033[0m\n";
    }
  }
}

} // namespace

fs::path resolveCoverArt(const fs::path &songPath) {
  if (songPath.empty())
    return {};

  std::error_code ec;
  const fs::path folder = songPath.parent_path();

  for (const auto &entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_regular_file(ec)) {
      std::string ext = toLowerStr(entry.path().extension().string());
      std::string stem = toLowerStr(entry.path().filename().stem().string());
      if ((ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
           ext == ".webp") &&
          (stem == "cover" || stem == "folder" || stem == "album" ||
           stem == "front" || stem == "art")) {
        return entry.path();
      }
    }
  }

  for (const auto &entry : fs::directory_iterator(folder, ec)) {
    if (entry.is_regular_file(ec)) {
      std::string ext = toLowerStr(entry.path().extension().string());
      if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp") {
        return entry.path();
      }
    }
  }

  fs::path tempCover = "/tmp/muzx_cover.jpg";
  fs::remove(tempCover, ec);

  if (hasCommand("ffmpeg")) {
    std::string cmd = "ffmpeg -y -loglevel quiet -i " +
                      shellEscape(songPath.string()) + " -an -vcodec mjpeg " +
                      shellEscape(tempCover.string()) + " >/dev/null 2>&1";
    std::system(cmd.c_str());
    if (fs::exists(tempCover, ec) && fs::file_size(tempCover, ec) > 0) {
      return tempCover;
    }
  }

  return {};
}

void clearTerminalCover() {
  if (kittyImageDrawn && isKittyTerminal()) {
    std::cout << "\033_Ga=d,d=A,q=2\033\\" << std::flush;
  }
  kittyImageDrawn = false;
  lastDrawnKittyCover.clear();
}

void renderPlayerCover(const std::string &songPath, bool showArt,
                       bool isPlayerTab) {
  if (!showArt || !isPlayerTab || songPath.empty())
    return;

  updateCacheIfNeeded(songPath);

  if (cachedCoverPath.empty()) {
    std::cout << "  \033[90m[No embedded or folder cover art found]\033[0m\n";
    return;
  }

  if (isKittyTerminal()) {
    // Exactly 10 lines to match the bigger 20x10 image height
    for (int i = 0; i < 10; ++i)
      std::cout << "\n";
  } else if (!cachedAnsiArt.empty()) {
    std::cout << cachedAnsiArt;
  } else {
    std::cout << "  \033[95m[Cover: " << cachedCoverPath.filename().string()
              << " — install 'chafa' or use Kitty]\033[0m\n";
  }
}

void drawConfirmedKittyCover(const std::string &songPath, bool showArt,
                             bool isPlayerTab) {
  if (!showArt || !isPlayerTab || songPath.empty()) {
    clearTerminalCover();
    return;
  }

  updateCacheIfNeeded(songPath);

  if (cachedCoverPath.empty() || !isKittyTerminal())
    return;

  winsize w{};
  int rows = 24;
  int cols = 80;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
    rows = static_cast<int>(w.ws_row);
    cols = static_cast<int>(w.ws_col);
  }

  bool sizeChanged = (rows != lastTermRows || cols != lastTermCols);
  lastTermRows = rows;
  lastTermCols = cols;

  if (!kittyImageDrawn || lastDrawnKittyCover != cachedCoverPath.string() ||
      sizeChanged) {
    std::cout << "\033_Ga=d,d=A,q=2\033\\" << std::flush;

    // Bigger prominent square: 20 columns wide by 10 rows tall placed at col 2,
    // row 5
    std::string cmd = "kitten icat --silent --stdin=no --transfer-mode=file "
                      "--align left --place 20x10@2x5 " +
                      shellEscape(cachedCoverPath.string()) + " 2>/dev/null";
    std::system(cmd.c_str());
    std::cout << "\033[H" << std::flush;

    kittyImageDrawn = true;
    lastDrawnKittyCover = cachedCoverPath.string();
  }
}
