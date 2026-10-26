#include "lyrics.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

extern char **environ;

namespace lyrics {

// ===========================================================================
// Small text helpers
// ===========================================================================
namespace {

std::string trimCopy(const std::string &s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

std::string lowerCopy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool startsWith(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string shellQuote(const std::string &input) {
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

// Removes every occurrence of `needle`, ignoring upper/lower case.
void eraseAllCI(std::string &text, const std::string &needle) {
  const std::string lowerNeedle = lowerCopy(needle);
  for (;;) {
    const auto pos = lowerCopy(text).find(lowerNeedle);
    if (pos == std::string::npos)
      return;
    text.erase(pos, needle.size());
  }
}

struct CommandOutput {
  std::string text;
  int status = -1; // exit status, -1 if it could not be run
};

CommandOutput runCommand(const std::string &cmd) {
  constexpr std::size_t kMaxBytes = 4 * 1024 * 1024;
  CommandOutput out;
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return out;
  char buffer[4096];
  std::size_t n = 0;
  while ((n = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
    if (out.text.size() + n <= kMaxBytes)
      out.text.append(buffer, n);
  }
  const int st = pclose(pipe);
  out.status = (st != -1 && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
  return out;
}

std::optional<std::string> readFile(const fs::path &path) {
  constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  std::string data((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (data.size() > kMaxBytes)
    return std::nullopt;
  return data;
}

// Writes via a temp file + rename so a crash cannot leave half a file.
void writeFileAtomic(const fs::path &path, const std::string &data) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return;
    out << data;
    out.flush();
    if (!out)
      return;
  }
  fs::rename(tmp, path, ec);
}

std::string hexHash(const std::string &text) { // FNV-1a, 64 bit
  std::uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : text) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(h));
  return buf;
}

} // namespace

std::string sanitize(const std::string &in) {
  // Lyrics can come from the internet, so drop everything that could act as a
  // terminal command (ESC and other control codes, including the C1 range).
  std::string out;
  out.reserve(in.size());
  for (std::size_t i = 0; i < in.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '\t') {
      out.push_back(' ');
      continue;
    }
    if (c < 0x20 || c == 0x7F)
      continue;
    if (c == 0xC2 && i + 1 < in.size()) {
      const unsigned char d = static_cast<unsigned char>(in[i + 1]);
      if (d >= 0x80 && d <= 0x9F) { // U+0080..U+009F
        ++i;
        continue;
      }
    }
    out.push_back(static_cast<char>(c));
  }
  return out;
}

std::string urlEncode(const std::string &text) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
}

// "SpotiDownloader.com", "spotdown.org", ... : the name of a website.
static bool looksLikeDomain(const std::string &word) {
  const std::string w = lowerCopy(word);
  const auto dot = w.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= w.size())
    return false;
  static const char *const kTlds[] = {
      "com", "org", "net", "io", "in",  "co",   "app",  "me",
      "fm",  "to",  "cc",  "tv", "xyz", "info", "site", "online"};
  const std::string tld = w.substr(dot + 1);
  bool known = false;
  for (const char *t : kTlds)
    known = known || tld == t;
  if (!known)
    return false;
  for (std::size_t i = 0; i < dot; ++i) {
    const unsigned char c = static_cast<unsigned char>(w[i]);
    if (!(std::isalnum(c) || c == '-' || c == '.'))
      return false;
  }
  return true;
}

// "BURNING UP FOR YOU_spotdown.org.mp3" -> "BURNING UP FOR YOU"
// "SpotiDownloader.com - Main Agar Saamne - Abhijeet.mp3"
//     -> "Main Agar Saamne - Abhijeet"
std::string cleanTitleFromFilename(const std::string &path) {
  const std::string stem = fs::path(path).stem().string();
  std::string s = stem;
  eraseAllCI(s, "_spotdown.org");
  eraseAllCI(s, "spotdown.org");

  // Drop anything in (...) [...] {...}: "(youtube)", "(slowed+reverb)", ...
  std::string noBrackets;
  int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') {
      ++depth;
    } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
      --depth;
    } else if (depth == 0) {
      noBrackets.push_back(c == '_' ? ' ' : c);
    }
  }

  // Collapse runs of spaces.
  std::string out;
  bool lastSpace = true;
  for (char c : noBrackets) {
    const bool space = (c == ' ' || c == '\t');
    if (space && lastSpace)
      continue;
    out.push_back(space ? ' ' : c);
    lastSpace = space;
  }
  out = trimCopy(out);

  // Drop website names and the dashes they leave behind at the ends.
  {
    std::istringstream words(out);
    std::string word, kept;
    while (words >> word) {
      if (looksLikeDomain(word))
        continue;
      if (!kept.empty())
        kept += ' ';
      kept += word;
    }
    out = kept;
    while (startsWith(out, "- "))
      out.erase(0, 2);
    while (out.size() >= 2 && out.compare(out.size() - 2, 2, " -") == 0)
      out.erase(out.size() - 2);
    if (out == "-")
      out.clear();
    out = trimCopy(out);
  }
  return out.empty() ? stem : out;
}

