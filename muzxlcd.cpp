// muzxlcd - one small background program that owns the Arduino LCD and shows
// different things on it ("modes"):
//
//   lyrics      synced lyrics of whatever is playing (Spotify, YouTube Music,
//               a browser tab, MUZX ...), found through MPRIS + lrclib
//   captions    YouTube captions of the video playing in the browser
//   nowplaying  title / artist of the current media
//   clock       time and date
//   status      CPU, RAM, temperature, network, battery
//
// Switch modes with the button on the Uno (short press = next, long press =
// previous) or from the PC:  muzxlcd ctl mode next | prev | clock | status ...
//
//   muzxlcd              start the background program
//   muzxlcd ctl <cmd>    talk to it  (mode, modes, status, quit)
//
// Settings (optional): ~/.config/muzx/lcd.conf, lines like  key=value
//   modes=lyrics,captions,nowplaying,clock,status     which modes, in which
//   order cols=16                                           20 for a 20x4
//   screen port=/dev/ttyUSB0                                 (or env
//   MUZX_LCD_PORT) captions_lang=en                                  caption
//   language for yt-dlp page_seconds=3                                    how
//   long each status page stays kana=0 1 = Japanese as katakana
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "lcd_link.hpp" // text conversion for the LCD (Japanese, Hindi ... -> letters)

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ===========================================================================
// small helpers
// ===========================================================================
static std::string trim(const std::string &s) {
  std::size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos)
    return "";
  std::size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

static fs::path homeDir() {
  const char *h = std::getenv("HOME");
  return h ? fs::path(h) : fs::path("/tmp");
}

static fs::path configDir() {
  const char *x = std::getenv("XDG_CONFIG_HOME");
  return (x && *x ? fs::path(x) : homeDir() / ".config") / "muzx";
}

static fs::path cacheDir() {
  const char *x = std::getenv("XDG_CACHE_HOME");
  return (x && *x ? fs::path(x) : homeDir() / ".cache") / "muzxlcd";
}

static std::string socketPath() {
  if (const char *o = std::getenv("MUZXLCD_SOCKET"); o && *o)
    return o;
  const char *rt = std::getenv("XDG_RUNTIME_DIR");
  if (rt && *rt)
    return std::string(rt) + "/muzxlcd.sock";
  return "/tmp/muzxlcd-" + std::to_string(getuid()) + ".sock";
}

static std::string readFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static bool hasHighBytes(const std::string &s) {
  for (unsigned char c : s)
    if (c >= 128)
      return true;
  return false;
}

static std::uint64_t fnv(const std::string &s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  return h;
}

static std::string hex(std::uint64_t v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(v));
  return b;
}

// Runs a program (no shell), returns its stdout. ok=false when it could not
// run or exited with an error.
static std::string runProgram(const std::vector<std::string> &args, bool &ok,
                              int *exitCode = nullptr) {
  ok = false;
  int fds[2];
  if (pipe(fds) != 0)
    return "";
  const pid_t pid = fork();
  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    return "";
  }
  if (pid == 0) {
    dup2(fds[1], 1);
    const int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, 0);
      dup2(devnull, 2);
    }
    close(fds[0]);
    close(fds[1]);
    std::vector<char *> argv;
    for (const std::string &a : args)
      argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(fds[1]);
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0)
    out.append(buf, static_cast<std::size_t>(n));
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (exitCode)
    *exitCode = code;
  ok = code == 0;
  return out;
}

// ===========================================================================
// settings
// ===========================================================================
struct Config {
  int cols = 16;
  std::vector<std::string> modes{"lyrics", "captions", "nowplaying", "clock",
                                 "status"};
  std::string port;
  std::string captionsLang = "en";
  double pageSeconds = 3.0;
  bool kana = false;
};

static const std::vector<std::string> kAllModes{
    "lyrics", "captions", "nowplaying", "clock", "status"};

static std::string modeTitle(const std::string &m) {
  if (m == "lyrics")
    return "Lyrics";
  if (m == "captions")
    return "Captions";
  if (m == "nowplaying")
    return "Now playing";
  if (m == "clock")
    return "Clock";
  if (m == "status")
    return "PC status";
  return m;
}

static Config loadConfig() {
  Config c;
  std::ifstream in(configDir() / "lcd.conf");
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#')
      continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string k = trim(line.substr(0, eq)),
                      v = trim(line.substr(eq + 1));
    if (k == "cols") {
      c.cols = std::clamp(std::atoi(v.c_str()), 8, 40);
    } else if (k == "modes") {
      std::vector<std::string> modes;
      for (const std::string &m : split(v, ',')) {
        const std::string name = lower(trim(m));
        if (std::find(kAllModes.begin(), kAllModes.end(), name) !=
                kAllModes.end() &&
            std::find(modes.begin(), modes.end(), name) == modes.end())
          modes.push_back(name);
      }
      if (!modes.empty())
        c.modes = modes;
    } else if (k == "port") {
      c.port = v;
    } else if (k == "captions_lang" && !v.empty()) {
      c.captionsLang = v;
    } else if (k == "page_seconds") {
      c.pageSeconds = std::clamp(std::atof(v.c_str()), 1.0, 30.0);
    } else if (k == "kana") {
      c.kana = v == "1" || v == "true";
    }
  }
  if (const char *p = std::getenv("MUZX_LCD_PORT"); p && *p)
    c.port = p;
  if (std::getenv("MUZX_LCD_KANA"))
    c.kana = true;
  if (const char *cols = std::getenv("MUZX_LCD_COLS"); cols && *cols)
    c.cols = std::clamp(std::atoi(cols), 8, 40);
  return c;
}

// ===========================================================================
// a tiny JSON reader (enough for lrclib)
// ===========================================================================
struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  bool b = false;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json *get(const std::string &key) const {
    for (const auto &kv : obj)
      if (kv.first == key)
        return &kv.second;
    return nullptr;
  }
  std::string getStr(const std::string &key) const {
    const Json *j = get(key);
    return j && j->type == Str ? j->str : "";
  }
  double getNum(const std::string &key, double def = 0) const {
    const Json *j = get(key);
    return j && j->type == Num ? j->num : def;
  }
};

