// Unit tests of the player's helper code (text layout, shuffle, settings, hub rows).
#include "../muzx.hpp"
#include "check.hpp"
#include <sys/socket.h>
#include <sys/un.h>

using namespace muzx;

static void testTextLayout() {
  CHECK_EQ(charColumns(U'a'), 1);
  CHECK_EQ(charColumns(0x65E5), 2);   // 日
  CHECK_EQ(charColumns(0x0301), 0);   // combining accent
  CHECK_EQ(displayWidth("ab"), static_cast<std::size_t>(2));
  CHECK_EQ(displayWidth("\xE6\x97\xA5\xE6\x9C\xAC" "a"), static_cast<std::size_t>(5)); // 日本a
  CHECK_EQ(clipColumns("abcdef", 3), std::string("abc"));
  CHECK_EQ(clipColumns("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 5), std::string("\xE6\x97\xA5\xE6\x9C\xAC")); // 日本語 in 5 cols
  CHECK_EQ(visualLength("h\xC3\xA9llo"), static_cast<std::size_t>(5)); // characters, not bytes

  const auto rows = wrapLyric("one two three four five six", 9);
  CHECK(rows.size() >= 3);
  std::string joined;
  for (const std::string &r : rows) {
    CHECK(displayWidth(r) <= 9);
    joined += (joined.empty() ? "" : " ") + r;
  }
  CHECK_EQ(joined, std::string("one two three four five six"));
  const auto longWord = wrapLyric("abcdefghijklmnopqrstuvwxyz", 10);
  CHECK_EQ(longWord.size(), static_cast<std::size_t>(3)); // broken where there is no space
  CHECK(displayWidth(longWord[0]) <= 10);
  const auto wide = wrapLyric("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE6\x97\xA5\xE6\x9C\xAC", 6); // 5 wide characters in 6 columns
  for (const std::string &r : wide)
    CHECK(displayWidth(r) <= 6);
  CHECK_EQ(wrapLyric("", 10).size(), static_cast<std::size_t>(1)); // an empty line still takes a row
  CHECK(wrapLyric("x", 0).empty());
  CHECK_EQ(wrapLyric("   padded", 20)[0], std::string("padded")); // no leading spaces
}

static void testHelpers() {
  CHECK_EQ(formatDurationHMS(3725), std::string("1h 02m 05s"));
  CHECK_EQ(formatDurationHMS(-5), std::string("0h 00m 00s"));
  CHECK_EQ(lowerCase("AbC"), std::string("abc"));
  CHECK_EQ(trim("  x  "), std::string("x"));
  CHECK_EQ(fileName("/a/b/song.mp3"), std::string("song.mp3"));
  CHECK_EQ(fileName(""), std::string(""));
  CHECK(isAudioFile("x.MP3") && isAudioFile("y.flac") && !isAudioFile("z.txt"));
  setenv("HOME", "/home/someone", 1);
  CHECK_EQ(normalizeFolder("~/Music").string(), std::string("/home/someone/Music"));
  CHECK_EQ(normalizeFolder("  \"/a/b/\"  ").string(), std::string("/a/b"));
  CHECK_EQ(normalizeFolder("/a/../b").string(), std::string("/b"));
  CHECK(normalizeFolder("   ").empty());
  CHECK_EQ(cycleIndex(0, -1, 3), 2);
  CHECK_EQ(cycleIndex(2, 1, 3), 0);
  const auto parts = splitCommas("a,b,,c,");
  CHECK_EQ(parts.size(), static_cast<std::size_t>(3));
  CHECK_EQ(parts[2], std::string("c"));
}

static void testShuffle() {
  AppState app;
  for (int i = 0; i < 6; ++i)
    app.playlist.push_back("/x/song" + std::to_string(i) + ".mp3");
  app.rng.seed(1234);
  app.playlistIndex = 2;
  setShuffle(app, true);
  // one round: every song once, never the same twice in a row
  std::vector<std::size_t> seen{app.playlistIndex};
  for (int step = 0; step < 5; ++step) {
    const std::size_t next = shuffleNextIndex(app);
    CHECK(next != app.playlistIndex);
    app.playlistIndex = next;
    seen.push_back(next);
  }
  std::vector<std::size_t> sorted = seen;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  CHECK_EQ(sorted.size(), static_cast<std::size_t>(6)); // all six played, none twice
  // stepping back goes through the history, forward replays it
  const std::size_t last = app.playlistIndex;
  const std::size_t back1 = shufflePreviousIndex(app);
  CHECK_EQ(back1, seen[4]);
  app.playlistIndex = back1;
  const std::size_t back2 = shufflePreviousIndex(app);
  CHECK_EQ(back2, seen[3]);
  app.playlistIndex = back2;
  CHECK_EQ(shuffleNextIndex(app), seen[4]);
  app.playlistIndex = seen[4];
  CHECK_EQ(shuffleNextIndex(app), last);
  app.playlistIndex = last;
  // a new round starts after the bag is empty
  const std::size_t again = shuffleNextIndex(app);
  CHECK(again != last);
  // one song: nothing to shuffle
  AppState single;
  single.playlist = {"/x/only.mp3"};
  CHECK_EQ(shuffleNextIndex(single), static_cast<std::size_t>(0));
}

static void testSettingsFile() {
  char tmpl[] = "/tmp/muzx_test_XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  AppState a;
  a.settingsFile = fs::path(dir) / "settings.conf";
  a.repeat = RepeatMode::All;
  a.resumeMode = 1;
  a.lyricsMode = LyricsMode::Bottom;
  a.showAlbumArt = true;
  a.showVisualizer = true;
  a.compact = true;
  a.notify = true;
  a.fontSize = 16;
  a.theme = 2;
  saveSettings(a);
  const std::string text = settingsToText(a);
  CHECK(text.find("repeat=1") != std::string::npos);
  CHECK(text.find("font_size=16") != std::string::npos);
  AppState b;
  b.settingsFile = a.settingsFile;
  loadSettings(b);
  CHECK(b.repeat == RepeatMode::All);
  CHECK_EQ(b.resumeMode, 1);
  CHECK(b.lyricsMode == LyricsMode::Bottom);
  CHECK(b.showAlbumArt && b.showVisualizer && b.compact && b.notify);
  CHECK_EQ(b.fontSize, 16);
  CHECK_EQ(b.theme, 2);
  // a garbage / hostile file does not crash and keeps sane values
  {
    std::ofstream out(a.settingsFile, std::ios::trunc);
    out << "font_size=9999999\nrepeat=77\ntheme=-4\nnonsense\n=\nresume=abc\n";
  }
  AppState c;
  c.settingsFile = a.settingsFile;
  loadSettings(c);
  CHECK(c.fontSize >= 0 && c.fontSize <= 72);
  CHECK(c.theme >= 0 && c.theme < kThemeCount);
  CHECK(static_cast<int>(c.repeat) >= 0 && static_cast<int>(c.repeat) <= 2);
  fs::remove_all(dir);
}

// A tiny stand-in for muzxlcd: answers "status" and records every command.
struct FakeHub {
  std::string path;
  int fd = -1;
  std::thread th;
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::vector<std::string> commands;
  std::string status = "mode=lyrics modes=lyrics,clock,spectrum all=lyrics,captions,nowplaying,spectrum,clock,status volume=1 notify=0 lcd=/dev/ttyUSB0";

  explicit FakeHub(const std::string &p) : path(p) {
    fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strcpy(addr.sun_path, path.c_str());
    ::unlink(path.c_str());
    ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    th = std::thread([this] {
      while (!stop) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 50) <= 0)
          continue;
        char buf[512];
        sockaddr_un from{};
        socklen_t flen = sizeof(from);
        const ssize_t n = ::recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from), &flen);
        if (n <= 0)
          continue;
        const std::string msg(buf, static_cast<std::size_t>(n));
        std::string reply = "ok";
        {
          std::lock_guard<std::mutex> lock(mu);
          if (msg == "status")
            reply = status;
          else
            commands.push_back(msg);
        }
        if (flen > sizeof(sa_family_t))
          ::sendto(fd, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), flen);
      }
    });
  }
  ~FakeHub() {
    stop = true;
    th.join();
    ::close(fd);
    ::unlink(path.c_str());
  }
  std::vector<std::string> sent() {
    std::lock_guard<std::mutex> lock(mu);
    return commands;
  }
};