// ===========================================================================
// LRC / plain text parsing
// ===========================================================================
namespace {

// "01:23.45" / "1:23" / "01:23:45"  ->  seconds
bool parseTimestamp(const std::string &tag, double &seconds) {
  const auto colon = tag.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= tag.size())
    return false;
  for (char c : tag) {
    if (!(std::isdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.'))
      return false;
  }
  for (std::size_t i = 0; i < colon; ++i) {
    if (!std::isdigit(static_cast<unsigned char>(tag[i])))
      return false;
  }
  if (!std::isdigit(static_cast<unsigned char>(tag[colon + 1])))
    return false;

  const double minutes = std::strtod(tag.substr(0, colon).c_str(), nullptr);
  std::string rest = tag.substr(colon + 1);
  std::replace(rest.begin(), rest.end(), ':', '.'); // "ss:xx" -> "ss.xx"
  const double secs = std::strtod(rest.c_str(), nullptr);
  seconds = minutes * 60.0 + secs;
  return true;
}

std::string stripInlineTags(const std::string &text) { // enhanced LRC <00:01.2>
  std::string out;
  bool inTag = false;
  for (char c : text) {
    if (c == '<') {
      inTag = true;
    } else if (c == '>' && inTag) {
      inTag = false;
    } else if (!inTag) {
      out.push_back(c);
    }
  }
  return out;
}

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines;
  std::string current;
  for (char c : text) {
    if (c == '\n') {
      if (!current.empty() && current.back() == '\r')
        current.pop_back();
      lines.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty() && current.back() == '\r')
    current.pop_back();
  if (!current.empty())
    lines.push_back(current);
  return lines;
}

} // namespace

Result parseLrc(const std::string &input) {
  Result result;
  std::string text = input;
  if (startsWith(text, "\xEF\xBB\xBF")) // UTF-8 byte order mark
    text.erase(0, 3);

  double offsetSeconds = 0.0;
  for (const std::string &raw : splitLines(text)) {
    std::size_t i = 0;
    std::vector<double> stamps;
    bool metaLine = false;

    while (i < raw.size() && raw[i] == '[') {
      const auto close = raw.find(']', i);
      if (close == std::string::npos)
        break;
      const std::string tag = raw.substr(i + 1, close - i - 1);
      double t = 0.0;
      if (parseTimestamp(tag, t)) {
        stamps.push_back(t);
        i = close + 1;
      } else {
        if (stamps.empty()) { // [ar:..] [ti:..] [offset:..] ...
          metaLine = true;
          const std::string lower = lowerCopy(tag);
          if (startsWith(lower, "offset:"))
            offsetSeconds =
                std::strtod(trimCopy(tag.substr(7)).c_str(), nullptr) / 1000.0;
        }
        break;
      }
    }

    if (metaLine || stamps.empty())
      continue;

    const std::string lineText =
        sanitize(trimCopy(stripInlineTags(raw.substr(i))));
    for (double t : stamps)
      result.lines.push_back({std::max(0.0, t - offsetSeconds), lineText});
  }

  std::stable_sort(
      result.lines.begin(), result.lines.end(),
      [](const Line &a, const Line &b) { return a.time < b.time; });
  result.synced = !result.lines.empty();
  return result;
}

Result parsePlain(const std::string &input) {
  Result result;
  std::string text = input;
  if (startsWith(text, "\xEF\xBB\xBF"))
    text.erase(0, 3);

  bool lastBlank = true; // also drops blank lines at the very start
  for (const std::string &raw : splitLines(text)) {
    const std::string line = sanitize(trimCopy(raw));
    if (line.empty()) {
      if (!lastBlank)
        result.lines.push_back({0.0, ""});
      lastBlank = true;
    } else {
      result.lines.push_back({0.0, line});
      lastBlank = false;
    }
  }
  while (!result.lines.empty() && result.lines.back().text.empty())
    result.lines.pop_back();
  result.synced = false;
  return result;
}

std::string toLrc(const Result &result) {
  std::string out;
  for (const Line &line : result.lines) {
    if (result.synced) {
      const long long cs = std::llround(line.time * 100.0);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "[%02lld:%02lld.%02lld]", cs / 6000,
                    (cs % 6000) / 100, cs % 100);
      out += buf;
    }
    out += line.text;
    out += "\n";
  }
  return out;
}

// ===========================================================================
// Minimal JSON reader (enough for LRCLIB replies)
// ===========================================================================
namespace {

struct JsonObject {
  std::map<std::string, std::string> str;
  std::map<std::string, double> num; // numbers and true(1)/false(0)
};

void skipWs(const char *&p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
    ++p;
}

void appendUtf8(std::string &out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool parseHex4(const char *p, const char *e, std::uint32_t &out) {
  if (e - p < 4)
    return false;
  out = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    out <<= 4;
    if (c >= '0' && c <= '9')
      out |= static_cast<std::uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      out |= static_cast<std::uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      out |= static_cast<std::uint32_t>(c - 'A' + 10);
    else
      return false;
  }
  return true;
}

// p points at the opening quote; on success p is just after the closing one.
bool parseJsonString(const char *&p, const char *e, std::string &out) {
  if (p >= e || *p != '"')
    return false;
  ++p;
  out.clear();
  while (p < e) {
    const char c = *p++;
    if (c == '"')
      return true;
    if (c != '\\') {
      out.push_back(c);
      continue;
    }
    if (p >= e)
      return false;
    const char esc = *p++;
    switch (esc) {
    case '"':
      out.push_back('"');
      break;
    case '\\':
      out.push_back('\\');
      break;
    case '/':
      out.push_back('/');
      break;
    case 'b':
      out.push_back('\b');
      break;
    case 'f':
      out.push_back('\f');
      break;
    case 'n':
      out.push_back('\n');
      break;
    case 'r':
      out.push_back('\r');
      break;
    case 't':
      out.push_back('\t');
      break;
    case 'u': {
      std::uint32_t cp = 0;
      if (!parseHex4(p, e, cp))
        return false;
      p += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' &&
          p[1] == 'u') {
        std::uint32_t low = 0;
        if (parseHex4(p + 2, e, low) && low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          p += 6;
        }
      }
      appendUtf8(out, cp);
      break;
    }
    default:
      return false;
    }
  }
  return false;
}

// Skips one JSON value of any type (used for values we do not need).
bool skipJsonValue(const char *&p, const char *e) {
  skipWs(p, e);
  if (p >= e)
    return false;
  if (*p == '"') {
    std::string ignored;
    return parseJsonString(p, e, ignored);
  }
  if (*p == '{' || *p == '[') {
    int depth = 0;
    while (p < e) {
      if (*p == '"') {
        std::string ignored;
        if (!parseJsonString(p, e, ignored))
          return false;
        continue;
      }
      if (*p == '{' || *p == '[')
        ++depth;
      else if (*p == '}' || *p == ']')
        --depth;
      ++p;
      if (depth == 0)
        return true;
    }
    return false;
  }
  while (p < e && *p != ',' && *p != '}' && *p != ']')
    ++p;
  return true;
}

