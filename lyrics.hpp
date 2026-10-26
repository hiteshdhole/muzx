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

enum class State { Idle, Loading, Ready, NotFound, Generating };

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

// ---- auto-generated lyrics (speech recognition with Whisper) ---------------
// Reads the JSON that the `whisper` command-line tool writes
// (--output_format json) and turns its segments into timed lines.
Result parseWhisperJson(const std::string &json);
// Full path of the whisper command that will be used, or "" if none is
// installed. $MUZX_WHISPER overrides the search ("whisper",
// "whisper-ctranslate2").
std::string generatorCommand();
// Model name passed to whisper: $MUZX_WHISPER_MODEL, default "small".
std::string generatorModel();

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

  // Generates lyrics for the current song with Whisper, in a child process
  // that is killed again when you skip the song, cancel, or quit. Works when
  // no lyrics were found (or to redo lyrics that were generated before).
  // Returns false if it could not start (see lastError()).
  bool generate(const std::string &songPath, double durationSeconds);
  void cancelGenerate(); // also cancels a generation that is still waiting
  // True when generate() was called while the lyrics search was still running:
  // Whisper then starts right after that search, if it finds nothing.
  bool generatePending() const;
  double generatingSeconds() const; // time since generation started, else 0
  std::string lastError() const;    // why the last generation / start failed

  struct Shared; // internal; public only so the worker thread can name it

private:
  std::shared_ptr<Shared> shared_;
};

} // namespace lyrics
