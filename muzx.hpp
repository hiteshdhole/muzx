// Shared declarations of the MUZX player. The code lives in the .cpp files:
//   terminal.cpp      colours, keys, mouse, raw terminal, input reading
//   util.cpp          strings and filesystem helpers
//   state.cpp         application state and statistics
//   playback.cpp      playback control, shuffle, resume
//   draw.cpp          drawing the tabs
//   integration.cpp   lyric offsets, MPRIS media keys, notifications
//   settings.cpp      Settings tab and the LCD hub rows
//   lyrics_panel.cpp  lyrics panel, LCD / prefetch updates
//   screen.cpp        player buttons and the whole screen
//   input.cpp         key and mouse handling
//   main.cpp          run() and main()
#pragma once
#include "audio_player.hpp"
#include "config.hpp"
#include "cover.hpp"
#include "lcd_link.hpp"
#include "lyrics.hpp"
#include "mpris.hpp"
#include "queue.hpp"
#include "selection.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <streambuf>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;


namespace muzx {

// ---- colours, keys, mouse, raw terminal, input reading (terminal.cpp) ----

inline constexpr const char *kReset = "\033[0m";

inline constexpr const char *kSelected = "\033[7m";

// Theme colours: changed at run time by applyTheme() (Settings tab).
inline const char *kMuted = "\033[90m";

inline const char *kPurpleBold = "\033[1;35m";

inline const char *kPink = "\033[95m";

inline const char *kGreenBold = "\033[1;32m";

inline const char *kCyanBold = "\033[1;36m";

inline const char *kLyricCurrent = "\033[1;95m";

inline constexpr const char *kLyricNear = "\033[37m";

struct Theme {
  const char *name, *title, *accent, *green, *cyan, *muted, *lyric;
};

inline const Theme kThemes[] = {
    {"Purple", "\033[1;35m", "\033[95m", "\033[1;32m", "\033[1;36m", "\033[90m", "\033[1;95m"},
    {"Ocean", "\033[1;34m", "\033[96m", "\033[1;36m", "\033[1;94m", "\033[90m", "\033[1;96m"},
    {"Forest", "\033[1;32m", "\033[93m", "\033[1;92m", "\033[1;33m", "\033[90m", "\033[1;93m"},
    {"Sunset", "\033[1;31m", "\033[93m", "\033[1;33m", "\033[1;91m", "\033[90m", "\033[1;93m"},
    {"Mono", "\033[1;37m", "\033[97m", "\033[1;37m", "\033[37m", "\033[90m", "\033[1;97m"},
    {"Custom", "", "", "", "", "\033[90m", ""}};

inline constexpr int kThemeCount = 6;

inline constexpr int kCustomTheme = 5;

// Colours the theme editor can pick from (256-colour codes).
struct PaletteColor {
  const char *name;
  int code;
};

inline const PaletteColor kPalette[] = {
    {"Red", 196},  {"Orange", 208}, {"Yellow", 220}, {"Lime", 118},  {"Green", 40},
    {"Teal", 37},  {"Cyan", 51},    {"Sky", 39},     {"Blue", 33},   {"Purple", 135},
    {"Magenta", 201}, {"Pink", 213}, {"White", 255}, {"Grey", 250}};

inline constexpr int kPaletteCount = 14;

inline int g_custom[4] = {10, 11, 4, 6};

inline std::string g_customCode[5];

void applyTheme(int index);

inline constexpr int kTabCount = 7;

inline constexpr int kFrameMs = 16;

inline constexpr int kEscapeTimeoutMs = 25;

// 1-based terminal column where the visualizer starts when album art is shown
// (the cover is 20 columns wide, starting at column 3).
inline constexpr int kVisColumnWithArt = 28;

inline constexpr std::size_t kVisBars = audio_player::kBandCount;

enum Key : int {
  KeyNone = 0,
  KeyEscape = 256,
  KeyUp,
  KeyDown,
  KeyLeft,
  KeyRight,
  KeyShiftTab,
  KeyEnter,
  KeyBackspace,
  KeyMouse, // a mouse event: see lastMouse
  KeyUnknown // recognised-but-ignored input (Delete, Alt+x, F-keys, ...)
};

// Mouse reporting (SGR mode). MUZX_MOUSE=0 turns the mouse off. While it is on,
// hold Shift to select text with the mouse as usual.
struct MouseEvent {
  int button = 0, x = 0, y = 0;
  bool press = false;
};

inline MouseEvent lastMouse;

bool mouseWanted();

inline termios originalTerminal{};

inline bool rawModeEnabled = false;

inline bool inputClosed = false;

inline volatile std::sig_atomic_t quitSignal = 0;

void onSignal(int);

void installSignalHandlers();

void enableRawMode();

void disableRawMode();

class TerminalSession {
public:
  TerminalSession() {
    enableRawMode();
    std::cout << "\033[?1049h\033[?25l\033[?7l\033[2J\033[H" << std::flush;
  }
  ~TerminalSession() {
    clearTerminalCover();
    disableRawMode();
    std::cout << "\033[?7h\033[?25h\033[?1049l" << std::flush;
  }
  TerminalSession(const TerminalSession &) = delete;
  TerminalSession &operator=(const TerminalSession &) = delete;
};

// Redirects std::cout into a string for the lifetime of the object.
class CoutCapture {
public:
  CoutCapture()
      : old_(std::cout.rdbuf(buffer_.rdbuf())), previous_(current_) {
    current_ = this;
  }
  ~CoutCapture() {
    current_ = previous_;
    std::cout.rdbuf(old_);
  }
  // Number of finished lines captured so far (the newest capture).
  static int rowsSoFar() {
    if (!current_)
      return 0;
    const std::string text = current_->buffer_.str();
    return static_cast<int>(std::count(text.begin(), text.end(), '\n'));
  }
  CoutCapture(const CoutCapture &) = delete;
  CoutCapture &operator=(const CoutCapture &) = delete;
  std::string str() const { return buffer_.str(); }

private:
  static inline CoutCapture *current_ = nullptr;
  std::ostringstream buffer_;
  std::streambuf *old_;
  CoutCapture *previous_;
};

// Clickable areas. The drawing code registers them while it draws; a mouse
// click is then matched against this list (rows and columns are 1-based).
enum ClickAction {
  ClkTab, ClkShuffle, ClkPrev, ClkPlayPause, ClkNext, ClkRepeat,
  ClkSeek, ClkListItem, ClkSetting, ClkVolume
};

struct ClickRegion {
  int row0, row1, col0, col1, action, arg;
};

inline std::vector<ClickRegion> g_clicks;

// Dragging the progress bar: remembers where the last seek went to, so every
// mouse movement only has to move by the difference.
struct DragState {
  bool active = false;
  double target = 0.0; // seconds
  double duration = 0.0;
  int col0 = 0, col1 = 0;
};

inline DragState g_drag;

inline int g_buttonsRow = 0;

void addClick(int row0, int row1, int col0, int col1, int action, int arg = 0);

int terminalRows();

bool waitForInput(int timeoutMs);

bool readByte(char &value, int timeoutMs);

int arrowKey(char finalByte);

// Consumes an OSC / APC / DCS string up to its terminator (BEL or ESC \).
void skipStringSequence();

int readKey();

// ---- strings and filesystem helpers (util.cpp) ----

std::string lowerCase(std::string value);

std::string trim(const std::string &text);

bool isAudioFile(const fs::path &path);

// Turns user/config input into a clean absolute path without a trailing slash
// (a trailing slash made "is this the music root?" comparisons fail, which
// let Backspace walk above the music folder).
fs::path normalizeFolder(std::string text);

std::vector<fs::path> directoryEntries(const fs::path &folder);

std::vector<std::string> audioFilesIn(const fs::path &folder,
                                      bool recursive = false);

std::vector<fs::path> searchAudioFiles(const fs::path &root,
                                       const std::string &query);

std::string fileName(const std::string &path);

// ---- application state and statistics (state.cpp) ----

enum class LyricsMode { Off, Right, Bottom };

enum class RepeatMode { Off, All, One };

enum class AppTab {
  Player = 0,
  Folders = 1,
  Queue = 2,
  Search = 3,
  Status = 4,
  Keybindings = 5,
  Settings = 6
};

// Shuffle plays every song once in random order, then reshuffles. The history
// lets Previous / Next walk through what was really played.
struct ShuffleState {
  std::vector<std::size_t> history; // playlist indices, oldest first
  std::size_t pos = 0;              // where we are in the history
  std::vector<std::size_t> bag;     // songs not played yet in this round
};

struct AppState {
  audio_player player;
  queue songQueue;
  AppTab tab = AppTab::Player;
  fs::path musicRoot;
  fs::path browseFolder;
  fs::path statsFile;
  std::vector<fs::path> folderEntries;
  std::vector<fs::path> searchResults;
  selection folderCursor;
  selection queueCursor;
  selection searchCursor;
  std::string searchQuery;
  std::string currentSong;
  std::vector<std::string> playlist;
  std::size_t playlistIndex = 0;
  bool hasCurrentSong = false;
  bool playbackRunning = false;
  bool paused = false;
  bool shuffle = false;
  RepeatMode repeat = RepeatMode::Off;
  bool compact = false;      // Settings: hide the hint lines
  bool showButtons = true;   // Settings: the clickable control buttons
  bool notify = false;       // Settings: desktop notification on song change
  bool showAlbumArt = false;
  bool showVisualizer = false;
  LyricsMode lyricsMode = LyricsMode::Right; // "L": Off -> Right -> Bottom
  std::chrono::steady_clock::time_point lyricsSongSince{};
  lyrics::Manager lyricsMgr; // finds lyrics on a background thread
  Mpris mpris;               // desktop media keys / playerctl (D-Bus)
  LcdLink lcd;               // "T": show the current lyric line on the Arduino LCD
  // Settings tab
  selection settingsCursor;
  int theme = 0;
  int fontSize = 0;          // 0 = leave the terminal's font alone
  fs::path settingsFile;
  std::string settingsSaved; // what is on disk now
  std::string settingsNote;  // message shown at the bottom of the tab
  config *cfg = nullptr;
  std::string lyricsSong;    // song the lyrics were last requested for
  // Lyrics timing: seconds the lyrics are shifted LATER (negative = earlier).
  double lyricShift = 0.0;
  std::string lyricShiftSong;                 // song lyricShift belongs to
  std::map<std::string, double> lyricShifts;  // saved per song
  fs::path lyricShiftFile;
  double volumeBeforeMute = 1.0;              // 0..1, for click-to-mute
  // Start with the song that was playing last time (Settings: "On start")
  int resumeMode = 2;                         // 0 off, 1 last song paused, 2 last song playing
  fs::path lastFile;                          // ~/.config/muzx/last.txt
  std::string lastSavedSong;                  // song last.txt was written for
  // Background lyrics preparation (Settings: "Prepare lyrics")
  int prefetchMode = 0;                       // 0 off, 1 look up lyrics of other songs in the background
  std::vector<std::string> library;           // every song under musicRoot
  fs::path libraryRoot;
  std::string prefetchKey;                    // when this changes, the queue is rebuilt

