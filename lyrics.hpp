// lyrics.hpp - finds, parses and caches song lyrics for MUZX.
//
// Lookup order for a song (done on a background thread, so the UI never waits):
//   1. a "<song name>.lrc" (or .txt) file next to the song
//   2. the cache in ~/.config/muzx/lyrics/
//   3. synced lyrics stored inside the file's tags (read with ffprobe)
//   4. the free LRCLIB service (https://lrclib.net, no account needed),
//      found by the song's tags or, if it has none, by its cleaned file name
//   5. plain (unsynced) lyrics from the tags
// Whatever is found online is cached, so each song is looked up only once.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lyrics {

struct Line {
  double time = 0.0; // seconds from the start of the song (0 if unsynced)
  std::string text;
};

struct Result {
  bool synced = false;     // true: every line has a real timestamp
  std::string source;      // where it came from, shown in the panel title
  std::vector<Line> lines; // sorted by time when synced
};

enum class State { Idle, Loading, Ready, NotFound };

// ---- helpers (public so they can be tested) --------------------------------
std::string sanitize(const std::string &text); // strips control codes
Result parseLrc(const std::string &text);      // "[01:23.45]words"
Result parsePlain(const std::string &text);    // lines without times
std::string toLrc(const Result &result);       // inverse of parse*
std::string cleanTitleFromFilename(const std::string &path);
std::string urlEncode(const std::string &text);
// Picks the best lyrics out of an LRCLIB reply (one object or an array).
// `synced` gets timestamped lyrics whose song length is within 5 s of
// `duration`; `plain` gets any unsynced lyrics.
// `wantedTitles` (optional): a result is only accepted when its track name
// contains one of these titles, or the other way round.
void pickFromLrclib(const std::string &json, double duration, Result &synced,
                    Result &plain,
                    const std::vector<std::string> &wantedTitles = {});

class Manager {
public:
  Manager();
  ~Manager();
  Manager(const Manager &) = delete;
  Manager &operator=(const Manager &) = delete;

  void setCacheDir(const std::filesystem::path &dir); // e.g. ~/.config/muzx
  void setOnlineEnabled(bool enabled);

  // Starts looking for this song's lyrics (no-op if it is already the current
  // song). Returns immediately; poll state()/result().
  void request(const std::string &songPath, double durationSeconds);

  State state() const;
  std::shared_ptr<const Result> result() const; // null unless state()==Ready

  // ---- background preparation ----------------------------------------------
  // Looks up lyrics for the songs in the queue, one at a time, while you
  // listen to something else. Everything is cached, so a song is done once.
  struct PrefetchStatus {
    bool enabled = false;
    std::size_t processed = 0; // songs of the queue that are finished
    std::size_t total = 0;     // songs in the queue
    std::size_t found = 0;     // finished songs that have lyrics
    std::string current;       // the song being worked on
  };
  void setPrefetch(bool enabled);
  void setPrefetchQueue(const std::vector<std::string> &songs); // in this order
  PrefetchStatus prefetchStatus() const;

  struct Shared; // internal; public only so the worker thread can name it

private:
  std::shared_ptr<Shared> shared_;
};

} // namespace lyrics