static void appendUtf8(std::string &out, unsigned cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

namespace {
struct JsonParser {
  const std::string &s;
  std::size_t i = 0;
  int depth = 0;
  bool bad = false;

  void ws() {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
  }
  unsigned hex4() {
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
      if (i >= s.size()) {
        bad = true;
        return 0;
      }
      const char c = s[i++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<unsigned>(c - 'A' + 10);
      else
        bad = true;
    }
    return v;
  }
  std::string string() {
    std::string out;
    ++i; // opening quote
    while (i < s.size() && s[i] != '"') {
      char c = s[i++];
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size())
        break;
      c = s[i++];
      switch (c) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'u': {
        unsigned cp = hex4();
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' &&
            s[i + 1] == 'u') {
          i += 2;
          const unsigned lo = hex4();
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        appendUtf8(out, cp);
        break;
      }
      default:
        out += c;
        break;
      }
    }
    if (i < s.size())
      ++i; // closing quote
    else
      bad = true;
    return out;
  }
  Json value() {
    Json j;
    if (++depth > 40) {
      bad = true;
      --depth;
      return j;
    }
    ws();
    if (i >= s.size()) {
      bad = true;
    } else if (s[i] == '{') {
      j.type = Json::Obj;
      ++i;
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
      } else {
        while (!bad && i < s.size()) {
          ws();
          if (i >= s.size() || s[i] != '"') {
            bad = true;
            break;
          }
          std::string key = string();
          ws();
          if (i >= s.size() || s[i] != ':') {
            bad = true;
            break;
          }
          ++i;
          j.obj.emplace_back(std::move(key), value());
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == '}') {
            ++i;
            break;
          }
          bad = true;
        }
      }
    } else if (s[i] == '[') {
      j.type = Json::Arr;
      ++i;
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
      } else {
        while (!bad && i < s.size()) {
          j.arr.push_back(value());
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == ']') {
            ++i;
            break;
          }
          bad = true;
        }
      }
    } else if (s[i] == '"') {
      j.type = Json::Str;
      j.str = string();
    } else if (s.compare(i, 4, "true") == 0) {
      j.type = Json::Bool;
      j.b = true;
      i += 4;
    } else if (s.compare(i, 5, "false") == 0) {
      j.type = Json::Bool;
      i += 5;
    } else if (s.compare(i, 4, "null") == 0) {
      i += 4;
    } else {
      char *end = nullptr;
      j.num = std::strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i) {
        bad = true;
      } else {
        j.type = Json::Num;
        i = static_cast<std::size_t>(end - s.c_str());
      }
    }
    --depth;
    return j;
  }
};
} // namespace

static std::optional<Json> parseJson(const std::string &text) {
  JsonParser p{text};
  Json j = p.value();
  if (p.bad)
    return std::nullopt;
  return j;
}

// ===========================================================================
// timed text: LRC lyrics and WebVTT captions
// ===========================================================================
struct TimedLine {
  double start = 0;
  double end = 1e9; // captions only
  std::string text;
};

static std::vector<TimedLine> parseLrc(const std::string &lrc) {
  std::vector<TimedLine> out;
  for (const std::string &raw : split(lrc, '\n')) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    std::vector<double> times;
    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == '[') {
      const std::size_t close = line.find(']', pos);
      if (close == std::string::npos)
        break;
      const std::string tag = line.substr(pos + 1, close - pos - 1);
      int mm = 0;
      double ss = 0;
      if (std::sscanf(tag.c_str(), "%d:%lf", &mm, &ss) == 2 && mm >= 0 &&
          std::isdigit(static_cast<unsigned char>(tag[0])))
        times.push_back(mm * 60.0 + ss);
      else
        break; // [ar:...], [ti:...] and the like
      pos = close + 1;
    }
    if (times.empty())
      continue;
    const std::string text = trim(line.substr(pos));
    for (double t : times)
      out.push_back({t, 1e9, text});
  }
  std::stable_sort(
      out.begin(), out.end(),
      [](const TimedLine &a, const TimedLine &b) { return a.start < b.start; });
  return out;
}

static double vttTime(const std::string &s) {
  int h = 0, m = 0;
  double sec = 0;
  if (std::sscanf(s.c_str(), "%d:%d:%lf", &h, &m, &sec) == 3)
    return h * 3600.0 + m * 60.0 + sec;
  if (std::sscanf(s.c_str(), "%d:%lf", &m, &sec) == 2)
    return m * 60.0 + sec;
  return -1;
}

static std::string stripVttTags(const std::string &in) {
  std::string out;
  bool tag = false;
  for (char c : in) {
    if (c == '<')
      tag = true;
    else if (c == '>')
      tag = false;
    else if (!tag)
      out += c;
  }
  // a few common entities
  const std::pair<const char *, const char *> ents[] = {
      {"&amp;", "&"},   {"&lt;", "<"},  {"&gt;", ">"},
      {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}};
  for (const auto &e : ents) {
    std::size_t p = 0;
    while ((p = out.find(e.first, p)) != std::string::npos) {
      out.replace(p, std::strlen(e.first), e.second);
      p += std::strlen(e.second);
    }
  }
  return trim(out);
}

// YouTube "automatic captions" repeat every line in the next cue (they roll
// up). We take the newest (last) line of each cue and drop repeats.
static std::vector<TimedLine> parseVtt(const std::string &vtt) {
  std::vector<TimedLine> out;
  const std::vector<std::string> lines = split(vtt, '\n');
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::size_t arrow = lines[i].find("-->");
    if (arrow == std::string::npos)
      continue;
    const double start = vttTime(trim(lines[i].substr(0, arrow)));
    std::string rest = trim(lines[i].substr(arrow + 3));
    const double end = vttTime(rest.substr(0, rest.find(' ')));
    if (start < 0 || end < 0)
      continue;
    std::string last;
    std::size_t j = i + 1;
    for (; j < lines.size() && !trim(lines[j]).empty(); ++j) {
      const std::string t = stripVttTags(lines[j]);
      if (!t.empty())
        last = t;
    }
    i = j;
    if (last.empty() || end - start < 0.02)
      continue;
    if (!out.empty() && out.back().text == last) {
      out.back().end = std::max(out.back().end, end);
      continue;
    }
    out.push_back({start, end, last});
  }
  std::stable_sort(
      out.begin(), out.end(),
      [](const TimedLine &a, const TimedLine &b) { return a.start < b.start; });
  // a line should vanish when the next one starts
  for (std::size_t k = 0; k + 1 < out.size(); ++k)
    out[k].end = std::min(out[k].end, out[k + 1].start);
  return out;
}

// index of the last line with start <= pos, or -1
static int lineAt(const std::vector<TimedLine> &lines, double pos) {
  const auto it = std::upper_bound(
      lines.begin(), lines.end(), pos,
      [](double p, const TimedLine &l) { return p < l.start; });
  return static_cast<int>(it - lines.begin()) - 1;
}

// ===========================================================================
// MPRIS: what is playing right now (Spotify, browsers, MUZX ...)
// ===========================================================================
struct PlayerInfo {
  std::string bus;    // org.mpris.MediaPlayer2.spotify
  std::string status; // Playing / Paused / Stopped
  std::string title, artist, url;
  double lengthSec = 0;
  double posSec = 0;
  Clock::time_point sampled{};