bool parseJsonObject(const char *&p, const char *e, JsonObject &obj) {
  skipWs(p, e);
  if (p >= e || *p != '{')
    return false;
  ++p;
  for (;;) {
    skipWs(p, e);
    if (p >= e)
      return false;
    if (*p == '}') {
      ++p;
      return true;
    }
    if (*p == ',') {
      ++p;
      continue;
    }
    std::string key;
    if (!parseJsonString(p, e, key))
      return false;
    skipWs(p, e);
    if (p >= e || *p != ':')
      return false;
    ++p;
    skipWs(p, e);
    if (p >= e)
      return false;

    if (*p == '"') {
      std::string value;
      if (!parseJsonString(p, e, value))
        return false;
      obj.str[key] = value;
    } else if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p))) {
      char *endPtr = nullptr;
      const double v = std::strtod(p, &endPtr);
      if (endPtr == p)
        return false;
      obj.num[key] = v;
      p = endPtr;
    } else if (e - p >= 4 && std::string(p, 4) == "true") {
      obj.num[key] = 1.0;
      p += 4;
    } else if (e - p >= 5 && std::string(p, 5) == "false") {
      obj.num[key] = 0.0;
      p += 5;
    } else if (!skipJsonValue(p, e)) { // null, nested objects, arrays
      return false;
    }
  }
}

std::vector<JsonObject> parseJsonObjects(const std::string &body) {
  std::vector<JsonObject> objects;
  const char *p = body.data();
  const char *e = p + body.size();
  skipWs(p, e);
  if (p >= e)
    return objects;

  if (*p == '{') {
    JsonObject obj;
    if (parseJsonObject(p, e, obj))
      objects.push_back(std::move(obj));
    return objects;
  }
  if (*p != '[')
    return objects;
  ++p;
  for (;;) {
    skipWs(p, e);
    if (p >= e || *p == ']')
      break;
    if (*p == ',') {
      ++p;
      continue;
    }
    JsonObject obj;
    if (*p == '{' && parseJsonObject(p, e, obj))
      objects.push_back(std::move(obj));
    else
      break; // malformed: stop with what we have
  }
  return objects;
}

} // namespace

// Lower-case letters and digits only (other scripts are kept as they are), so
// "Main Agar Saamne (From "Raaz")" and "main agar saamne" can be compared.
static std::string matchKey(const std::string &text) {
  std::string key;
  for (unsigned char c : text) {
    if (std::isalnum(c))
      key.push_back(static_cast<char>(std::tolower(c)));
    else if (c >= 0x80)
      key.push_back(static_cast<char>(c));
  }
  return key;
}

void pickFromLrclib(const std::string &json, double duration, Result &synced,
                    Result &plain,
                    const std::vector<std::string> &wantedTitles) {
  constexpr double kSyncTolerance = 5.0; // seconds
  synced = Result{};
  plain = Result{};

  std::vector<std::string> wantedKeys;
  for (const std::string &title : wantedTitles) {
    std::string key = matchKey(title);
    if (key.size() >= 4)
      wantedKeys.push_back(std::move(key));
  }

  double bestDiff = 1e18;
  for (const JsonObject &obj : parseJsonObjects(json)) {
    // A search for "Abhijeet" also returns other songs of that artist: only
    // accept results whose track name matches what we are looking for.
    if (!wantedKeys.empty()) {
      const auto itName = obj.str.find("trackName");
      if (itName != obj.str.end()) {
        const std::string nameKey = matchKey(itName->second);
        bool related = false;
        for (const std::string &wanted : wantedKeys) {
          if (nameKey.size() >= 3 &&
              (nameKey.find(wanted) != std::string::npos ||
               wanted.find(nameKey) != std::string::npos))
            related = true;
        }
        if (!related)
          continue;
      }
    }

    const auto itSynced = obj.str.find("syncedLyrics");
    const auto itPlain = obj.str.find("plainLyrics");
    const auto itDur = obj.num.find("duration");
    const double objDuration = itDur != obj.num.end() ? itDur->second : 0.0;

    if (itSynced != obj.str.end() && !trimCopy(itSynced->second).empty()) {
      const double diff = (duration > 0.0 && objDuration > 0.0)
                              ? std::fabs(objDuration - duration)
                              : 0.0; // length unknown on either side: accept
      if (diff <= kSyncTolerance && diff < bestDiff) {
        Result r = parseLrc(itSynced->second);
        if (!r.lines.empty()) {
          r.source = "lrclib";
          synced = std::move(r);
          bestDiff = diff;
        }
      }
    }
    if (plain.lines.empty() && itPlain != obj.str.end() &&
        !trimCopy(itPlain->second).empty()) {
      Result r = parsePlain(itPlain->second);
      if (!r.lines.empty()) {
        r.source = "lrclib";
        plain = std::move(r);
      }
    }
  }
}

