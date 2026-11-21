// strings and filesystem helpers
#include "muzx.hpp"

namespace muzx {

std::string lowerCase(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trim(const std::string &text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return "";
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

bool isAudioFile(const fs::path &path) {
  const std::string ext = lowerCase(path.extension().string());
  return ext == ".mp3" || ext == ".ogg" || ext == ".wav" || ext == ".flac" ||
         ext == ".aiff" || ext == ".au" || ext == ".m4a" || ext == ".opus" ||
         ext == ".aac";
}

// Turns user/config input into a clean absolute path without a trailing slash
// (a trailing slash made "is this the music root?" comparisons fail, which
// let Backspace walk above the music folder).
fs::path normalizeFolder(std::string text) {
  text = trim(text);
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') &&
      text.back() == text.front())
    text = trim(text.substr(1, text.size() - 2));
  if (text.empty())
    return {};

  if (text[0] == '~' && (text.size() == 1 || text[1] == '/')) {
    if (const char *home = std::getenv("HOME"); home && *home)
      text = std::string(home) + text.substr(1);
  }

  std::error_code ec;
  fs::path path = fs::absolute(fs::path(text), ec);
  if (ec)
    return {};
  path = path.lexically_normal();
  if (!path.has_filename() && path.has_parent_path())
    path = path.parent_path();
  return path;
}

std::vector<fs::path> directoryEntries(const fs::path &folder) {
  struct Item {
    fs::path path;
    std::string sortKey;
    bool isDir;
  };
  std::vector<Item> items;

  std::error_code ec;
  fs::directory_iterator it(folder, ec);
  fs::directory_iterator end;

  while (!ec && it != end) {
    std::error_code entryEc;
    const bool isDir = it->is_directory(entryEc);
    if (isDir || isAudioFile(it->path())) {
      fs::path full = fs::absolute(it->path(), entryEc).lexically_normal();
      std::string key = lowerCase(full.filename().string());
      items.push_back({std::move(full), std::move(key), isDir});
    }
    it.increment(ec);
  }

  // Sort keys are computed once; the old comparator hit the disk on every
  // comparison.
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
    if (a.isDir != b.isDir)
      return a.isDir;
    if (a.sortKey != b.sortKey)
      return a.sortKey < b.sortKey;
    return a.path < b.path;
  });

  std::vector<fs::path> entries;
  entries.reserve(items.size());
  for (auto &item : items)
    entries.push_back(std::move(item.path));
  return entries;
}

template <typename Iterator>
void collectAudioFiles(Iterator it, std::error_code &ec,
                       std::vector<std::pair<std::string, std::string>> &out) {
  Iterator end;
  while (!ec && it != end) {
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && isAudioFile(it->path())) {
      const fs::path absolute = fs::absolute(it->path(), entryEc).lexically_normal();
      out.emplace_back(lowerCase(absolute.filename().string()),
                       absolute.string());
    }
    it.increment(ec);
  }
}

std::vector<std::string> audioFilesIn(const fs::path &folder,
                                      bool recursive) {
  std::vector<std::pair<std::string, std::string>> keyed; // {lower name, path}
  std::error_code ec;

  if (recursive) {
    collectAudioFiles(fs::recursive_directory_iterator(
                          folder, fs::directory_options::skip_permission_denied,
                          ec),
                      ec, keyed);
  } else {
    collectAudioFiles(fs::directory_iterator(folder, ec), ec, keyed);
  }

  // Name first (as before), full path as tie-breaker so the order is stable.
  std::sort(keyed.begin(), keyed.end());

  std::vector<std::string> songs;
  songs.reserve(keyed.size());
  for (auto &entry : keyed)
    songs.push_back(std::move(entry.second));
  return songs;
}

std::vector<fs::path> searchAudioFiles(const fs::path &root,
                                       const std::string &query) {
  std::vector<fs::path> results;
  const std::string wanted = lowerCase(query);
  if (wanted.empty())
    return results;

  std::error_code ec;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  fs::recursive_directory_iterator end;

  while (!ec && it != end) {
    std::error_code entryEc;
    if (it->is_regular_file(entryEc) && isAudioFile(it->path())) {
      const std::string filename = lowerCase(it->path().filename().string());
      if (filename.find(wanted) != std::string::npos)
        results.push_back(fs::absolute(it->path(), entryEc).lexically_normal());
    }
    it.increment(ec);
  }

  std::sort(results.begin(), results.end());
  return results;
}

std::string fileName(const std::string &path) {
  if (path.empty())
    return "";
  return fs::path(path).filename().string();
}

} // namespace muzx