  bool playing() const { return status == "Playing"; }
  bool isMuzx() const { return bus == "org.mpris.MediaPlayer2.muzx"; }
  bool isYoutube() const {
    return url.find("youtube.com/watch") != std::string::npos ||
           url.find("youtu.be/") != std::string::npos ||
           url.find("music.youtube.com") != std::string::npos;
  }
  double position(Clock::time_point now) const {
    double p = posSec;
    if (playing())
      p += std::chrono::duration<double>(now - sampled).count();
    return p;
  }
  std::string key() const {
    return bus + "|" + title + "|" + artist + "|" +
           std::to_string(static_cast<long>(lengthSec));
  }
};

class MprisWatcher {
public:
  MprisWatcher() {
    worker_ = std::thread([this] { loop(); });
  }
  ~MprisWatcher() {
    stop_ = true;
    if (worker_.joinable())
      worker_.join();
  }
  std::vector<PlayerInfo> players() const {
    std::lock_guard<std::mutex> lock(mu_);
    return players_;
  }

private:
  static constexpr const char *kPath = "/org/mpris/MediaPlayer2";
  static constexpr const char *kIface = "org.mpris.MediaPlayer2.Player";

  void loop() {
    sd_bus *bus = nullptr;
    while (!stop_) {
      if (!bus) {
        if (sd_bus_open_user(&bus) < 0) {
          bus = nullptr;
          sleepMs(2000);
          continue;
        }
        sd_bus_set_method_call_timeout(bus, 700 * 1000);
      }
      std::vector<PlayerInfo> found;
      char **names = nullptr;
      const int n = sd_bus_list_names(bus, &names, nullptr);
      if (n < 0) {
        sd_bus_flush_close_unref(bus);
        bus = nullptr;
        sleepMs(2000);
        continue;
      }
      for (char **p = names; p && *p; ++p) {
        if (std::strncmp(*p, "org.mpris.MediaPlayer2.", 23) == 0)
          found.push_back(query(bus, *p));
        free(*p);
      }
      free(names);
      {
        std::lock_guard<std::mutex> lock(mu_);
        players_ = std::move(found);
      }
      sleepMs(1000);
    }
    if (bus)
      sd_bus_flush_close_unref(bus);
  }

  void sleepMs(int ms) {
    for (int i = 0; i < ms / 50 && !stop_; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  static PlayerInfo query(sd_bus *bus, const char *name) {
    PlayerInfo p;
    p.bus = name;
    p.sampled = Clock::now();
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *status = nullptr;
    if (sd_bus_get_property_string(bus, name, kPath, kIface, "PlaybackStatus",
                                   &err, &status) >= 0 &&
        status)
      p.status = status;
    free(status);
    sd_bus_error_free(&err);
    err = SD_BUS_ERROR_NULL;

    std::int64_t pos = 0;
    if (sd_bus_get_property_trivial(bus, name, kPath, kIface, "Position", &err,
                                    'x', &pos) >= 0) {
      p.posSec = static_cast<double>(pos) / 1e6;
      p.sampled = Clock::now();
    }
    sd_bus_error_free(&err);
    err = SD_BUS_ERROR_NULL;

    sd_bus_message *m = nullptr;
    if (sd_bus_get_property(bus, name, kPath, kIface, "Metadata", &err, &m,
                            "a{sv}") >= 0 &&
        m)
      readMetadata(m, p);
    if (m)
      sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return p;
  }

  static void readMetadata(sd_bus_message *m, PlayerInfo &p) {
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
      return;
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
      const char *key = nullptr;
      sd_bus_message_read_basic(m, 's', &key);
      char type = 0;
      const char *contents = nullptr;
      if (sd_bus_message_peek_type(m, &type, &contents) < 0 || type != 'v' ||
          !contents) {
        break;
      }
      const std::string k = key ? key : "";
      const std::string sig = contents;
      sd_bus_message_enter_container(m, 'v', contents);
      if (sig == "s") {
        const char *v = nullptr;
        sd_bus_message_read_basic(m, 's', &v);
        if (v) {
          if (k == "xesam:title")
            p.title = v;
          else if (k == "xesam:url")
            p.url = v;
          else if (k == "xesam:artist")
            p.artist = v;
        }
      } else if (sig == "as") {
        if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
          const char *v = nullptr;
          std::string joined;
          while (sd_bus_message_read_basic(m, 's', &v) > 0) {
            if (!joined.empty())
              joined += ", ";
            joined += v ? v : "";
          }
          sd_bus_message_exit_container(m);
          if (k == "xesam:artist")
            p.artist = joined;
        }
      } else if (sig == "x" || sig == "t") {
        if (sig == "x") {
          std::int64_t v = 0;
          sd_bus_message_read_basic(m, 'x', &v);
          if (k == "mpris:length")
            p.lengthSec = static_cast<double>(v) / 1e6;
        } else {
          std::uint64_t v = 0;
          sd_bus_message_read_basic(m, 't', &v);
          if (k == "mpris:length")
            p.lengthSec = static_cast<double>(v) / 1e6;
        }
      } else {
        sd_bus_message_skip(m, contents);
      }
      sd_bus_message_exit_container(m); // v
      sd_bus_message_exit_container(m); // e
    }
    sd_bus_message_exit_container(m); // a (may fail if we broke out: harmless)
  }

  mutable std::mutex mu_;
  std::vector<PlayerInfo> players_;
  std::atomic<bool> stop_{false};
  std::thread worker_;
};

// ===========================================================================
// lyrics from lrclib (for players other than MUZX)
// ===========================================================================
static std::string lrclibBase() {
  const char *env = std::getenv("MUZX_LRCLIB_URL");
  if (env && *env) {
    std::string s = env;
    while (!s.empty() && s.back() == '/')
      s.pop_back();
    return s;
  }
  return "https://lrclib.net";
}

// Titles from the web look like "Artist - Song (Official Video) [4K]".
static std::string cleanTitle(std::string t) {
  std::string out;
  int depth = 0;
  for (char c : t) {
    if (c == '(' || c == '[') {
      ++depth;
    } else if ((c == ')' || c == ']') && depth > 0) {
      --depth;
    } else if (depth == 0) {
      out += c;
    }
  }
  for (const char *junk : {" | ", " // "}) {
    const std::size_t p = out.find(junk);
    if (p != std::string::npos)
      out = out.substr(0, p);
  }
  return trim(out);
}

static std::string cleanArtist(std::string a) {
  const std::string suffix = " - Topic";
  if (a.size() > suffix.size() &&
      a.compare(a.size() - suffix.size(), suffix.size(), suffix) == 0)
    a.erase(a.size() - suffix.size());
  return trim(a);
}