static void testHubSettings() {
  char tmpl[] = "/tmp/muzx_hubtest_XXXXXX";
  const std::string dir = mkdtemp(tmpl);
  setenv("MUZXLCD_SOCKET", (dir + "/hub.sock").c_str(), 1);
  AppState app;
  // hub not running: the rows say so and a change shows a note
  refreshHub(app, true);
  CHECK(!g_hub.running);
  CHECK_EQ(settingValue(app, SetHubVolume), std::string("hub not running"));
  changeSetting(app, SetHubVolume, 1);
  CHECK(!app.settingsNote.empty());
  {
    FakeHub hub(dir + "/hub.sock");
    refreshHub(app, true);
    CHECK(g_hub.running);
    CHECK_EQ(g_hub.mode, std::string("lyrics"));
    CHECK_EQ(g_hub.modes.size(), static_cast<std::size_t>(3));
    CHECK(g_hub.volume && !g_hub.notify);
    CHECK_EQ(settingValue(app, SetHubVolume), std::string("On"));
    CHECK_EQ(settingValue(app, SetHubNotify), std::string("Off"));
    CHECK_EQ(settingValue(app, SetHubClock), std::string("On"));
    CHECK_EQ(settingValue(app, SetHubCaptions), std::string("Off"));
    changeSetting(app, SetHubVolume, 1);   // on -> off
    changeSetting(app, SetHubNotify, 1);   // off -> on
    changeSetting(app, SetHubCaptions, 1); // adds captions, in the usual order
    changeSetting(app, SetHubClock, 1);    // removes clock
    changeSetting(app, SetHubMode, 1);
    changeSetting(app, SetHubMode, -1);
    const auto cmds = hub.sent();
    CHECK(std::find(cmds.begin(), cmds.end(), "set volume 0") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "set notify 1") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "set modes lyrics,captions,clock,spectrum") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "set modes lyrics,spectrum") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "mode next") != cmds.end());
    CHECK(std::find(cmds.begin(), cmds.end(), "mode prev") != cmds.end());
    // a custom order is kept: switching a mode on puts it in its usual place, not at the end
    {
      std::lock_guard<std::mutex> lock(hub.mu);
      hub.status = "mode=lyrics modes=status,clock,lyrics all=x volume=1 notify=1 lcd=x";
    }
    refreshHub(app, true);
    changeSetting(app, SetHubNowPlaying, 1);
    {
      const auto c2 = hub.sent();
      CHECK(std::find(c2.begin(), c2.end(), "set modes status,clock,nowplaying,lyrics") == c2.end());
      CHECK(std::find(c2.begin(), c2.end(), "set modes status,clock,lyrics,nowplaying") != c2.end());
    }
    // the last mode cannot be switched off
    {
      std::lock_guard<std::mutex> lock(hub.mu);
      hub.status = "mode=clock modes=clock all=lyrics,captions,nowplaying,spectrum,clock,status volume=1 notify=1 lcd=x";
    }
    refreshHub(app, true);
    app.settingsNote.clear();
    changeSetting(app, SetHubClock, 1);
    CHECK(!app.settingsNote.empty());
  }
  unsetenv("MUZXLCD_SOCKET");
  fs::remove_all(dir);
}

int main() {
  testTextLayout();
  testHelpers();
  testShuffle();
  testSettingsFile();
  testHubSettings();
  TEST_MAIN_END();
}