// ===========================================================================
// Finding lyrics for one song (runs on the worker thread)
// ===========================================================================
namespace {

struct Tags {
  std::string title, artist, album, lyrics;
};

// Reads tags with ffprobe. Output lines look like "TAG:title=Foo"; a value
// can continue over following lines (multi-line lyrics).
Tags readTags(const std::string &songPath) {
  Tags tags;
  const CommandOutput out =
      runCommand("ffprobe -v error -show_entries format_tags:stream_tags "
                 "-of default=noprint_wrappers=1 " +
                 shellQuote(songPath) + " 2>/dev/null");
  if (out.status != 0)
    return tags;

  std::string key;
  std::string *target = nullptr;
  std::istringstream in(out.text);
  std::string line;
  while (std::getline(in, line)) {
    if (startsWith(line, "TAG:")) {
      const auto eq = line.find('=');
      if (eq == std::string::npos) {
        target = nullptr;
        continue;
      }
      key = lowerCopy(line.substr(4, eq - 4));
      const std::string value = line.substr(eq + 1);
      target = nullptr;
      if (key == "title")
        target = &tags.title;
      else if (key == "artist")
        target = &tags.artist;
      else if (key == "album")
        target = &tags.album;
      else if (startsWith(key, "lyrics") || startsWith(key, "unsyncedlyrics") ||
               key == "uslt")
        target = &tags.lyrics;
      if (target && target->empty())
        *target = value;
      else
        target = nullptr; // keep the first value of each tag only
    } else if (target) {
      *target += "\n" + line; // continuation of a multi-line value
    }
  }
  tags.title = trimCopy(tags.title);
  tags.artist = trimCopy(tags.artist);
  tags.album = trimCopy(tags.album);
  return tags;
}

std::string lrclibBase() {
  const char *env = std::getenv("MUZX_LRCLIB_URL");
  if (env && *env) {
    std::string base = env;
    while (!base.empty() && base.back() == '/')
      base.pop_back();
    return base;
  }
  return "https://lrclib.net";
}

// Downloads one URL. `transient` is set when it failed for a reason that says
// nothing about the lyrics (no network, timeout), so we must not remember
// "not found" for that song.
// Set by the worker thread: returns true once the song this lookup is for was
// skipped, so slow network steps can be abandoned.
thread_local const std::function<bool()> *tlsStale = nullptr;

std::string fetchUrl(const std::string &url, bool &transient) {
  if (tlsStale && (*tlsStale)()) { // skipped: do not ask the server any more
    transient = true;
    return "";
  }
  const CommandOutput out = runCommand("curl -s -f -m 6 -A 'muzx/1.0' -- " +
                                       shellQuote(url) + " 2>/dev/null");
  if (out.status == 0)
    return out.text;
  if (out.status != 22) // 22 = the server answered with an HTTP error (404)
    transient = true;
  return "";
}

std::string findExecutable(const std::string &name); // defined below

// "Main Agar Saamne - Abhijeet" -> {"Main Agar Saamne", "Abhijeet"}
// (download sites join artist and title with " - ").
std::vector<std::string> splitDashes(const std::string &text) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const auto pos = text.find(" - ", start);
    const std::string part = trimCopy(text.substr(
        start, pos == std::string::npos ? std::string::npos : pos - start));
    if (!part.empty())
      parts.push_back(part);
    if (pos == std::string::npos)
      break;
    start = pos + 3;
  }
  return parts;
}

bool lookupOnline(const Tags &tags, const std::string &fallbackTitle,
                  double duration, Result &out, bool &transient) {
  const std::string base = lrclibBase();
  const std::string title = tags.title.empty() ? fallbackTitle : tags.title;

  const std::vector<std::string> parts = splitDashes(title);
  std::string
      flat; // "Main Agar Saamne - Abhijeet" -> "Main Agar Saamne Abhijeet"
  for (const std::string &p : parts)
    flat += (flat.empty() ? "" : " ") + p;
  if (flat.empty())
    flat = title;

  // Search results must look like this song (by track name) to be accepted.
  std::vector<std::string> wanted = {flat};
  for (const std::string &p : parts)
    wanted.push_back(p);

  Result plainFallback;
  bool havePlain = false;
  // Returns true as soon as timestamped lyrics were found.
  auto consider = [&](const std::string &body, bool filtered = true) -> bool {
    Result synced, plain;
    pickFromLrclib(body, duration, synced, plain,
                   filtered ? wanted : std::vector<std::string>{});
    if (!synced.lines.empty()) {
      out = std::move(synced);
      return true;
    }
    if (!havePlain && !plain.lines.empty()) {
      plainFallback = std::move(plain);
      havePlain = true;
    }
    return false;
  };

  // The network is down (or this lookup was abandoned): stop asking, but keep
  // plain lyrics if one of the earlier answers had them.
  auto giveUp = [&]() -> bool {
    if (!havePlain)
      return false;
    out = std::move(plainFallback);
    return true;
  };

  // 1) exact match by tags
  if (!tags.title.empty() && !tags.artist.empty()) {
    std::string url = base + "/api/get?track_name=" + urlEncode(tags.title) +
                      "&artist_name=" + urlEncode(tags.artist);
    if (!tags.album.empty())
      url += "&album_name=" + urlEncode(tags.album);
    if (duration > 0.0)
      url += "&duration=" +
             std::to_string(static_cast<long>(std::lround(duration)));
    if (consider(fetchUrl(url, transient), false))
      return true;
    if (transient)
      return giveUp();
  }
  // 2) search by title + artist
  if (!tags.artist.empty()) {
    if (consider(fetchUrl(base + "/api/search?track_name=" + urlEncode(title) +
                              "&artist_name=" + urlEncode(tags.artist),
                          transient)))
      return true;
    if (transient)
      return giveUp();
  }
  // 3) free-text searches (work with just a cleaned-up file name): the whole
  // name first, then each " - " part on its own.
  std::vector<std::string> queries;
  auto addQuery = [&](const std::string &q) {
    const std::string t = trimCopy(q);
    if (!t.empty() &&
        std::find(queries.begin(), queries.end(), t) == queries.end())
      queries.push_back(t);
  };
  addQuery(tags.artist + " " + flat);
  addQuery(flat);
  if (parts.size() > 1) {
    for (const std::string &p : parts)
      addQuery(p);
  }
  if (queries.size() > 3)
    queries.resize(3);
  for (const std::string &q : queries) {
    if (consider(fetchUrl(base + "/api/search?q=" + urlEncode(q), transient)))
      return true;
    if (transient)
      return giveUp();
  }

  if (havePlain) {
    out = std::move(plainFallback);
    return true;
  }
  return false;
}