struct LrcQuery {
  std::string artist, track;
};

static std::vector<LrcQuery> lrcCandidates(const PlayerInfo &p) {
  std::vector<LrcQuery> out;
  const std::string title = cleanTitle(p.title);
  const std::string artist = cleanArtist(p.artist);
  if (title.empty())
    return out;
  const std::size_t dash = title.find(" - ");
  if (dash != std::string::npos) {
    const std::string a = trim(title.substr(0, dash)),
                      b = trim(title.substr(dash + 3));
    out.push_back({a, b}); // "Artist - Song" is the usual order on YouTube
    if (!artist.empty())
      out.push_back({artist, b});
  }
  if (!artist.empty())
    out.push_back({artist, title});
  out.push_back({"", title});
  return out;
}

static std::string
lrclibGet(const std::vector<std::string> &args, const std::string &path,
          const std::vector<std::pair<std::string, std::string>> &query) {
  std::vector<std::string> cmd{
      "curl", "-sS", "-f",          "--max-time",       "8",
      "-G",   "-A",  "muzxlcd/1.0", lrclibBase() + path};
  for (const auto &kv : query) {
    cmd.push_back("--data-urlencode");
    cmd.push_back(kv.first + "=" + kv.second);
  }
  (void)args;
  bool ok = false;
  const std::string out = runProgram(cmd, ok);
  return ok ? out : "";
}

// Looks for synced lyrics. Returns "" when none (or the network failed).
// `transient` is set when a failure might go away later (no network).
static std::string findSyncedLyrics(const PlayerInfo &p, bool &transient) {
  transient = false;
  bool anyReply = false;
  for (const LrcQuery &q : lrcCandidates(p)) {
    if (!q.artist.empty()) {
      std::vector<std::pair<std::string, std::string>> params{
          {"artist_name", q.artist}, {"track_name", q.track}};
      if (p.lengthSec > 1)
        params.push_back({"duration", std::to_string(static_cast<int>(
                                          std::lround(p.lengthSec)))});
      const std::string body = lrclibGet({}, "/api/get", params);
      if (!body.empty()) {
        anyReply = true;
        if (auto j = parseJson(body); j && j->type == Json::Obj) {
          const std::string synced = j->getStr("syncedLyrics");
          if (!trim(synced).empty())
            return synced;
        }
      }
    }
    const std::string body = lrclibGet(
        {}, "/api/search",
        {{"q", q.artist.empty() ? q.track : q.artist + " " + q.track}});
    if (body.empty())
      continue;
    anyReply = true;
    auto j = parseJson(body);
    if (!j || j->type != Json::Arr)
      continue;
    const std::string wantTrack = lower(q.track);
    const std::string wantArtist = lower(q.artist);
    const std::string *best = nullptr;
    double bestScore = -1e9;
    for (const Json &item : j->arr) {
      const std::string synced = item.getStr("syncedLyrics");
      if (trim(synced).empty())
        continue;
      double score = 0;
      const std::string name = lower(item.getStr("trackName"));
      const std::string art = lower(item.getStr("artistName"));
      if (!wantTrack.empty() && (name.find(wantTrack) != std::string::npos ||
                                 wantTrack.find(name) != std::string::npos))
        score += 10;
      if (!wantArtist.empty() && art.find(wantArtist) != std::string::npos)
        score += 5;
      if (p.lengthSec > 1) {
        const double d = item.getNum("duration", -1);
        if (d > 0)
          score -= std::min(10.0, std::fabs(d - p.lengthSec));
      }
      if (score > bestScore) {
        bestScore = score;
        best = &item.get("syncedLyrics")->str;
      }
    }
    // need at least a name match, otherwise we would show another song's words
    if (best && bestScore >= 5)
      return *best;
  }
  transient = !anyReply;
  return "";
}

// One lookup running in the background; the main loop polls `state`.
struct Lookup {
  enum State { Searching, Ready, None, NoTool, Failed };
  std::atomic<int> state{Searching};
  std::atomic<long long> doneMs{0}; // when the lookup finished (steady clock)
  std::mutex mu;
  std::vector<TimedLine> lines;
  std::string key;

  static long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch())
        .count();
  }
  void finish(State s) {
    doneMs = nowMs();
    state = s;
  }
  double ageSeconds() const {
    return static_cast<double>(nowMs() - doneMs) / 1000.0;
  }
};

static std::shared_ptr<Lookup> startLyricsLookup(const PlayerInfo &p) {
  auto lookup = std::make_shared<Lookup>();
  lookup->key = p.key();
  std::thread([lookup, p] {
    const fs::path dir = cacheDir() / "lyrics";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string id =
        hex(fnv(cleanArtist(p.artist) + "|" + cleanTitle(p.title) + "|" +
                std::to_string(static_cast<int>(p.lengthSec))));
    const fs::path lrcFile = dir / (id + ".lrc"),
                   noneFile = dir / (id + ".none");
    std::string lrc;
    if (fs::exists(lrcFile, ec)) {
      lrc = readFile(lrcFile);
    } else if (fs::exists(noneFile, ec) &&
               fs::file_time_type::clock::now() -
                       fs::last_write_time(noneFile, ec) <
                   std::chrono::hours(24)) {
      lookup->finish(Lookup::None);
      return;
    } else {
      bool transient = false;
      lrc = findSyncedLyrics(p, transient);
      if (!lrc.empty()) {
        std::ofstream(lrcFile) << lrc;
      } else if (transient) {
        lookup->finish(Lookup::Failed);
        return;
      } else {
        std::ofstream(noneFile) << "none\n";
      }
    }
    std::vector<TimedLine> lines = parseLrc(lrc);
    if (lines.empty()) {
      lookup->finish(Lookup::None);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(lookup->mu);
      lookup->lines = std::move(lines);
    }
    lookup->finish(Lookup::Ready);
  }).detach();
  return lookup;
}

// YouTube captions through yt-dlp.
static std::string youtubeId(const std::string &url) {
  auto grab = [&](const std::string &marker) -> std::string {
    const std::size_t p = url.find(marker);
    if (p == std::string::npos)
      return "";
    std::string id;
    for (std::size_t i = p + marker.size(); i < url.size(); ++i) {
      const char c = url[i];
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
        id += c;
      else
        break;
    }
    return id;
  };
  std::string id = grab("v=");
  if (id.empty())
    id = grab("youtu.be/");
  return id;
}

