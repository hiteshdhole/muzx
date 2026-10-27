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
      if (d >= 0x80 && d <= 0x9F) {
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

static bool looksLikeDomain(const std::string &word) {
  const std::string w = lowerCopy(word);
  const auto dot = w.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 >= w.size())
    return false;
  static const char *const kTlds[] = {
      "com", "org", "net", "io", "in",  "co",   "app",  "me",
      "fm",  "to",  "cc",  "tv", "xyz", "info", "site", "online",
      "pk",  "se",  "ru",  "uk", "de",  "fr",   "es",   "it"};
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

static bool isJunkPhrase(const std::string &s) {
  static const char *const kJunk[] = {
      "official",     "video",        "audio",       "lyrics",
      "lyric",        "visualizer",   "remaster",    "remastered",
      "slowed",       "reverb",       "sped up",     "nightcore",
      "320kbps",      "128kbps",      "kbps",        "hq",
      "hd",           "4k",           "full song",   "full video",
      "from ",        "ost",          "soundtrack",  "original",
      "youtube",      "spotify",      "spotdown",    "pagalworld",
      "djpunjab",     "mr-jatt",      "mp3",         "flac",
      "m4a",          "wav",          "unplugged",   "lofi",
      "lo-fi",        "chill",        "bass boosted"};
  const std::string low = lowerCopy(s);
  for (const char *j : kJunk) {
    if (low.find(j) != std::string::npos)
      return true;
  }
  return false;
}

static std::string cleanQueryText(const std::string &raw) {
  std::string s = raw;
  static const char *const kWatermarks[] = {
      "_spotdown.org", "spotdown.org", "SpotiDownloader.com",
      "spotidownloader.com", "PagalWorld", "DJMaza", "SongsPK",
      "y2mate.com", "ytmp3", "savefrom.net"};
  for (const char *wm : kWatermarks)
    eraseAllCI(s, wm);

  // Drop bracketed segments (...) [...] {...}
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

  // Strip "feat.", "ft.", "featuring" and everything after if at the end of a clause
  {
    const std::string low = lowerCopy(noBrackets);
    for (const char *ft : {" feat.", " feat ", " ft.", " ft ", " featuring "}) {
      const auto pos = low.find(ft);
      if (pos != std::string::npos) {
        const auto dashAfter = noBrackets.find(" - ", pos);
        if (dashAfter == std::string::npos) {
          noBrackets.erase(pos);
        } else {
          noBrackets.erase(pos, dashAfter - pos);
        }
        break;
      }
    }
  }

  // Strip common junk phrases outside brackets
  for (const char *junk : {"official video", "official audio", "official music video",
                           "lyric video", "lyrics video", "full video", "full audio",
                           "320kbps", "128kbps", "320 kbps", "128 kbps"}) {
    eraseAllCI(noBrackets, junk);
  }

  // Strip leading track numbers like "01. ", "01 - ", "1 - "
  std::string trimmed = trimCopy(noBrackets);
  {
    std::size_t idx = 0;
    while (idx < trimmed.size() && idx < 4 &&
           std::isdigit(static_cast<unsigned char>(trimmed[idx]))) {
      ++idx;
    }
    if (idx > 0 && idx < trimmed.size()) {
      while (idx < trimmed.size() &&
             (trimmed[idx] == '.' || trimmed[idx] == '-' || trimmed[idx] == ' ')) {
        ++idx;
      }
      if (idx < trimmed.size())
        trimmed.erase(0, idx);
    }
  }

  // Collapse runs of spaces & drop domain words
  std::istringstream words(trimmed);
  std::string word, kept;
  while (words >> word) {
    if (looksLikeDomain(word))
      continue;
    if (!kept.empty())
      kept += ' ';
    kept += word;
  }

  while (startsWith(kept, "- "))
    kept.erase(0, 2);
  while (kept.size() >= 2 && kept.compare(kept.size() - 2, 2, " -") == 0)
    kept.erase(kept.size() - 2);
  if (kept == "-")
    kept.clear();
  return trimCopy(kept);
}

static std::string primaryArtist(const std::string &artist) {
  std::string a = cleanQueryText(artist);
  // Split on comma, semicolon, slash, or "&" to extract primary artist as fallback
  for (const char *sep : {", ", " & ", " / ", "; ", " x ", " X "}) {
    const auto pos = a.find(sep);
    if (pos != std::string::npos && pos > 0) {
      return trimCopy(a.substr(0, pos));
    }
  }
  return a;
}

std::string cleanTitleFromFilename(const std::string &path) {
  const std::string stem = fs::path(path).stem().string();
  const std::string out = cleanQueryText(stem);
  return out.empty() ? stem : out;
}

// ===========================================================================
// LRC / plain text parsing + Synthetic Sync
// ===========================================================================
namespace {

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
  std::replace(rest.begin(), rest.end(), ':', '.');
  const double secs = std::strtod(rest.c_str(), nullptr);
  seconds = minutes * 60.0 + secs;
  return true;
}

std::string stripInlineTags(const std::string &text) {
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

// Converts plain unsynced lyrics into estimated synced timestamps across the song
void synthesizeSync(Result &r, double duration) {
  if (r.synced || r.lines.empty())
    return;

  // Filter out empty spacer lines so LCD and UI stay active
  std::vector<Line> active;
  active.reserve(r.lines.size());
  for (const auto &l : r.lines) {
    if (!trimCopy(l.text).empty())
      active.push_back(l);
  }
  if (active.empty())
    return;

  const double totalDur = (duration > 15.0) ? duration : 180.0;
  const double startOffset = std::min(5.0, totalDur * 0.04);
  const double endMargin = std::min(8.0, totalDur * 0.06);
  const double usable = std::max(10.0, totalDur - startOffset - endMargin);
  const double step = usable / static_cast<double>(active.size());

  for (std::size_t i = 0; i < active.size(); ++i) {
    active[i].time = startOffset + i * step;
  }
  r.lines = std::move(active);
  r.synced = true;
  if (r.source.find("auto-sync") == std::string::npos) {
    r.source += " (auto-sync)";
  }
}

} // namespace

Result parseLrc(const std::string &input) {
  Result result;
  std::string text = input;
  if (startsWith(text, "\xEF\xBB\xBF"))
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
        if (stamps.empty()) {
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

  bool lastBlank = true;
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
  std::map<std::string, double> num;
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
    } else if (!skipJsonValue(p, e)) {
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
      break;
  }
  return objects;
}

} // namespace

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