std::optional<Result> loadSidecar(const std::string &songPath) {
  fs::path p = songPath;
  for (const char *ext : {".lrc", ".LRC", ".txt"}) {
    p.replace_extension(ext);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec))
      continue;
    const auto data = readFile(p);
    if (!data)
      continue;
    Result r = parseLrc(*data);
    if (r.lines.empty())
      r = parsePlain(*data);
    if (!r.lines.empty()) {
      r.source = std::string("local ") + ext;
      return r;
    }
  }
  return std::nullopt;
}

std::optional<Result> loadCache(const fs::path &cacheDir,
                                const std::string &key) {
  for (const char *ext : {".lrc", ".gen.lrc", ".txt"}) {
    const std::string e = ext;
    const fs::path p = cacheDir / "lyrics" / (key + e);
    const auto data = readFile(p);
    if (!data)
      continue;
    Result r = e == ".txt" ? parsePlain(*data) : parseLrc(*data);
    if (!r.lines.empty()) {
      r.source = e == ".gen.lrc" ? "generated" : "cache";
      return r;
    }
  }
  return std::nullopt;
}

void saveCache(const fs::path &cacheDir, const std::string &key,
               const Result &r) {
  if (cacheDir.empty())
    return;
  writeFileAtomic(cacheDir / "lyrics" / (key + (r.synced ? ".lrc" : ".txt")),
                  toLrc(r));
}

// "Not found" is remembered for 3 days so we do not ask the server on every
// single play of a song it does not have.
bool recentlyNotFound(const fs::path &cacheDir, const std::string &key) {
  std::error_code ec;
  const fs::path marker = cacheDir / "lyrics" / (key + ".none");
  const auto t = fs::last_write_time(marker, ec);
  if (ec)
    return false;
  return (fs::file_time_type::clock::now() - t) < std::chrono::hours(72);
}

// Second online source: the `syncedlyrics` command-line tool (pipx install
// syncedlyrics) searches Musixmatch, NetEase, Megalobiz and others for timed
// lyrics. It is optional; without it this step is skipped.
bool lookupSyncedlyrics(const Tags &tags, const std::string &fallbackTitle,
                        double duration, const fs::path &cacheDir, Result &out,
                        bool &transient) {
  const std::string bin = findExecutable("syncedlyrics");
  if (bin.empty())
    return false;
  if (tlsStale && (*tlsStale)()) {
    transient = true;
    return false;
  }

  const std::string title = tags.title.empty() ? fallbackTitle : tags.title;
  std::string term;
  for (const std::string &part : splitDashes(title))
    term += (term.empty() ? "" : " ") + part;
  if (term.empty())
    term = title;
  term = trimCopy(tags.artist + " " + term);
  if (term.empty())
    return false;

  std::error_code ec;
  const fs::path base =
      cacheDir.empty() ? fs::temp_directory_path(ec) : cacheDir / "lyrics";
  const fs::path workDir =
      base / ("sl-" + std::to_string(getpid()) + "-" + hexHash(term));
  fs::create_directories(workDir, ec);
  const fs::path outPath = workDir / "out.lrc";

  // `timeout` keeps a stuck provider from blocking the lookup thread.
  const CommandOutput run = runCommand(
      "timeout 20 " + shellQuote(bin) + " -o " + shellQuote(outPath.string()) +
      " -- " + shellQuote(term) + " </dev/null 2>/dev/null");
  bool found = false;
  if (run.status != 0) {
    transient = true; // tool crashed, timed out or no network
  } else if (const auto data = readFile(outPath)) {
    Result r = parseLrc(*data);
    // A version of the song with a different length would be out of sync.
    const bool tooLong = duration > 0.0 && !r.lines.empty() &&
                         r.lines.back().time > duration + 15.0;
    if (!r.lines.empty() && !tooLong) {
      r.source = "syncedlyrics";
      out = std::move(r);
      found = true;
    }
  }
  fs::remove_all(workDir, ec);
  return found;
}

std::optional<Result> findLyrics(const std::string &songPath, double duration,
                                 const fs::path &cacheDir, bool online) {
  if (auto r = loadSidecar(songPath))
    return r;

  const std::string key = hexHash(songPath);
  if (!cacheDir.empty()) {
    if (auto r = loadCache(cacheDir, key))
      return r;
  }

  const Tags tags = readTags(songPath);

  // Synced lyrics stored inside the tags win over a web lookup.
  Result embedded;
  if (!trimCopy(tags.lyrics).empty()) {
    embedded = parseLrc(tags.lyrics);
    if (embedded.lines.empty())
      embedded = parsePlain(tags.lyrics);
    embedded.source = "embedded";
    if (embedded.synced)
      return embedded;
  }

  if (online && !(!cacheDir.empty() && recentlyNotFound(cacheDir, key))) {
    Result found;
    bool transient = false;
    const std::string cleaned = cleanTitleFromFilename(songPath);
    bool have = lookupOnline(tags, cleaned, duration, found, transient);
    if ((!have || !found.synced) &&
        !transient) { // nothing, or only untimed lyrics: try harder
      Result better;
      if (lookupSyncedlyrics(tags, cleaned, duration, cacheDir, better,
                             transient)) {
        found = std::move(better);
        have = true;
      }
    }
    if (have) {
      saveCache(cacheDir, key, found);
      return found;
    }
    if (!transient && !cacheDir.empty())
      writeFileAtomic(cacheDir / "lyrics" / (key + ".none"), "");
  }

  if (!embedded.lines.empty())
    return embedded;
  return std::nullopt;
}

} // namespace

// ===========================================================================
// Auto-generated lyrics: run Whisper (speech recognition) on the song
// ===========================================================================

// Text that is only music notes / punctuation is a Whisper "hallucination" on
// instrumental parts, not a lyric.
static bool isNoiseLine(std::string text) {
  for (const char *symbol : {"\xE2\x99\xAA", "\xE2\x99\xAB", "\xE2\x99\xAC"}) {
    const std::string s = symbol;
    for (auto pos = text.find(s); pos != std::string::npos; pos = text.find(s))
      text.erase(pos, s.size());
  }
  for (unsigned char c : text) {
    if (std::isalnum(c) || c >= 0x80)
      return false;
  }
  return true;
}