static std::shared_ptr<Lookup> startCaptionLookup(const PlayerInfo &p,
                                                  const std::string &lang) {
  auto lookup = std::make_shared<Lookup>();
  lookup->key = youtubeId(p.url) + "|" + lang;
  const std::string id = youtubeId(p.url);
  std::thread([lookup, id, lang] {
    const fs::path dir = cacheDir() / "captions";
    std::error_code ec;
    fs::create_directories(dir, ec);
    auto findVtt = [&]() -> fs::path {
      fs::path any;
      for (const auto &e : fs::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind(id + ".", 0) != 0 || e.path().extension() != ".vtt")
          continue;
        if (name == id + "." + lang + ".vtt")
          return e.path();
        if (any.empty())
          any = e.path();
      }
      return any;
    };
    fs::path vtt = findVtt();
    if (vtt.empty()) {
      bool ok = false;
      int code = 0;
      runProgram({"timeout", "90", "yt-dlp", "--skip-download", "--no-warnings",
                  "--no-playlist", "--write-subs", "--write-auto-subs",
                  "--sub-langs", lang + ".*," + lang, "--sub-format", "vtt",
                  "-o", (dir / "%(id)s.%(ext)s").string(),
                  "https://www.youtube.com/watch?v=" + id},
                 ok, &code);
      if (code == 127) {
        lookup->finish(Lookup::NoTool);
        return;
      }
      vtt = findVtt();
    }
    if (vtt.empty()) {
      lookup->finish(Lookup::None);
      return;
    }
    std::vector<TimedLine> lines = parseVtt(readFile(vtt));
    if (lines.empty()) {
      lookup->finish(Lookup::None);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(lookup->mu);
      lookup->lines = std::move(lines);
    }
    lookup->finish(Lookup::Ready);
  }).detach();
  return lookup;
}

// ===========================================================================
// PC status
// ===========================================================================
class SystemStats {
public:
  void update() {
    const auto now = Clock::now();
    if (now - last_ < std::chrono::milliseconds(900))
      return;
    const double dt = std::chrono::duration<double>(now - last_).count();
    last_ = now;

    { // CPU
      std::ifstream in("/proc/stat");
      std::string tag;
      unsigned long long v[8] = {0};
      in >> tag;
      for (auto &x : v)
        in >> x;
      const unsigned long long idle = v[3] + v[4];
      unsigned long long total = 0;
      for (auto x : v)
        total += x;
      if (prevTotal_ > 0 && total > prevTotal_)
        cpu_ = 100.0 * (1.0 - static_cast<double>(idle - prevIdle_) /
                                  static_cast<double>(total - prevTotal_));
      prevIdle_ = idle;
      prevTotal_ = total;
    }
    { // memory
      std::ifstream in("/proc/meminfo");
      std::string key;
      unsigned long long val = 0;
      std::string unit;
      unsigned long long total = 0, avail = 0;
      while (in >> key >> val >> unit) {
        if (key == "MemTotal:")
          total = val;
        else if (key == "MemAvailable:")
          avail = val;
      }
      if (total > 0)
        ram_ = 100.0 * static_cast<double>(total - avail) /
               static_cast<double>(total);
    }
    { // temperature: hottest sensor
      double best = -1;
      std::error_code ec;
      for (const char *root : {"/sys/class/thermal", "/sys/class/hwmon"})
        for (const auto &e : fs::directory_iterator(root, ec)) {
          for (const auto &f : fs::directory_iterator(e.path(), ec)) {
            const std::string n = f.path().filename().string();
            const bool isTemp =
                (n == "temp") || (n.rfind("temp", 0) == 0 &&
                                  n.find("_input") != std::string::npos);
            if (!isTemp)
              continue;
            std::ifstream in(f.path());
            double v = 0;
            if (in >> v && v > 1000 && v < 150000)
              best = std::max(best, v / 1000.0);
          }
        }
      temp_ = best;
    }
    { // load, uptime
      std::ifstream in("/proc/loadavg");
      in >> load_;
      std::ifstream up("/proc/uptime");
      up >> uptime_;
    }
    { // network
      std::ifstream in("/proc/net/dev");
      std::string line;
      std::getline(in, line);
      std::getline(in, line);
      unsigned long long rx = 0, tx = 0;
      while (std::getline(in, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos)
          continue;
        if (trim(line.substr(0, colon)) == "lo")
          continue;
        std::istringstream ss(line.substr(colon + 1));
        unsigned long long r = 0, t = 0, skip = 0;
        ss >> r;
        for (int i = 0; i < 7; ++i)
          ss >> skip;
        ss >> t;
        rx += r;
        tx += t;
      }
      if (prevRx_ > 0 && dt > 0) {
        down_ = rx >= prevRx_ ? static_cast<double>(rx - prevRx_) / dt : 0;
        up_ = tx >= prevTx_ ? static_cast<double>(tx - prevTx_) / dt : 0;
      }
      prevRx_ = rx;
      prevTx_ = tx;
    }
    { // battery
      battery_ = -1;
      std::error_code ec;
      for (const auto &e :
           fs::directory_iterator("/sys/class/power_supply", ec)) {
        if (e.path().filename().string().rfind("BAT", 0) != 0)
          continue;
        std::ifstream cap(e.path() / "capacity"), st(e.path() / "status");
        cap >> battery_;
        std::getline(st, batteryState_);
        break;
      }
    }
  }

  double cpu() const { return cpu_; }
  double ram() const { return ram_; }
  double temp() const { return temp_; }
  double load() const { return load_; }
  double uptime() const { return uptime_; }
  double down() const { return down_; }
  double up() const { return up_; }
  int battery() const { return battery_; }
  const std::string &batteryState() const { return batteryState_; }

private:
  Clock::time_point last_{};
  unsigned long long prevIdle_ = 0, prevTotal_ = 0, prevRx_ = 0, prevTx_ = 0;
  double cpu_ = 0, ram_ = 0, temp_ = -1, load_ = 0, uptime_ = 0, down_ = 0,
         up_ = 0;
  int battery_ = -1;
  std::string batteryState_;
};

static std::string fmtRate(double bytes) {
  char b[24];
  if (bytes >= 1048576.0)
    std::snprintf(b, sizeof(b), "%.1fM", bytes / 1048576.0);
  else if (bytes >= 1024.0)
    std::snprintf(b, sizeof(b), "%.0fK", bytes / 1024.0);
  else
    std::snprintf(b, sizeof(b), "%.0fB", bytes);
  return b;
}

// ===========================================================================
// the LCD: serial port + frames
// ===========================================================================
struct Frame {
  bool rows = false; // two fixed rows (a, b) instead of free text
  std::string a, b, text;
  bool operator==(const Frame &o) const {
    return rows == o.rows && a == o.a && b == o.b && text == o.text;
  }
};

static std::string fit(const std::string &s, int cols) {
  return s.size() > static_cast<std::size_t>(cols)
             ? s.substr(0, static_cast<std::size_t>(cols))
             : s;
}