static bool hasWordOverlap(const std::string &a, const std::string &b) {
  const std::string ka = matchKey(a);
  const std::string kb = matchKey(b);
  if (ka.empty() || kb.empty())
    return false;
  if (ka.find(kb) != std::string::npos || kb.find(ka) != std::string::npos)
    return true;

  // Check if any significant word (>= 4 chars) matches
  std::istringstream iss(lowerCopy(a));
  std::string w;
  while (iss >> w) {
    std::string kw = matchKey(w);
    if (kw.size() >= 4 && kb.find(kw) != std::string::npos)
      return true;
  }
  return false;
}

void pickFromLrclib(const std::string &json, double duration, Result &synced,
                    Result &plain,
                    const std::vector<std::string> &wantedTitles) {
  constexpr double kStrictTolerance = 12.0; // seconds for primary match
  constexpr double kLooseTolerance = 45.0;  // fallback for video edits/extended
  synced = Result{};
  plain = Result{};

  std::vector<std::string> validWanted;
  for (const std::string &title : wantedTitles) {
    if (matchKey(title).size() >= 3)
      validWanted.push_back(title);
  }

  const auto objects = parseJsonObjects(json);

  // Pass 1: Strict title overlap + strict duration tolerance
  // Pass 2: Any result from query + loose duration tolerance
  for (int pass = 0; pass < 2; ++pass) {
    double bestDiff = 1e18;
    const double tol = (pass == 0) ? kStrictTolerance : kLooseTolerance;

    for (const JsonObject &obj : objects) {
      if (pass == 0 && !validWanted.empty()) {
        const auto itName = obj.str.find("trackName");
        const auto itArt = obj.str.find("artistName");
        const std::string combined =
            (itName != obj.str.end() ? itName->second : "") + " " +
            (itArt != obj.str.end() ? itArt->second : "");
        bool related = false;
        for (const std::string &wanted : validWanted) {
          if (hasWordOverlap( combined, wanted)) {
            related = true;
            break;
          }
        }
        if (!related)
          continue;
      }

      const auto itSynced = obj.str.find("syncedLyrics");
      const auto itPlain = obj.str.find("plainLyrics");
      const auto itDur = obj.num.find("duration");
      const double objDuration = itDur != obj.num.end() ? itDur->second : 0.0;

      if (itSynced != obj.str.end() && !trimCopy(itSynced->second).empty()) {
        const double diff = (duration > 0.0 && objDuration > 0.0)
                                ? std::fabs(objDuration - duration)
                                : 0.0;
        if (diff <= tol && diff < bestDiff) {
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
    if (!synced.lines.empty())
      break;
  }
}

// ===========================================================================
// Finding lyrics for one song (runs on the worker thread)
// ===========================================================================
namespace {

struct Tags {
  std::string title, artist, album, lyrics;
};

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
      else if (key == "artist" || key == "album_artist")
        target = &tags.artist;
      else if (key == "album")
        target = &tags.album;
      else if (startsWith(key, "lyrics") || startsWith(key, "unsyncedlyrics") ||
               key == "uslt" || key == "sylt")
        target = &tags.lyrics;
      if (target && target->empty())
        *target = value;
      else
        target = nullptr;
    } else if (target) {
      *target += "\n" + line;
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

thread_local const std::function<bool()> *tlsStale = nullptr;

std::string fetchUrl(const std::string &url, bool &transient) {
  if (tlsStale && (*tlsStale)()) {
    transient = true;
    return "";
  }
  const CommandOutput out = runCommand("curl -s -L -f -m 8 -A 'muzx/1.0' -- " +
                                       shellQuote(url) + " 2>/dev/null");
  if (out.status == 0)
    return out.text;
  if (out.status != 22)
    transient = true;
  return "";
}

std::string findExecutable(const std::string &name);

std::vector<std::string> splitDashes(const std::string &text) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const auto pos = text.find(" - ", start);
    const std::string part = trimCopy(text.substr(
        start, pos == std::string::npos ? std::string::npos : pos - start));
    if (!part.empty() && !isJunkPhrase(part))
      parts.push_back(part);
    if (pos == std::string::npos)
      break;
    start = pos + 3;
  }
  return parts;
}

bool lookupOnline(const Tags &rawTags, const std::string &fallbackTitle,
                  double duration, Result &out, bool &transient) {
  const std::string base = lrclibBase();

  const std::string cleanTagTitle = cleanQueryText(rawTags.title);
  const std::string cleanTagArtist = cleanQueryText(rawTags.artist);
  const std::string mainArtist = primaryArtist(rawTags.artist);
  const std::string title =
      cleanTagTitle.empty() ? fallbackTitle : cleanTagTitle;

  const std::vector<std::string> parts = splitDashes(title);
  const std::vector<std::string> fileParts = splitDashes(fallbackTitle);

  std::string flat;
  for (const std::string &p : parts)
    flat += (flat.empty() ? "" : " ") + p;
  if (flat.empty())
    flat = title;

  std::vector<std::string> wanted = {flat, fallbackTitle};
  for (const std::string &p : parts)
    wanted.push_back(p);
  for (const std::string &p : fileParts)
    wanted.push_back(p);

  Result plainFallback;
  bool havePlain = false;

  auto consider = [&](const std::string &body, bool filtered = true) -> bool {
    if (body.empty())
      return false;
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

  auto giveUp = [&]() -> bool {
    if (!havePlain)
      return false;
    out = std::move(plainFallback);
    return true;
  };

  // 1) Exact match by cleaned tags
  if (!title.empty() && !cleanTagArtist.empty()) {
    std::string url = base + "/api/get?track_name=" + urlEncode(title) +
                      "&artist_name=" + urlEncode(cleanTagArtist);
    if (duration > 0.0)
      url += "&duration=" +
             std::to_string(static_cast<long>(std::lround(duration)));
    if (consider(fetchUrl(url, transient), false))
      return true;
    if (transient)
      return giveUp();
  }

  // 2) Search by cleaned title + primary artist
  if (!mainArtist.empty()) {
    const std::string songOnly = parts.empty() ? title : parts[0];
    if (consider(fetchUrl(base + "/api/search?track_name=" +
                              urlEncode(songOnly) + "&artist_name=" +
                              urlEncode(mainArtist),
                          transient)))
      return true;
    if (transient)
      return giveUp();
  }

  // 3) If filename has "Artist - Title" or "Title - Artist", try explicit search
  if (fileParts.size() >= 2) {
    if (consider(fetchUrl(base + "/api/search?track_name=" +
                              urlEncode(fileParts[0]) + "&artist_name=" +
                              urlEncode(fileParts[1]),
                          transient)))
      return true;
    if (consider(fetchUrl(base + "/api/search?track_name=" +
                              urlEncode(fileParts[1]) + "&artist_name=" +
                              urlEncode(fileParts[0]),
                          transient)))
      return true;
  }

  // 4) Free-text queries (most flexible on LRCLIB)
  std::vector<std::string> queries;
  auto addQuery = [&](const std::string &q) {
    const std::string t = trimCopy(q);
    if (t.size() >= 2 &&
        std::find(queries.begin(), queries.end(), t) == queries.end())
      queries.push_back(t);
  };

  if (!mainArtist.empty())
    addQuery(mainArtist + " " + (parts.empty() ? flat : parts[0]));
  addQuery(flat);
  if (fallbackTitle != flat)
    addQuery(fallbackTitle);
  for (const std::string &p : parts)
    addQuery(p);
  for (const std::string &p : fileParts)
    addQuery(p);

  if (queries.size() > 5)
    queries.resize(5);

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

std::optional<Result> loadSidecar(const std::string &songPath,
                                  double duration) {
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
      synthesizeSync(r, duration);
      return r;
    }
  }
  return std::nullopt;
}

std::optional<Result> loadCache(const fs::path &cacheDir,
                                const std::string &key, double duration) {
  for (const char *ext : {".lrc", ".gen.lrc", ".txt"}) {
    const std::string e = ext;
    const fs::path p = cacheDir / "lyrics" / (key + e);
    const auto data = readFile(p);
    if (!data)
      continue;
    Result r = e == ".txt" ? parsePlain(*data) : parseLrc(*data);
    if (!r.lines.empty()) {
      r.source = e == ".gen.lrc" ? "generated" : "cache";
      synthesizeSync(r, duration);
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

  const std::string cleanTitle = cleanQueryText(tags.title);
  const std::string title = cleanTitle.empty() ? fallbackTitle : cleanTitle;
  std::string term;
  for (const std::string &part : splitDashes(title))
    term += (term.empty() ? "" : " ") + part;
  if (term.empty())
    term = title;
  term = trimCopy(primaryArtist(tags.artist) + " " + term);
  if (term.empty())
    return false;

  std::error_code ec;
  const fs::path base =
      cacheDir.empty() ? fs::temp_directory_path(ec) : cacheDir / "lyrics";
  const fs::path workDir =
      base / ("sl-" + std::to_string(getpid()) + "-" + hexHash(term));
  fs::create_directories(workDir, ec);
  const fs::path outPath = workDir / "out.lrc";

  const CommandOutput run = runCommand(
      "timeout 20 " + shellQuote(bin) + " -o " + shellQuote(outPath.string()) +
      " -- " + shellQuote(term) + " </dev/null 2>/dev/null");
  bool found = false;
  if (run.status != 0) {
    transient = true;
  } else if (const auto data = readFile(outPath)) {
    Result r = parseLrc(*data);
    if (r.lines.empty())
      r = parsePlain(*data);
    if (!r.lines.empty()) {
      r.source = "syncedlyrics";
      synthesizeSync(r, duration);
      out = std::move(r);
      found = true;
    }
  }
  fs::remove_all(workDir, ec);
  return found;
}

std::optional<Result> findLyrics(const std::string &songPath, double duration,
                                 const fs::path &cacheDir, bool online) {
  if (auto r = loadSidecar(songPath, duration))
    return r;

  const std::string key = hexHash(songPath);
  if (!cacheDir.empty()) {
    if (auto r = loadCache(cacheDir, key, duration))
      return r;
  }

  const Tags tags = readTags(songPath);

  Result embedded;
  if (!trimCopy(tags.lyrics).empty()) {
    embedded = parseLrc(tags.lyrics);
    if (embedded.lines.empty())
      embedded = parsePlain(tags.lyrics);
    embedded.source = "embedded";
    if (embedded.synced)
      return embedded;
  }

  if (online) {
    Result found;
    bool transient = false;
    const std::string cleaned = cleanTitleFromFilename(songPath);
    bool have = lookupOnline(tags, cleaned, duration, found, transient);
    if ((!have || !found.synced) && !transient) {
      Result better;
      if (lookupSyncedlyrics(tags, cleaned, duration, cacheDir, better,
                             transient)) {
        found = std::move(better);
        have = true;
      }
    }
    if (have) {
      synthesizeSync(found, duration);
      saveCache(cacheDir, key, found);
      return found;
    }
  }

  if (!embedded.lines.empty()) {
    synthesizeSync(embedded, duration);
    saveCache(cacheDir, key, embedded);
    return embedded;
  }
  return std::nullopt;
}

} // namespace

// ===========================================================================
// Auto-generated lyrics: run Whisper (speech recognition) on the song
// ===========================================================================

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
    if (text == prev1 && text == prev2)
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
  if (fs::path(bin).filename() == "whisper") {
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
  if (!hasThreads)
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
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
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

  setpgid(pid, pid);
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
      break;
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
  if (killedBecause.empty() && cancelled())
    killedBecause = "cancelled";

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
  std::uint64_t generation = 0;
  bool hasPending = false;
  bool stop = false;
  bool workerStarted = false;
  Request pending;
  std::string currentSong;
  State state = State::Idle;
  std::shared_ptr<const Result> result;
  fs::path cacheDir;
  bool online = true;

  bool generating = false;
  bool pendingGenerate = false;
  std::string genModel, genLanguage;
  std::uint64_t genToken = 0;
  pid_t childPid = 0;
  std::chrono::steady_clock::time_point genStart;
  std::string lastError;
};

namespace {

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
  if (sh->genToken != token)
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

    if (gen == sh->generation) {
      if (found) {
        sh->result = std::make_shared<const Result>(std::move(*found));
        sh->state = State::Ready;
      } else {
        sh->result.reset();
        sh->state = State::NotFound;
        if (sh->pendingGenerate)
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
    cancelGenerationLocked(*shared_);
  }
  shared_->cv.notify_all();
}

void Manager::setCacheDir(const fs::path &dir) {
  {
    std::lock_guard<std::mutex> lock(shared_->m);
    shared_->cacheDir = dir;
  }
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
  shared_->result.reset();
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
  const std::string bin = generatorCommand();
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

  if (s.state == State::Loading) {
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