Result parseWhisperJson(const std::string &body) {
  Result result;
  const char *p = body.data();
  const char *e = p + body.size();
  skipWs(p, e);
  if (p >= e || *p != '{')
    return result;
  ++p;

  std::vector<JsonObject> segments;
  for (;;) {
    skipWs(p, e);
    if (p >= e || *p == '}')
      break;
    if (*p == ',') {
      ++p;
      continue;
    }
    std::string key;
    if (!parseJsonString(p, e, key))
      break;
    skipWs(p, e);
    if (p >= e || *p != ':')
      break;
    ++p;
    skipWs(p, e);
    if (key == "segments" && p < e && *p == '[') {
      ++p;
      for (;;) {
        skipWs(p, e);
        if (p >= e)
          break;
        if (*p == ']') {
          ++p;
          break;
        }
        if (*p == ',') {
          ++p;
          continue;
        }
        JsonObject obj;
        if (*p == '{' && parseJsonObject(p, e, obj))
          segments.push_back(std::move(obj));
        else
          break;
      }
    } else if (!skipJsonValue(p, e)) {
      break;
    }
  }

  std::string prev1, prev2;
  for (const JsonObject &seg : segments) {
    const auto itText = seg.str.find("text");
    const auto itStart = seg.num.find("start");
    if (itText == seg.str.end() || itStart == seg.num.end())
      continue;
    const std::string text = sanitize(trimCopy(itText->second));
    if (isNoiseLine(text))
      continue;
    if (text == prev1 && text == prev2) // stuck in a repetition loop
      continue;
    prev2 = prev1;
    prev1 = text;
    result.lines.push_back({std::max(0.0, itStart->second), text});
  }
  std::stable_sort(
      result.lines.begin(), result.lines.end(),
      [](const Line &a, const Line &b) { return a.time < b.time; });
  result.synced = !result.lines.empty();
  return result;
}

namespace {

std::string findExecutable(const std::string &name) {
  if (name.empty())
    return "";
  auto isExec = [](const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && access(path.c_str(), X_OK) == 0;
  };
  if (name.find('/') != std::string::npos)
    return isExec(name) ? name : "";
  const char *pathEnv = std::getenv("PATH");
  if (!pathEnv)
    return "";
  std::istringstream dirs(pathEnv);
  std::string dir;
  while (std::getline(dirs, dir, ':')) {
    if (dir.empty())
      continue;
    const fs::path candidate = fs::path(dir) / name;
    if (isExec(candidate))
      return candidate.string();
  }
  return "";
}

struct GenOutcome {
  std::optional<Result> result;
  std::string error;
};

std::string lastLineOf(const fs::path &path) {
  const auto data = readFile(path);
  if (!data)
    return "";
  // Prefer the last line that names an error (the end of a Python traceback);
  // otherwise the last non-empty line.
  std::string last, lastError;
  for (const std::string &line : splitLines(*data)) {
    const std::string t = trimCopy(line);
    if (t.empty())
      continue;
    last = t;
    const std::string lower = lowerCopy(t);
    if (lower.find("error") != std::string::npos ||
        lower.find("exception") != std::string::npos ||
        lower.find("no such file") != std::string::npos ||
        lower.find("not found") != std::string::npos)
      lastError = t;
  }
  std::string pick = lastError.empty() ? last : lastError;
  if (pick.size() > 100)
    pick.resize(100);
  return sanitize(pick);
}

// Runs `whisper` on the song. It is started in its own process group, at low
// priority (so the music does not stutter), and killed when `cancelled()`
// says so. Everything the child needs is prepared BEFORE fork(), because after
// fork() in a multi-threaded program only a few calls are safe.
GenOutcome runWhisper(const std::string &songPath, const fs::path &workDir,
                      const std::string &model, const std::string &language,
                      const std::function<bool()> &cancelled,
                      const std::function<void(pid_t)> &onPid) {
  GenOutcome out;
  const std::string bin = generatorCommand();
  if (bin.empty()) {
    out.error = "Whisper is not installed";
    return out;
  }

  std::error_code ec;
  fs::create_directories(workDir, ec);
  if (ec) {
    out.error = "cannot create a work folder";
    return out;
  }
  const std::string logPath = (workDir / "log.txt").string();

  std::vector<std::string> args = {bin,
                                   songPath,
                                   "--model",
                                   model,
                                   "--output_format",
                                   "json",
                                   "--output_dir",
                                   workDir.string(),
                                   "--condition_on_previous_text",
                                   "False"};
  if (fs::path(bin).filename() == "whisper") { // openai-whisper only
    args.push_back("--fp16");
    args.push_back("False");
  }
  if (!language.empty()) {
    args.push_back("--language");
    args.push_back(language);
  }
  std::vector<char *> argv;
  for (std::string &a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  std::vector<std::string> envStrings;
  bool hasThreads = false;
  for (char **env = environ; env && *env; ++env) {
    envStrings.emplace_back(*env);
    hasThreads =
        hasThreads || startsWith(envStrings.back(), "OMP_NUM_THREADS=");
  }
  if (!hasThreads) // leave half of the CPU to the player
    envStrings.push_back(
        "OMP_NUM_THREADS=" +
        std::to_string(std::max(1U, std::thread::hardware_concurrency() / 2)));
  std::vector<char *> envp;
  for (std::string &s : envStrings)
    envp.push_back(s.data());
  envp.push_back(nullptr);

  const pid_t pid = fork();
  if (pid < 0) {
    out.error = "cannot start Whisper";
    fs::remove_all(workDir, ec);
    return out;
  }
  if (pid == 0) {
    // ---- child: only async-signal-safe calls from here on ----
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL); // die with the player
    setpriority(PRIO_PROCESS, 0, 15);
    const int devNull = open("/dev/null", O_RDONLY);
    if (devNull >= 0)
      dup2(devNull, STDIN_FILENO);
    const int log = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log >= 0) {
      dup2(log, STDOUT_FILENO);
      dup2(log, STDERR_FILENO);
    }
    execve(argv[0], argv.data(), envp.data());
    _exit(127);
  }