  double sessionPlayedSeconds = 0.0;
  double totalPlayedSeconds = 0.0;
  bool statsDirty = false;
  std::map<std::string, int> playCounts;
  std::array<float, kVisBars> visHeights{};
  std::array<float, kVisBars> visPeaks{};

  std::mt19937 rng{std::random_device{}()};
  ShuffleState shuffleState;
};

void loadStats(AppState &app);

// Writes to a temp file and renames it over the real one, so a crash or full
// disk can no longer leave a truncated stats file behind.
void saveStats(AppState &app);

std::string formatDurationHMS(double totalSeconds);

std::string currentPcTimeString();

void updateVisualizerAndStats(AppState &app, double dtSeconds);

// ---- playback control, shuffle, resume (playback.cpp) ----

std::size_t folderMenuSize(const AppState &app);

void shuffleReset(AppState &app, std::size_t current);

// Keeps the shuffle state in step with the song that is really playing (a
// broken file is skipped, so the index can differ from what was asked for).
void shuffleSync(AppState &app, bool newList);

void shuffleEnsure(AppState &app);

std::size_t shuffleNextIndex(AppState &app);

std::size_t shufflePreviousIndex(AppState &app);

void setShuffle(AppState &app, bool on);

bool playFromPlaylist(AppState &app, const std::vector<std::string> &songs,
                      std::size_t index, bool switchToPlayer = true,
                      bool countPlay = true);

bool playSongFromFolder(AppState &app, const fs::path &path);

bool playFolder(AppState &app, const fs::path &folder, bool recursive = true);

std::size_t pickNextIndex(AppState &app);

void nextSong(AppState &app);

void previousSong(AppState &app);

void checkAutomaticNext(AppState &app);

// Start with the last played song: ~/.config/muzx/last.txt holds its path and
// the position (two lines). Written when the song changes, every few seconds
// and on quit.
void saveLastPlayed(AppState &app);

// Returns true when the last song was loaded.
bool resumeLastPlayed(AppState &app);

void togglePause(AppState &app);

void seekLeft(AppState &app);

void seekRight(AppState &app);

std::vector<std::string> queueAsList(const queue &songQueue);

void addToQueue(AppState &app, const fs::path &path);

void openFolder(AppState &app, const fs::path &folder);

void askSearchQuery(AppState &app);

// ---- drawing the tabs (draw.cpp) ----

std::size_t visualLength(const std::string &text);

void showTabs(AppTab active);

void drawList(const std::vector<std::string> &labels,
              std::size_t selectedIndex);

const char *subBlockGlyph(int eighths);

// The cover code prints a text placeholder such as
// "[No embedded or folder cover]" when a song has no picture. It is wider than
// the space left of the visualizer, so the bars cut it off. Show nothing
// instead: any placeholder line is replaced by an empty line.
// (Real cover rows never contain these words, so they are left untouched.)
std::string fitCoverLine(const std::string &line);

void drawPlayerCoverAndVisualizer(const AppState &app);

void drawPlayer(const AppState &app);

void drawFolders(const AppState &app);

void drawQueue(const AppState &app);

void drawSearch(const AppState &app);

void drawStatus(const AppState &app);

void drawKeybindings();

// ---- lyric offsets, MPRIS media keys, notifications (integration.cpp) ----

// "SpotiDownloader.com - Title - Artist.mp3" -> title and artist.
void splitSongName(const std::string &path, std::string &title, std::string &artist);

// Lyrics timing offset, saved per song: ~/.config/muzx/lyrics-offsets.txt
// (one line per song:  <path> TAB <seconds>)
void loadLyricShifts(AppState &app);

void saveLyricShifts(const AppState &app);

// Picks up the saved shift when the song changes.
void syncLyricShift(AppState &app);

// delta = 0 resets. Keys: [ earlier, ] later, \ reset.
void adjustLyricShift(AppState &app, double delta);

// The player only has "louder" and "quieter" steps, so walk towards the target.
void setPlayerVolume(AppState &app, double target01);

// Tells the desktop what is playing and does what it asked. Returns true when
// the desktop asked MUZX to quit.
bool serviceMpris(AppState &app);

bool commandExists(const std::string &name);

void sendNotification(const std::string &title, const std::string &body);

void notifySongChange(AppState &app);

// ---- Settings tab and the LCD hub rows (settings.cpp) ----

enum SettingItem {
  SetRepeat = 0, SetResume,                       // playback
  SetLyrics, SetPrefetch,                         // lyrics
  SetLcd, SetLcdKana,                             // LCD
  SetHubMode, SetHubVolume, SetHubNotify,         // LCD hub
  SetHubLyrics, SetHubCaptions, SetHubNowPlaying, SetHubSpectrum, SetHubClock, SetHubStatus,
  SetArt, SetVisualizer, SetLayout, SetButtons, SetNotify, // display
  SetTheme, SetColTitle, SetColAccent, SetColHighlight, SetColBars, SetFont, // look
  SetFolder,                                      // general
  kSettingCount
};

// What the LCD hub (muzxlcd) says about itself. It keeps these settings in its
// own file (lcd.conf); MUZX only asks and sends commands.
struct HubInfo {
  bool running = false;
  std::string mode;
  std::vector<std::string> modes;
  bool volume = true, notify = true;
  std::chrono::steady_clock::time_point asked{};
};

inline HubInfo g_hub;

inline const char *const kHubModeNames[] = {"lyrics", "captions", "nowplaying", "spectrum", "clock", "status"};

std::vector<std::string> splitCommas(const std::string &s);

// Asks the hub for its state (at most every 2 seconds unless forced).
void refreshHub(const AppState &app, bool force = false);

inline const int kFontChoices[] = {0, 10, 11, 12, 13, 14, 16, 18, 20, 22, 24, 28};

int cycleIndex(int current, int dir, int count);

// Font size can only be changed by Kitty itself, through its remote control.
// Returns an empty string on success, otherwise a message for the user.
std::string applyFontSize(int size);

std::string settingsToText(const AppState &app);

// Writes the settings file, but only when something changed.
void saveSettings(AppState &app);

void loadSettings(AppState &app);

// Changes one setting by one step forward (+1) or backward (-1).
void changeSetting(AppState &app, int item, int dir);

// Asks for a new music folder (same style as the search prompt).
void askMusicFolder(AppState &app);

std::string settingValue(const AppState &app, int item);

void drawSettings(const AppState &app);

// ---- lyrics panel, LCD / prefetch updates (lyrics_panel.cpp) ----

inline constexpr int kLyricsMinCols = 101;

inline constexpr double kLyricsLeadSeconds = 0.25;

// Where we are in the song, as far as the lyrics are concerned.
double lyricsPosition(const AppState &app);

int terminalCols();

// Reads one UTF-8 character at text[i]; returns how many bytes it uses.
std::size_t decodeUtf8(const std::string &text, std::size_t i, char32_t &cp);

// Terminal columns a character occupies: Japanese/Chinese/Korean and emoji
// take 2, combining marks take 0, everything else 1.
int charColumns(char32_t cp);

// Splits a lyric line into rows that fit `width` columns. Breaks at spaces
// when possible; text without spaces (Japanese) is broken between characters.
std::vector<std::string> wrapLyric(const std::string &text, std::size_t width);

// Terminal columns taken by a whole string (see charColumns above).
std::size_t displayWidth(const std::string &text);

// Cuts a string to at most `maxCols` terminal columns (never inside a
// character).
std::string clipColumns(const std::string &text, std::size_t maxCols);

// Asks the lyrics thread for the lyrics of the song that is playing now.
// Called every frame; it only does work when the song changes.
void updateLyricsRequest(AppState &app);

// Tells the lyrics module which songs to prepare in the background: the rest
// of the playlist first (in play order), then everything else in the library.
void updatePrefetchQueue(AppState &app);

// Sends the line being sung (or the song name) to the Arduino LCD.
void updateLcd(AppState &app);

// Where the lyrics are drawn.
struct LyricsBox {
  int col = 1;       // left column (1-based)
  int titleRow = 1;  // row of the "LYRICS" title
  int top = 1;       // first lyric row
  int height = 0;    // number of lyric rows
  std::size_t width = 0;
  bool centered = false; // bottom placement centres every line
};

// Works out the box for the current placement. Returns false when it does not
// fit on the screen.
//   Right  = the empty area to the right of the player
//   Bottom = the empty area below the player (needs a few free rows)
bool lyricsBox(const AppState &app, int rows, int cols, int usedRows,
               LyricsBox &box);

// Builds the escape codes that draw the lyrics. Returns "" when they should
// not be shown (other tab, switched off, nothing playing, no room).
std::string lyricsPanel(const AppState &app, int rows, int cols, int usedRows);

// ---- player buttons and the whole screen (screen.cpp) ----

int rightPanelColumn(const AppState &app, int cols);

std::string playerButtons(const AppState &app, int rows, int cols);

void drawScreen(const AppState &app);

// ---- key and mouse handling (input.cpp) ----

enum class KeyResult { Handled, FlushInput, Quit };

KeyResult handleMouse(AppState &app, const fs::path &queueFile);

KeyResult handleKey(AppState &app, int key, const fs::path &queueFile);

// ---- run() and main() (app.cpp) ----

int run();

} // namespace muzx