static std::string center(const std::string &s, int cols) {
  const std::string t = fit(s, cols);
  const int pad = (cols - static_cast<int>(t.size())) / 2;
  return std::string(static_cast<std::size_t>(std::max(0, pad)), ' ') + t;
}

static Frame rowsFrame(const std::string &a, const std::string &b) {
  Frame f;
  f.rows = true;
  f.a = a;
  f.b = b;
  return f;
}

static Frame textFrame(const std::string &t) {
  Frame f;
  f.text = t;
  return f;
}

class Lcd {
public:
  explicit Lcd(const Config &cfg) : cfg_(cfg) {}
  ~Lcd() { closePort(); }

  bool connected() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  void setKana(bool on) {
    if (cfg_.kana == on)
      return;
    cfg_.kana = on;
    convIn_.clear();
    last_.clear(); // redraw with the new character set
  }
  const std::string &port() const { return port_; }

  // Call every loop; (re)opens the port when needed.
  void maintain() {
    const auto now = Clock::now();
    if (fd_ >= 0 && now >= nextCheck_) {
      nextCheck_ = now + std::chrono::milliseconds(500);
      struct stat stPath{}, stFd{};
      if (::stat(port_.c_str(), &stPath) != 0 || ::fstat(fd_, &stFd) != 0 ||
          stPath.st_ino != stFd.st_ino)
        closePort();
    }
    if (fd_ < 0 && now >= nextTry_) {
      nextTry_ = now + std::chrono::milliseconds(1000);
      openPort();
    }
  }

  // Reads button messages. Returns the letters received ('B' / 'L').
  std::string readButtons() {
    std::string out;
    if (fd_ < 0)
      return out;
    char buf[64];
    for (;;) {
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n > 0) {
        for (ssize_t i = 0; i < n; ++i) {
          if (buf[i] == '\n') {
            const std::string line = trim(rx_);
            if (line == "B" || line == "L")
              out += line;
            rx_.clear();
          } else if (rx_.size() < 64) {
            rx_ += buf[i];
          }
        }
        continue;
      }
      if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
        closePort(); // unplugged
      break;
    }
    return out;
  }

  void show(const Frame &f) {
    if (fd_ < 0)
      return;
    const auto now = Clock::now();
    if (now - openedAt_ < std::chrono::milliseconds(2400))
      return; // the Uno restarts when the port opens
    std::string msg;
    if (f.rows)
      msg = fit(convert(f.a), cfg_.cols) + '\x01' +
            fit(convert(f.b), cfg_.cols) + '\n';
    else
      msg = convert(f.text) + '\n';
    if (msg == last_ && now - lastWrite_ < std::chrono::milliseconds(2500))
      return;
    const ssize_t n = ::write(fd_, msg.data(), msg.size());
    if (n < 0 && errno != EAGAIN && errno != EINTR) {
      closePort();
      return;
    }
    last_ = msg;
    lastWrite_ = now;
  }

private:
  std::string convert(const std::string &s) {
    if (!hasHighBytes(s))
      return s; // plain ASCII needs no conversion
    if (s == convIn_)
      return convOut_;
    convIn_ = s;
    convOut_ = lcdtext::toLcd(s, cfg_.kana);
    while (!convOut_.empty() &&
           (convOut_.back() == '\n' || convOut_.back() == '\r'))
      convOut_.pop_back();
    return convOut_;
  }

  void openPort() {
    std::vector<std::string> candidates;
    if (!cfg_.port.empty())
      candidates.push_back(cfg_.port);
    for (const char *p : {"/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2",
                          "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2"})
      candidates.push_back(p);
    for (const std::string &p : candidates) {
      const int fd = ::open(p.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
      if (fd < 0)
        continue;
      termios tio{};
      if (tcgetattr(fd, &tio) != 0) {
        ::close(fd);
        continue;
      }
      cfmakeraw(&tio);
      cfsetospeed(&tio, B115200);
      cfsetispeed(&tio, B115200);
      tio.c_cflag |= CLOCAL | CREAD | HUPCL;
      tio.c_cflag &= ~static_cast<tcflag_t>(CSTOPB | PARENB | CRTSCTS);
      if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        ::close(fd);
        continue;
      }
      int status = TIOCM_DTR | TIOCM_RTS;
      ::ioctl(fd, TIOCMBIS, &status);
      tcflush(fd, TCIOFLUSH);
      fd_ = fd;
      port_ = p;
      openedAt_ = Clock::now();
      last_.clear();
      rx_.clear();
      return;
    }
  }

  void closePort() {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    last_.clear();
  }

  Config cfg_;
  int fd_ = -1;
  std::string port_, last_, rx_, convIn_, convOut_;
  Clock::time_point nextTry_{}, openedAt_{}, nextCheck_{}, lastWrite_{};
};

// ===========================================================================
// the hub: modes
// ===========================================================================
static std::atomic<bool> g_quit{false};
static void onSignal(int) { g_quit = true; }

class Hub {
public:
  Hub(const Config &cfg) : cfg_(cfg), lcd_(cfg) {
    const std::string saved = trim(readFile(configDir() / "lcd.state"));
    for (std::size_t i = 0; i < cfg_.modes.size(); ++i)
      if (cfg_.modes[i] == saved)
        mode_ = i;
    bannerUntil_ = Clock::now() + std::chrono::milliseconds(1500);
  }

  int run() {
    const std::string path = socketPath();
    const int sock = openControlSocket(path);
    if (sock < 0)
      return 1;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "muzxlcd: running, mode '%s', control socket %s\n",
                 cfg_.modes[mode_].c_str(), path.c_str());

    while (!g_quit) {
      lcd_.maintain();
      pollfd pfds[2];
      pfds[0] = {sock, POLLIN, 0};
      pfds[1] = {lcd_.fd(), POLLIN, 0};
      ::poll(pfds, lcd_.fd() >= 0 ? 2 : 1, 100);
      handleSocket(sock);
      for (char c : lcd_.readButtons())
        step(c == 'L' ? -1 : 1);
      stats_.update();
      lcd_.show(compose());
    }
    ::close(sock);
    ::unlink(path.c_str());
    return 0;
  }