  setpgid(pid, pid); // also from the parent: avoids a race with the child
  onPid(pid);

  const auto startedAt = std::chrono::steady_clock::now();
  int status = 0;
  bool exited = false;
  std::string killedBecause;
  for (;;) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      exited = true;
      break;
    }
    if (r < 0 && errno != EINTR)
      break; // lost track of the child
    if (cancelled())
      killedBecause = "cancelled";
    else if (std::chrono::steady_clock::now() - startedAt >
             std::chrono::minutes(30))
      killedBecause = "timed out after 30 minutes";
    if (!killedBecause.empty()) {
      kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }
  onPid(0);
  // The UI kills the process itself when you cancel or skip the song, so it can
  // look like a crash from here; that must not be reported as a failure.
  if (killedBecause.empty() && cancelled())
    killedBecause = "cancelled";

  // On a real failure, keep everything Whisper printed (plus the command that
  // was run) in whisper-error.log next to the lyrics cache.
  auto keepLog = [&]() {
    std::string text = "command:";
    for (const std::string &a : args)
      text += " '" + a + "'";
    text += "\n\n";
    if (const auto log = readFile(logPath))
      text += *log;
    writeFileAtomic(workDir.parent_path() / "whisper-error.log", text);
  };

  if (!killedBecause.empty()) {
    out.error = killedBecause;
  } else if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const std::string reason = lastLineOf(logPath);
    out.error = "Whisper failed" + (reason.empty() ? "" : ": " + reason);
    keepLog();
  } else {
    for (fs::directory_iterator it(workDir, ec), end; !ec && it != end;
         it.increment(ec)) {
      if (it->path().extension() != ".json")
        continue;
      if (const auto data = readFile(it->path())) {
        Result r = parseWhisperJson(*data);
        if (!r.lines.empty()) {
          out.result = std::move(r);
          break;
        }
      }
    }
    if (!out.result) {
      out.error = "no lyrics detected (instrumental?)";
      keepLog();
    }
  }
  fs::remove_all(workDir, ec);
  return out;
}

void saveCacheGenerated(const fs::path &cacheDir, const std::string &key,
                        const Result &r) {
  if (cacheDir.empty())
    return;
  writeFileAtomic(cacheDir / "lyrics" / (key + ".gen.lrc"), toLrc(r));
}

} // namespace

std::string generatorCommand() {
  if (const char *custom = std::getenv("MUZX_WHISPER"); custom && *custom)
    return findExecutable(custom);
  for (const char *name : {"whisper", "whisper-ctranslate2"}) {
    const std::string found = findExecutable(name);
    if (!found.empty())
      return found;
  }
  return "";
}

std::string generatorModel() {
  if (const char *model = std::getenv("MUZX_WHISPER_MODEL"); model && *model)
    return model;
  return "small";
}

// ===========================================================================
// Manager: one background thread, always working on the newest request
// ===========================================================================
struct Manager::Shared {
  struct Request {
    std::string path;
    double duration = 0.0;
  };

  mutable std::mutex m;
  std::condition_variable cv;
  std::uint64_t generation = 0; // bumped for every new request
  bool hasPending = false;
  bool stop = false;
  bool workerStarted = false;
  Request pending;
  std::string currentSong;
  State state = State::Idle;
  std::shared_ptr<const Result> result;
  fs::path cacheDir;
  bool online = true;

  // auto-generated lyrics
  bool generating = false;
  bool pendingGenerate =
      false; // start Whisper as soon as the search ends empty
  std::string genModel, genLanguage;
  std::uint64_t genToken = 0; // bumped to cancel / invalidate a generation
  pid_t childPid = 0;         // the running whisper process, 0 if none
  std::chrono::steady_clock::time_point genStart;
  std::string lastError;
};

namespace {

// Stops a running generation. The caller holds the lock.
void cancelGenerationLocked(Manager::Shared &s) {
  if (!s.generating)
    return;
  ++s.genToken;
  s.generating = false;
  if (s.childPid > 0) {
    kill(-s.childPid, SIGKILL);
    kill(s.childPid, SIGKILL);
    s.childPid = 0;
  }
}

// Body of the thread that waits for one Whisper run.
void generatorMain(std::shared_ptr<Manager::Shared> sh, std::string songPath,
                   std::uint64_t token, fs::path cacheDir, std::string model,
                   std::string language) {
  std::error_code ec;
  const fs::path base =
      cacheDir.empty() ? fs::temp_directory_path(ec) : cacheDir / "lyrics";
  const fs::path workDir =
      base / ("gen-" + std::to_string(getpid()) + "-" + std::to_string(token));

  auto cancelled = [&]() {
    std::lock_guard<std::mutex> lock(sh->m);
    return sh->stop || sh->genToken != token;
  };
  auto onPid = [&](pid_t pid) {
    std::lock_guard<std::mutex> lock(sh->m);
    if (sh->genToken == token)
      sh->childPid = pid;
  };

  GenOutcome outcome =
      runWhisper(songPath, workDir, model, language, cancelled, onPid);
  if (outcome.result) {
    outcome.result->source = "generated (whisper " + model + ")";
    saveCacheGenerated(cacheDir, hexHash(songPath), *outcome.result);
  }

  std::lock_guard<std::mutex> lock(sh->m);
  if (sh->genToken != token) // cancelled or the song changed: drop the answer
    return;
  sh->generating = false;
  sh->childPid = 0;
  if (outcome.result) {
    sh->result = std::make_shared<const Result>(std::move(*outcome.result));
    sh->state = State::Ready;
    sh->lastError.clear();
  } else {
    sh->result.reset();
    sh->state = State::NotFound;
    sh->lastError = outcome.error;
  }
}

// Starts Whisper for `songPath`. The caller holds the lock.
void startGenerationLocked(const std::shared_ptr<Manager::Shared> &sh,
                           const std::string &songPath) {
  Manager::Shared &s = *sh;
  s.generating = true;
  s.pendingGenerate = false;
  ++s.genToken;
  s.state = State::Generating;
  s.result.reset();
  s.genStart = std::chrono::steady_clock::now();
  s.lastError.clear();
  std::thread(generatorMain, sh, songPath, s.genToken, s.cacheDir, s.genModel,
              s.genLanguage)
      .detach();
}

} // namespace

