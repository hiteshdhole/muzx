// Unit tests of the helper functions in muzxlcd.cpp (no hardware needed).
#define MUZXLCD_TEST
#include "../muzxlcd.cpp"
#include "check.hpp"

static void testText() {
  CHECK_EQ(trim("  hi \t\n"), std::string("hi"));
  CHECK_EQ(lower("AbC"), std::string("abc"));
  const auto parts = split("a,b,,c", ',');
  CHECK(parts.size() >= 3);
  CHECK_EQ(parts[0], std::string("a"));
  CHECK_EQ(fit("abcdefghijklmnopqrst", 16).size(), static_cast<std::size_t>(16));
  CHECK_EQ(fit("abc", 16), std::string("abc"));
  CHECK_EQ(center("abcd", 16), std::string("      abcd"));
  CHECK_EQ(center("", 16), std::string("        "));
  CHECK_EQ(fmtRate(500), std::string("500B"));
  CHECK_EQ(fmtRate(2048), std::string("2K"));
  CHECK_EQ(fmtRate(3 * 1048576.0), std::string("3.0M"));
  const Frame f = rowsFrame("a", "b");
  CHECK(f.rows && f.a == "a" && f.b == "b");
  CHECK(!textFrame("x").rows);
  CHECK(rowsFrame("a", "b") == rowsFrame("a", "b"));
  CHECK(!(rowsFrame("a", "b") == rowsFrame("a", "c")));
}

static void testJson() {
  const auto j = parseJson(R"({"a":"x","n":12.5,"l":[1,2,"three"],"o":{"k":true},"u":"é\n"})");
  CHECK(j.has_value());
  CHECK_EQ(j->getStr("a"), std::string("x"));
  CHECK_NEAR(j->getNum("n"), 12.5, 1e-9);
  CHECK_NEAR(j->getNum("missing", 7), 7, 1e-9);
  CHECK_EQ(j->get("l")->arr.size(), static_cast<std::size_t>(3));
  CHECK_EQ(j->get("l")->arr[2].str, std::string("three"));
  CHECK(j->get("o")->get("k")->b);
  CHECK_EQ(j->getStr("u"), std::string("\xC3\xA9\n")); // é and a newline
  CHECK(!parseJson("{\"a\":").has_value());
  CHECK(!parseJson("{bad}").has_value());
  const auto arr = parseJson(R"([{"syncedLyrics":"[00:01.00] hi"}])");
  CHECK(arr.has_value() && arr->type == Json::Arr && arr->arr.size() == 1);
}

static void testLrc() {
  const auto l = parseLrc("[ar:Someone]\n[ti:Song]\n[00:12.50] Hello\r\n[01:02.00][00:30.25] Twice\n\nno tags here\n[00:05.00]\n");
  CHECK_EQ(l.size(), static_cast<std::size_t>(4));
  CHECK_NEAR(l[0].start, 5.0, 1e-9);        // an empty line is kept (a pause)
  CHECK_EQ(l[0].text, std::string(""));
  CHECK_NEAR(l[1].start, 12.5, 1e-9);
  CHECK_EQ(l[1].text, std::string("Hello"));
  CHECK_NEAR(l[2].start, 30.25, 1e-9);      // two time tags: two lines, sorted
  CHECK_NEAR(l[3].start, 62.0, 1e-9);
  CHECK_EQ(l[3].text, std::string("Twice"));
  CHECK_EQ(lineAt(l, 0.0), -1);
  CHECK_EQ(lineAt(l, 5.0), 0);
  CHECK_EQ(lineAt(l, 20.0), 1);
  CHECK_EQ(lineAt(l, 1000.0), 3);
  CHECK(parseLrc("").empty());
  CHECK(parseLrc("just text").empty());
}

static void testVtt() {
  CHECK_NEAR(vttTime("00:01:02.500"), 62.5, 1e-9);
  CHECK_NEAR(vttTime("01:02.500"), 62.5, 1e-9);
  CHECK_NEAR(vttTime("nonsense"), -1, 1e-9);
  CHECK_EQ(stripVttTags("<c.colorE5E5E5>Hi</c> &amp; <00:00:01.000>bye&nbsp;now"), std::string("Hi & bye now"));
  const std::string vtt =
      "WEBVTT\nKind: captions\n\n"
      "00:00:01.000 --> 00:00:03.000 align:start position:0%\nHello there\n\n"
      "00:00:03.000 --> 00:00:03.010\n \n\n"                 // too short: dropped
      "00:00:03.010 --> 00:00:05.000\nHello there\nthis is rolled up\n\n"  // newest line wins
      "00:00:05.000 --> 00:00:07.000\nthis is rolled up\n\n"   // a repeat: merged
      "00:00:07.000 --> 00:00:09.000\n<c>Last</c> line\n";
  const auto v = parseVtt(vtt);
  CHECK_EQ(v.size(), static_cast<std::size_t>(3));
  CHECK_EQ(v[0].text, std::string("Hello there"));
  CHECK_NEAR(v[0].start, 1.0, 1e-9);
  CHECK_NEAR(v[0].end, 3.0, 1e-9);
  CHECK_EQ(v[1].text, std::string("this is rolled up"));
  CHECK_NEAR(v[1].start, 3.01, 1e-9);
  CHECK_NEAR(v[1].end, 7.0, 1e-9);          // merged with its repeat
  CHECK_EQ(v[2].text, std::string("Last line"));
}