private:
  // ---- control socket ------------------------------------------------------
  static int openControlSocket(const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      std::fprintf(stderr, "muzxlcd: socket path too long\n");
      return -1;
    }
    std::string reply;
    if (ctlSend(path, "ping", &reply, 300)) {
      std::fprintf(stderr, "muzxlcd: already running\n");
      return -1;
    }
    std::strcpy(addr.sun_path, path.c_str());
    ::unlink(path.c_str());
    const int fd =
        ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0 ||
        ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      std::fprintf(stderr, "muzxlcd: cannot create %s: %s\n", path.c_str(),
                   std::strerror(errno));
      if (fd >= 0)
        ::close(fd);
      return -1;
    }
    ::chmod(path.c_str(), 0600);
    return fd;
  }

  void handleSocket(int sock) {
    for (int guard = 0; guard < 50; ++guard) {
      char buf[2048];
      sockaddr_un from{};
      socklen_t fromLen = sizeof(from);
      const ssize_t n =
          ::recvfrom(sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                     reinterpret_cast<sockaddr *>(&from), &fromLen);
      if (n <= 0)
        return;
      std::string msg(buf, static_cast<std::size_t>(n));
      while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
        msg.pop_back();
      const std::string reply = command(msg);
      if (fromLen > sizeof(sa_family_t) && !reply.empty())
        ::sendto(sock, reply.data(), reply.size(), MSG_DONTWAIT,
                 reinterpret_cast<sockaddr *>(&from), fromLen);
    }
  }

  std::string command(const std::string &msg) {
    const std::size_t sp = msg.find(' ');
    const std::string cmd = lower(msg.substr(0, sp));
    const std::string arg =
        sp == std::string::npos ? "" : trim(msg.substr(sp + 1));
    if (cmd == "ping")
      return "pong";
    if (cmd == "lyric") { // pushed by MUZX
      pushed_ = arg;
      pushedAt_ = Clock::now();
      return "ok";
    }
    if (cmd == "kana") { // the player's "Japanese as katakana" setting
      lcd_.setKana(arg == "1");
      return "ok";
    }
    if (cmd == "mode") {
      const std::string want = lower(arg);
      if (want == "next" || want.empty()) {
        step(1);
      } else if (want == "prev" || want == "previous") {
        step(-1);
      } else {
        bool found = false;
        for (std::size_t i = 0; i < cfg_.modes.size(); ++i) {
          const std::string &m = cfg_.modes[i];
          if (m == want || lower(modeTitle(m)) == want ||
              (want == "time" && m == "clock") ||
              (want == "pc" && m == "status")) {
            setMode(i);
            found = true;
            break;
          }
        }
        if (!found)
          return "unknown mode '" + arg + "' (try: " + join(cfg_.modes) + ")";
      }
      return "mode " + cfg_.modes[mode_];
    }
    if (cmd == "modes")
      return join(cfg_.modes);
    if (cmd == "status") {
      return "mode=" + cfg_.modes[mode_] + " modes=" + join(cfg_.modes) +
             " lcd=" +
             (lcd_.connected() ? lcd_.port() : std::string("not connected"));
    }
    if (cmd == "quit") {
      g_quit = true;
      return "bye";
    }
    return "unknown command (mode, modes, status, quit)";
  }

  static std::string join(const std::vector<std::string> &v) {
    std::string out;
    for (const std::string &s : v)
      out += (out.empty() ? "" : ",") + s;
    return out;
  }

public:
  // Sends one datagram to the running program. When `reply` is given, waits
  // for the answer.
  static bool ctlSend(const std::string &path, const std::string &msg,
                      std::string *reply, int timeoutMs) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
      return false;
    // bind to an automatic (abstract) address so the answer can come back
    sockaddr_un me{};
    me.sun_family = AF_UNIX;
    ::bind(fd, reinterpret_cast<sockaddr *>(&me), sizeof(sa_family_t));
    sockaddr_un to{};
    to.sun_family = AF_UNIX;
    if (path.size() >= sizeof(to.sun_path)) {
      ::close(fd);
      return false;
    }
    std::strcpy(to.sun_path, path.c_str());
    if (::sendto(fd, msg.data(), msg.size(), 0,
                 reinterpret_cast<sockaddr *>(&to), sizeof(to)) < 0) {
      ::close(fd);
      return false;
    }
    bool ok = true;
    if (reply) {
      ok = false;
      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, timeoutMs) > 0) {
        char buf[2048];
        const ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
        if (n >= 0) {
          reply->assign(buf, static_cast<std::size_t>(n));
          ok = true;
        }
      }
    }
    ::close(fd);
    return ok;
  }