namespace {

void workerMain(std::shared_ptr<Manager::Shared> sh);

} // namespace

// The worker only needs Shared, so it is a free function that owns a
// shared_ptr: it stays valid even if the Manager is destroyed mid-lookup.
namespace {

void workerMain(std::shared_ptr<Manager::Shared> sh) {
  std::unique_lock<std::mutex> lock(sh->m);
  for (;;) {
    sh->cv.wait(lock, [&] { return sh->hasPending || sh->stop; });
    if (sh->stop)
      return;

    const Manager::Shared::Request req = sh->pending;
    sh->hasPending = false;
    const std::uint64_t gen = sh->generation;
    const fs::path cacheDir = sh->cacheDir;
    const bool online = sh->online;

    lock.unlock();
    const std::function<bool()> stale = [&]() {
      std::lock_guard<std::mutex> guard(sh->m);
      return sh->stop || sh->generation != gen;
    };
    tlsStale = &stale;
    std::optional<Result> found;
    try {
      found = findLyrics(req.path, req.duration, cacheDir, online);
    } catch (...) {
      found.reset();
    }
    tlsStale = nullptr;
    lock.lock();

    if (gen == sh->generation) { // ignore answers for a song we left already
      if (found) {
        sh->result = std::make_shared<const Result>(std::move(*found));
        sh->state = State::Ready;
      } else {
        sh->result.reset();
        sh->state = State::NotFound;
        if (sh->pendingGenerate) // G was pressed during the search
          startGenerationLocked(sh, req.path);
      }
      sh->pendingGenerate = false;
    }
  }
}

} // namespace

Manager::Manager() : shared_(std::make_shared<Shared>()) {}

Manager::~Manager() {
  {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->stop = true;
    cancelGenerationLocked(*shared_); // never leave Whisper running behind us
  }
  shared_->cv.notify_all();
}

void Manager::setCacheDir(const fs::path &dir) {
  {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->cacheDir = dir;
  }
  // Work folders of generations that were killed hard (older than an hour).
  std::error_code ec;
  for (fs::directory_iterator it(dir / "lyrics", ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code entryEc;
    if (startsWith(it->path().filename().string(), "gen-") &&
        it->is_directory(entryEc) &&
        fs::file_time_type::clock::now() -
                fs::last_write_time(it->path(), entryEc) >
            std::chrono::hours(1))
      fs::remove_all(it->path(), entryEc);
  }
}

void Manager::setOnlineEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(shared_->m);
  shared_->online = enabled;
}

void Manager::request(const std::string &songPath, double durationSeconds) {
  std::lock_guard<std::mutex> lock(shared_->m);
  if (songPath == shared_->currentSong)
    return;
  shared_->currentSong = songPath;
  ++shared_->generation;
  cancelGenerationLocked(*shared_);
  shared_->pendingGenerate = false;
  shared_->lastError.clear();
  shared_->state = State::Loading;
  shared_->result.reset(); // never show the previous song's lyrics
  shared_->pending = {songPath, durationSeconds};
  shared_->hasPending = true;

  if (!shared_->workerStarted) {
    shared_->workerStarted = true;
    std::thread(workerMain, shared_).detach();
  }
  shared_->cv.notify_one();
}

State Manager::state() const {
  std::lock_guard<std::mutex> lock(shared_->m);
  return shared_->state;
}

std::shared_ptr<const Result> Manager::result() const {
  std::lock_guard<std::mutex> lock(shared_->m);
  return shared_->result;
}

bool Manager::generate(const std::string &songPath, double /*duration*/) {
  const std::string bin = generatorCommand(); // file-system access: no lock
  const std::string model = generatorModel();
  const char *langEnv = std::getenv("MUZX_WHISPER_LANG");
  const std::string language = langEnv ? langEnv : "";

  std::lock_guard<std::mutex> lock(shared_->m);
  Shared &s = *shared_;
  if (songPath != s.currentSong || s.generating || s.pendingGenerate)
    return false;
  if (bin.empty()) {
    s.lastError = "Whisper is not installed";
    return false;
  }
  s.genModel = model;
  s.genLanguage = language;

  if (s.state == State::Loading) { // still searching: start right after it
    s.pendingGenerate = true;
    s.lastError.clear();
    return true;
  }
  const bool redo = s.state == State::Ready && s.result &&
                    startsWith(s.result->source, "generated");
  if (s.state != State::NotFound && !redo)
    return false;
  startGenerationLocked(shared_, songPath);
  return true;
}

bool Manager::generatePending() const {
  std::lock_guard<std::mutex> lock(shared_->m);
  return shared_->pendingGenerate;
}

void Manager::cancelGenerate() {
  std::lock_guard<std::mutex> lock(shared_->m);
  if (shared_->pendingGenerate) {
    shared_->pendingGenerate = false;
    return;
  }
  if (!shared_->generating)
    return;
  cancelGenerationLocked(*shared_);
  shared_->state = State::NotFound;
  shared_->lastError = "cancelled";
}

double Manager::generatingSeconds() const {
  std::lock_guard<std::mutex> lock(shared_->m);
  if (!shared_->generating)
    return 0.0;
  return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       shared_->genStart)
      .count();
}

std::string Manager::lastError() const {
  std::lock_guard<std::mutex> lock(shared_->m);
  return shared_->lastError;
}

} // namespace lyrics