static void testTitles() {
  CHECK_EQ(cleanTitle("Artist - Song (Official Video) [4K]"), std::string("Artist - Song"));
  CHECK_EQ(cleanTitle("Song | Channel"), std::string("Song"));
  CHECK_EQ(cleanTitle("Plain"), std::string("Plain"));
  CHECK_EQ(cleanTitle("Nested (a (b)) end"), std::string("Nested  end"));
  CHECK_EQ(cleanArtist("Some Band - Topic"), std::string("Some Band"));
  CHECK_EQ(cleanArtist("Topic"), std::string("Topic"));

  PlayerInfo p;
  p.title = "Queen - Bohemian Rhapsody (Official Video)";
  p.artist = "Queen Official";
  const auto c = lrcCandidates(p);
  CHECK(c.size() >= 3);
  CHECK_EQ(c[0].artist, std::string("Queen"));
  CHECK_EQ(c[0].track, std::string("Bohemian Rhapsody"));
  CHECK_EQ(c[1].artist, std::string("Queen Official"));
  CHECK_EQ(c.back().artist, std::string(""));
  PlayerInfo none;
  CHECK(lrcCandidates(none).empty());
  PlayerInfo plain;
  plain.title = "Song";
  plain.artist = "Band - Topic";
  const auto pc = lrcCandidates(plain);
  CHECK_EQ(pc[0].artist, std::string("Band"));
  CHECK_EQ(pc[0].track, std::string("Song"));
}

static void testYoutube() {
  CHECK_EQ(youtubeId("https://www.youtube.com/watch?v=dQw4w9WgXcQ&t=10"), std::string("dQw4w9WgXcQ"));
  CHECK_EQ(youtubeId("https://youtu.be/abc_DEF-123?si=x"), std::string("abc_DEF-123"));
  CHECK_EQ(youtubeId("https://example.com/"), std::string(""));
  CHECK_EQ(youtubeId(""), std::string(""));
  PlayerInfo p;
  p.bus = "org.mpris.MediaPlayer2.firefox.instance1";
  p.url = "https://www.youtube.com/watch?v=abc";
  CHECK(p.isYoutube());
  p.url = "https://music.youtube.com/watch?v=abc";
  CHECK(p.isYoutube());
  p.url = "https://example.com/x";
  CHECK(!p.isYoutube());
}

static void testPlayerClock() {
  PlayerInfo p;
  p.status = "Playing";
  p.posSec = 10;
  p.sampled = Clock::now() - std::chrono::seconds(2);
  CHECK_NEAR(p.position(Clock::now()), 12.0, 0.1);
  p.status = "Paused";
  CHECK_NEAR(p.position(Clock::now()), 10.0, 0.01);
  CHECK(!p.playing());
}

static void testConfigFile() {
  char tmpl[] = "/tmp/muzxlcd_test_XXXXXX";
  const char *d = mkdtemp(tmpl);
  CHECK(d != nullptr);
  setenv("XDG_CONFIG_HOME", d, 1);
  setenv("HOME", d, 1);
  unsetenv("MUZX_LCD_PORT");
  unsetenv("MUZX_LCD_COLS");
  // missing file: defaults
  Config c0 = loadConfig();
  CHECK_EQ(c0.cols, 16);
  CHECK(c0.volumePopup && c0.notifications);
  // saveConfigKey adds, then replaces, and keeps other lines
  saveConfigKey("modes", "lyrics,clock");
  saveConfigKey("volume_popup", "0");
  saveConfigKey("modes", "clock,spectrum");
  const std::string text = readFile(fs::path(d) / "muzx" / "lcd.conf");
  CHECK(text.find("modes=clock,spectrum") != std::string::npos);
  CHECK(text.find("modes=lyrics,clock") == std::string::npos);
  CHECK(text.find("volume_popup=0") != std::string::npos);
  Config c1 = loadConfig();
  CHECK_EQ(c1.modes.size(), static_cast<std::size_t>(2));
  CHECK_EQ(c1.modes[1], std::string("spectrum"));
  CHECK(!c1.volumePopup);
  CHECK(c1.notifications);
  // comments and unknown keys are kept and ignored
  {
    std::ofstream out(fs::path(d) / "muzx" / "lcd.conf", std::ios::app);
    out << "# my note\nunknown=1\nnotify_seconds=99\ncols=20\n";
  }
  saveConfigKey("notifications", "0");
  const std::string t2 = readFile(fs::path(d) / "muzx" / "lcd.conf");
  CHECK(t2.find("# my note") != std::string::npos);
  Config c2 = loadConfig();
  CHECK(!c2.notifications);
  CHECK_NEAR(c2.notifySeconds, 30.0, 1e-9); // limited to 30
  CHECK_EQ(c2.cols, 20);
  fs::remove_all(d);
}

static void testLcdText() {
  // plain letters pass through; accents and other scripts become something an LCD can show
  CHECK_EQ(lcdtext::toLcd("Hello", false), std::string("Hello"));
  const std::string cafe = lcdtext::toLcd("caf\xC3\xA9", false); // café
  CHECK(!cafe.empty());
  for (unsigned char c : cafe)
    CHECK(c >= 32 && c < 127);
  const std::string jp = lcdtext::toLcd("\xE3\x81\x82\xE3\x82\x8A", false); // あり
  CHECK(!jp.empty());
  for (unsigned char c : jp)
    CHECK(c >= 32 && c < 127);
}

int main() {
  testText();
  testJson();
  testLrc();
  testVtt();
  testTitles();
  testYoutube();
  testPlayerClock();
  testConfigFile();
  testLcdText();
  TEST_MAIN_END();
}