private:
  // ---- modes ---------------------------------------------------------------
  void step(int dir) {
    const int n = static_cast<int>(cfg_.modes.size());
    setMode(static_cast<std::size_t>(((static_cast<int>(mode_) + dir) % n + n) %
                                     n));
  }

  void setMode(std::size_t i) {
    mode_ = i;
    bannerUntil_ = Clock::now() + std::chrono::milliseconds(1200);
    start_ = Clock::now() + std::chrono::milliseconds(
                                1200); // status pages start from the first
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    std::ofstream(configDir() / "lcd.state") << cfg_.modes[mode_] << "\n";
  }

  static std::string trackText(const PlayerInfo &p) {
    const std::string title =
        cleanTitle(p.title).empty() ? p.title : cleanTitle(p.title);
    const std::string artist = cleanArtist(p.artist);
    if (title.empty())
      return artist;
    return artist.empty() ? title : title + " - " + artist;
  }

  Frame compose() {
    const auto now = Clock::now();
    const std::string &mode = cfg_.modes[mode_];
    const int cols = cfg_.cols;
    if (now < bannerUntil_)
      return rowsFrame(center("Mode " + std::to_string(mode_ + 1) + "/" +
                                  std::to_string(cfg_.modes.size()),
                              cols),
                       center(modeTitle(mode), cols));
    const std::vector<PlayerInfo> ps = mpris_.players();

    if (mode == "clock") {
      const std::time_t t = std::time(nullptr);
      std::tm tm{};
      localtime_r(&t, &tm);
      char a[32], b[40];
      std::strftime(a, sizeof(a), "%H:%M:%S", &tm);
      std::strftime(b, sizeof(b), "%a %d %b %Y", &tm);
      return rowsFrame(center(a, cols), center(b, cols));
    }
    if (mode == "status")
      return statusFrame(now);
    if (mode == "nowplaying") {
      const PlayerInfo *p = nullptr;
      for (const PlayerInfo &c : ps)
        if (c.playing() && !c.title.empty()) {
          p = &c;
          break;
        }
      if (!p)
        for (const PlayerInfo &c : ps)
          if (c.status == "Paused" && !c.title.empty()) {
            p = &c;
            break;
          }
      if (!p)
        return rowsFrame(center("Nothing", cols), center("playing", cols));
      return textFrame((p->playing() ? "" : "|| ") + trackText(*p));
    }
    if (mode == "captions")
      return captionsFrame(ps, now);
    return lyricsFrame(ps, now);
  }

  Frame statusFrame(Clock::time_point now) {
    const int cols = cfg_.cols;
    const int pages = 3;
    const int page =
        static_cast<int>(std::chrono::duration<double>(now - start_).count() /
                         cfg_.pageSeconds) %
        pages;
    char a[64], b[64];
    if (page == 0) {
      std::snprintf(a, sizeof(a), "CPU%3.0f%% RAM%3.0f%%", stats_.cpu(),
                    stats_.ram());
      if (stats_.temp() >= 0)
        std::snprintf(b, sizeof(b), "Temp%3.0fC Ld %.1f", stats_.temp(),
                      stats_.load());
      else
        std::snprintf(b, sizeof(b), "Load %.2f", stats_.load());
    } else if (page == 1) {
      std::snprintf(a, sizeof(a), "Net down %s",
                    fmtRate(stats_.down()).c_str());
      std::snprintf(b, sizeof(b), "Net up   %s", fmtRate(stats_.up()).c_str());
    } else {
      if (stats_.battery() >= 0) {
        const std::string st = stats_.batteryState();
        std::snprintf(a, sizeof(a), "Bat %d%% %s", stats_.battery(),
                      st == "Charging" ? "chrg"
                      : st == "Full"   ? "full"
                                       : "");
      } else {
        std::snprintf(a, sizeof(a), "No battery");
      }
      const long up = static_cast<long>(stats_.uptime());
      if (up >= 86400)
        std::snprintf(b, sizeof(b), "Up %ldd %ldh", up / 86400,
                      (up % 86400) / 3600);
      else
        std::snprintf(b, sizeof(b), "Up %ldh %02ldm", up / 3600,
                      (up % 3600) / 60);
    }
    return rowsFrame(fit(a, cols), fit(b, cols));
  }

  Frame lyricsFrame(const std::vector<PlayerInfo> &ps, Clock::time_point now) {
    const int cols = cfg_.cols;
    const bool muzxPlaying =
        std::any_of(ps.begin(), ps.end(), [](const PlayerInfo &p) {
          return p.isMuzx() && p.playing();
        });
    const bool pushedFresh = now - pushedAt_ < std::chrono::seconds(4);
    const PlayerInfo *p = nullptr;
    if (!muzxPlaying) {
      for (const PlayerInfo &c : ps)
        if (!c.isMuzx() && c.playing() && !c.title.empty()) {
          p = &c;
          break;
        }
      if (!p && !pushedFresh)
        for (const PlayerInfo &c : ps)
          if (!c.isMuzx() && c.status == "Paused" && !c.title.empty()) {
            p = &c;
            break;
          }
    }
    if (!p) {
      if (pushedFresh)
        return textFrame(pushed_); // MUZX sends its own lyrics line
      return rowsFrame(center("No music", cols), center("playing", cols));
    }
    // A failed lookup (no network yet, ...) is tried again after a while.
    const bool retry = lyrics_ && lyrics_->state == Lookup::Failed &&
                       lyrics_->ageSeconds() > 20.0;
    if (!lyrics_ || lyrics_->key != p->key() || retry)
      lyrics_ = startLyricsLookup(*p);
    if (lyrics_->state == Lookup::Ready) {
      std::lock_guard<std::mutex> lock(lyrics_->mu);
      const int idx = lineAt(lyrics_->lines, p->position(now) + 0.25);
      if (idx >= 0)
        return textFrame(
            lyrics_->lines[static_cast<std::size_t>(idx)].text.empty()
                ? "~ music ~"
                : lyrics_->lines[static_cast<std::size_t>(idx)].text);
    }
    return textFrame(
        trackText(*p)); // searching, not found, or before the first line
  }

  Frame captionsFrame(const std::vector<PlayerInfo> &ps,
                      Clock::time_point now) {
    const int cols = cfg_.cols;
    const PlayerInfo *p = nullptr;
    for (const PlayerInfo &c : ps)
      if (c.isYoutube() && c.playing()) {
        p = &c;
        break;
      }
    if (!p)
      for (const PlayerInfo &c : ps)
        if (c.isYoutube() && !youtubeId(c.url).empty()) {
          p = &c;
          break;
        }
    if (!p || youtubeId(p->url).empty())
      return rowsFrame(center("No YouTube", cols),
                       center("video playing", cols));
    const std::string key = youtubeId(p->url) + "|" + cfg_.captionsLang;
    // Try again later: yt-dlp may have been installed, or the network was down.
    bool retry = false;
    if (captions_) {
      const int st = captions_->state;
      const double age = captions_->ageSeconds();
      retry = (st == Lookup::NoTool && age > 15.0) ||
              ((st == Lookup::Failed || st == Lookup::None) && age > 120.0);
    }
    if (!captions_ || captions_->key != key || retry)
      captions_ = startCaptionLookup(*p, cfg_.captionsLang);
    switch (captions_->state.load()) {
    case Lookup::Searching:
      return rowsFrame(center("Getting", cols), center("captions...", cols));
    case Lookup::NoTool:
      return rowsFrame(center("Install yt-dlp", cols),
                       center("for captions", cols));
    case Lookup::None:
    case Lookup::Failed:
      return rowsFrame(center("No captions", cols),
                       center("for this video", cols));
    default:
      break;
    }
    std::lock_guard<std::mutex> lock(captions_->mu);
    const double pos = p->position(now);
    const int idx = lineAt(captions_->lines, pos);
    if (idx >= 0 &&
        pos < captions_->lines[static_cast<std::size_t>(idx)].end + 0.3)
      return textFrame(captions_->lines[static_cast<std::size_t>(idx)].text);
    return textFrame("");
  }

  Config cfg_;
  Lcd lcd_;
  SystemStats stats_;
  MprisWatcher mpris_;
  std::size_t mode_ = 0;
  Clock::time_point bannerUntil_{}, pushedAt_{};
  Clock::time_point start_ = Clock::now();
  std::string pushed_;
  std::shared_ptr<Lookup> lyrics_, captions_;
};

int main(int argc, char **argv) {
  if (argc >= 2 &&
      (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
    std::puts("muzxlcd            start the LCD program (keep it running in "
              "the background)\n"
              "muzxlcd ctl mode next | prev | lyrics | captions | nowplaying | "
              "clock | status\n"
              "muzxlcd ctl modes | status | quit");
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "ctl") {
    std::string msg;
    for (int i = 2; i < argc; ++i)
      msg += (msg.empty() ? "" : " ") + std::string(argv[i]);
    if (msg.empty())
      msg = "status";
    std::string reply;
    if (!Hub::ctlSend(socketPath(), msg, &reply, 1500)) {
      std::fprintf(stderr,
                   "muzxlcd is not running (start it with: muzxlcd &)\n");
      return 1;
    }
    std::puts(reply.c_str());
    return 0;
  }
  Hub hub(loadConfig());
  return hub.run();
}
