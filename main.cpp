// ---------- Includes ----------
#include "audio_player.hpp" // our audio player class (play, pause, volume...)
#include "config.hpp"       // saves/loads settings (music folder path)
#include "library.hpp"      // list of music folders
#include "selection.hpp"    // cursor that moves up/down in a list
#include <algorithm>        // std::sort, std::transform
#include <cctype>           // std::tolower
#include <chrono>           // time measuring
#include <filesystem>       // read folders and files
#include <iostream>         // print to the screen
#include <sstream>          // parse text (mouse data)
#include <string>
#include <sys/select.h> // select() - check if input is ready
#include <termios.h>    // terminal settings (raw mode)
#include <unistd.h>     // read(), usleep()
#include <vector>

namespace fs = std::filesystem; // short name for std::filesystem

// Saved terminal settings, so we can restore them when the app exits.
termios originalTerminal{};
bool rawModeEnabled = false;

// Read ONE character from the keyboard. Returns '\0' if nothing was read.
char getkey() {
  char key = '\0';

  if (read(STDIN_FILENO, &key, 1) != 1)
    return '\0';

  return key;
}

// Check if a key is waiting to be read (does NOT wait).
bool keyAvailable() {
  timeval timeout{}; // timeout 0 = return immediately
  fd_set readfds;

  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  return select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) > 0;
}

// Holds what the user did: a key press OR a mouse click.
struct InputEvent {
  char key = '\0';         // key that was pressed
  bool mouseClick = false; // true if the mouse was clicked
  int mouseX = 0;          // click column
  int mouseY = 0;          // click row
};

// Wait up to 30 ms for one byte. Returns false if nothing arrives.
bool readByteTimeout(char &value) {
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(STDIN_FILENO, &readfds);

  timeval timeout{};
  timeout.tv_usec = 30000; // 30,000 microseconds = 30 ms

  if (select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout) <= 0)
    return false;

  return read(STDIN_FILENO, &value, 1) == 1;
}

// Read a key press or a mouse click.
// Mouse clicks arrive as an escape code like: ESC [ < button ; x ; y M
InputEvent readInputEvent() {
  InputEvent event;
  char first;

  if (read(STDIN_FILENO, &first, 1) != 1)
    return event;

  // Normal key (not an escape code) -> just return it.
  if (first != '\033') {
    event.key = first;
    return event;
  }

  char second, third;

  // Not "ESC [" -> treat it as a plain Escape key.
  if (!readByteTimeout(second) || second != '[') {
    event.key = '\033';
    return event;
  }

  // Not "ESC [ <" -> not a mouse event (e.g. arrow keys), treat as Escape.
  if (!readByteTimeout(third) || third != '<') {
    event.key = '\033';
    return event;
  }

  // Collect the rest of the mouse text until 'M' (press) or 'm' (release).
  std::string data;
  char c = '\0';
  char ending = '\0';

  while (readByteTimeout(c)) {
    if (c == 'M' || c == 'm') {
      ending = c;
      break;
    }

    data += c;
  }

  // Parse "button;x;y" from the text.
  int button, x, y;
  char separator1, separator2;

  std::istringstream input(data);

  // 'M' = press, button & 3 == 0 = left button. Accept only left clicks.
  if (ending == 'M' &&
      (input >> button >> separator1 >> x >> separator2 >> y) &&
      separator1 == ';' && separator2 == ';' && (button & 3) == 0) {
    event.mouseClick = true;
    event.mouseX = x;
    event.mouseY = y;
  }

  return event;
}

// Raw mode: keys arrive instantly (no Enter needed) and aren't shown on screen.
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1)
    return; // couldn't read terminal settings

  termios terminal = originalTerminal;

  terminal.c_lflag &= ~(ICANON | ECHO); // no line buffering, no echo
  terminal.c_cc[VMIN] = 0;              // read() may return with 0 bytes
  terminal.c_cc[VTIME] = 1;             // ...after 0.1 s

  if (tcsetattr(STDIN_FILENO, TCSANOW, &terminal) == 0) {
    rawModeEnabled = true;
    // Turn on mouse reporting.
    std::cout << "\033[?1000h\033[?1006h" << std::flush;
  }
}

// Undo raw mode: turn off the mouse and restore the old terminal settings.
void disableRawMode() {
  if (rawModeEnabled) {
    std::cout << "\033[?1000l\033[?1006l" << std::flush;
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTerminal);
    rawModeEnabled = false;
  }
}

// Returns true if the file ends with ".mp3" (any letter case).
bool isMp3(const fs::path &path) {
  std::string extension = path.extension().string();

  // Make the extension lowercase so ".MP3" also works.
  std::transform(
      extension.begin(), extension.end(), extension.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  return extension == ".mp3";
}

// The four tabs of the app.
enum class AppTab { Player, Folders, Queue, Search };

// All player data in one place (not used yet - planned for the Player tab).
struct AppState {
  audio_player player;

  std::vector<std::string> songs; // song file paths
  std::string folderPath;
  std::string folderName;

  selection songCursor;

  std::size_t playingIndex = 0; // which song is playing

  bool hasTrack = false;
  bool isPlaying = false;
  bool isPaused = false;
  bool shuffleEnabled = false;
};

// Draw the tab bar at the top. The active tab is shown like [Folders].
void showTabs(AppTab active) {
  const char *names[] = {"Player", "Folders", "Queue", "Search"};

  std::cout << "MUZX  ";

  for (int i = 0; i < 4; i++) {
    std::string label = names[i];

    if (static_cast<int>(active) == i)
      label = "[" + label + "]";

    std::cout << label;

    // Pad each tab to 12 characters so they line up (needed for mouse clicks).
    for (std::size_t j = label.size(); j < 12; j++)
      std::cout << ' ';
  }

  std::cout << "\n-----------------------------------------------\n\n";
}

// Draw the list of music folders. "> " marks the selected one.
void showLibrary(const library &musicLibrary, const selection &cursor,
                 AppTab active) {
  std::cout << "\033[2J\033[H"; // clear screen, move cursor to top-left

  showTabs(active);

  std::cout << "MUSIC LIBRARY\n\n";
  if (musicLibrary.empty()) {
    std::cout << "No folders found.\n";
  }

  for (std::size_t i = 0; i < musicLibrary.size(); i++) {
    std::cout << (i == cursor.current() ? "> " : "  ");
    std::cout << musicLibrary.itemName(i) << '\n';
  }

  std::cout << "\n[j/k] Move  [Enter] Open Folder  [q] Quit\n";
}

// Move the cursor to a specific position by stepping up/down.
void moveCursorTo(selection &cursor, std::size_t target, std::size_t count) {
  while (cursor.current() < target)
    cursor.moveDown(count);

  while (cursor.current() > target)
    cursor.moveUp(count);
}

// The song screen for one folder: lists songs and handles playback.
void showSongs(const std::string &folder, const std::string &folderName) {
  std::vector<std::string> songs;

  // Find all .mp3 files in the folder.
  try {
    for (const auto &entry : fs::directory_iterator(folder)) {
      if (entry.is_regular_file() && isMp3(entry.path()))
        songs.push_back(entry.path().string());
    }
  } catch (const fs::filesystem_error &error) {
    // Folder couldn't be opened: show the error briefly and go back.
    std::cerr << "Could not open folder: " << error.what() << '\n';
    usleep(1500000); // wait 1.5 seconds
    return;
  }

  std::sort(songs.begin(), songs.end()); // alphabetical order

  selection cursor;
  audio_player player;

  // +1 because item 0 is "[Play All]", songs start at item 1.
  const std::size_t menuCount = songs.size() + 1;

  bool isPlaying = false;
  bool isPaused = false;
  bool hasTrack = false; // true once a song has been started

  std::size_t playingIndex = 0; // index of the current song in `songs`

  // Set in the past so the first screen draw happens right away.
  auto lastRefresh =
      std::chrono::steady_clock::now() - std::chrono::milliseconds(300);

  AppTab activeTab = AppTab::Folders; // (unused here)

  // ----- Main loop of the song screen -----
  while (true) {
    // Auto-play: when a song ends, start the next one.
    if (isPlaying && player.isFinished()) {
      if (playingIndex + 1 < songs.size()) {
        playingIndex++;

        // Move the cursor to the new song (+1 for "[Play All]").
        moveCursorTo(cursor, playingIndex + 1, songs.size());

        if (player.load(songs[playingIndex])) {
          player.play();
          isPlaying = true;
          isPaused = false;
        } else {
          isPlaying = false;
          isPaused = false;
        }
      } else {
        // That was the last song: stop.
        isPlaying = false;
        isPaused = false;
      }

      // Force a redraw.
      lastRefresh =
          std::chrono::steady_clock::now() - std::chrono::milliseconds(300);
    }

    // Redraw the screen every 300 ms (keeps the progress bar moving).
    auto now = std::chrono::steady_clock::now();

    if (now - lastRefresh >= std::chrono::milliseconds(300)) {
      std::cout << "\033[2J\033[H"; // clear screen
      std::cout << "============== " << folderName << " ==============\n\n";

      if (songs.empty()) {
        std::cout << "No MP3 files found in this folder.\n";
      }

      // First menu item: Play All
      std::cout << (cursor.current() == 0 ? "> " : "  ") << "[Play All]\n";

      // Song list
      for (std::size_t i = 0; i < songs.size(); i++) {
        std::cout << (i + 1 == cursor.current() ? "> " : "  ");

        std::cout << fs::path(songs[i]).filename().string(); // name only

        // Show a label next to the current song.
        if (hasTrack && i == playingIndex && isPlaying)
          std::cout << " [PLAYING]";
        else if (hasTrack && i == playingIndex && isPaused)
          std::cout << " [PAUSED]";

        std::cout << '\n';
      }

      std::cout << "\n-----------------------------------------------\n";

      // Now-playing info
      if (hasTrack && playingIndex < songs.size()) {
        std::cout << "Track: "
                  << fs::path(songs[playingIndex]).filename().string() << '\n';

        if (isPlaying)
          std::cout << "Status: Playing\n";
        else if (isPaused)
          std::cout << "Status: Paused\n";
        else
          std::cout << "Status: Stopped\n";

        int currentSeconds =
            static_cast<int>(player.getPlayingOffset().asSeconds());

        int totalSeconds = static_cast<int>(player.getDuration().asSeconds());

        // Progress = how much of the song has played (0.0 to 1.0).
        double progress = 0.0;

        if (totalSeconds > 0) {
          progress = static_cast<double>(currentSeconds) / totalSeconds;

          if (progress > 1.0)
            progress = 1.0;
        }

        // Draw a 20-character bar: # = played, - = remaining.
        const int barWidth = 20;
        int filled = static_cast<int>(progress * barWidth);

        std::cout << "Progress: [";

        for (int i = 0; i < barWidth; i++)
          std::cout << (i < filled ? '#' : '-');

        // Print times as m:ss / m:ss (adds a leading 0 to seconds if < 10).
        std::cout << "] " << currentSeconds / 60 << ':'
                  << (currentSeconds % 60 < 10 ? "0" : "")
                  << currentSeconds % 60 << " / " << totalSeconds / 60 << ':'
                  << (totalSeconds % 60 < 10 ? "0" : "") << totalSeconds % 60
                  << '\n';
      } else {
        std::cout << "Status: Stopped\n";
      }

      std::cout << "Volume: " << static_cast<int>(player.getVolume()) << "%\n";

      // Help text
      std::cout << "\n[j/k] Move  [Enter] Play Selected\n"
                << "[p] Pause  [r] Resume  [x] Stop\n"
                << "[n] Next  [b] Previous  [+/-] Volume\n"
                << "[q] Back to Folders\n";

      lastRefresh = now;
    }

    // No key pressed? Sleep a tiny bit and loop again
    // (so auto-play and the progress bar keep working).
    if (!keyAvailable()) {
      usleep(10000); // 10 ms
      continue;
    }

    char key = getkey();

    if (key == 'j') {
      cursor.moveDown(menuCount); // move selection down
    } else if (key == 'k') {
      cursor.moveUp(menuCount); // move selection up
    } else if (key == '\n') {
      // Enter: play the selected item.
      if (!songs.empty()) {
        // Item 0 = Play All (start at song 0), otherwise the chosen song.
        playingIndex = (cursor.current() == 0) ? 0 : cursor.current() - 1;

        hasTrack = true;

        moveCursorTo(cursor, playingIndex + 1, menuCount);

        player.stop(); // stop the old song first

        if (player.load(songs[playingIndex])) {
          player.play();
          isPlaying = true;
          isPaused = false;
        } else {
          isPlaying = false;
          isPaused = false;
          std::cerr << "Could not load this song.\n";
          usleep(1000000); // show the error for 1 second
        }
      }
    } else if (key == 'p' && isPlaying) {
      // Pause
      player.pause();
      isPlaying = false;
      isPaused = true;
    } else if (key == 'r' && isPaused) {
      // Resume
      player.resume();
      isPlaying = true;
      isPaused = false;
    } else if (key == 'x') {
      // Stop
      player.stop();
      isPlaying = false;
      isPaused = false;
    } else if (key == 'n' && !songs.empty()) {
      // Next song
      if (hasTrack && playingIndex + 1 < songs.size())
        playingIndex++;
      else if (!hasTrack && cursor.current() + 1 < songs.size())
        playingIndex = cursor.current() + 1;
      else
        continue; // no next song

      hasTrack = true;
      moveCursorTo(cursor, playingIndex, songs.size());

      player.stop();

      if (player.load(songs[playingIndex])) {
        player.play();
        isPlaying = true;
        isPaused = false;
      } else {
        isPlaying = false;
        isPaused = false;
      }
    } else if (key == 'b' && !songs.empty()) {
      // Previous song
      if (hasTrack && playingIndex > 0)
        playingIndex--;
      else if (!hasTrack && cursor.current() >= 2)
        playingIndex = cursor.current() - 2;
      else
        continue; // no previous song

      hasTrack = true;
      moveCursorTo(cursor, playingIndex, songs.size());

      player.stop();

      if (player.load(songs[playingIndex])) {
        player.play();
        isPlaying = true;
        isPaused = false;
      } else {
        isPlaying = false;
        isPaused = false;
      }
    } else if (key == '+') {
      player.increaseVolume();
    } else if (key == '-') {
      player.decreaseVolume();
    } else if (key == 'q') {
      // Quit this screen: stop music and go back to folders.
      player.stop();
      break;
    }

    // After any key, redraw the screen right away.
    lastRefresh =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(300);
  }
}

// ---------- Program starts here ----------
int main() {
  config settings;      // loads saved settings
  library musicLibrary; // list of folders
  selection cursor;     // selected folder

  AppTab activeTab = AppTab::Folders; // start on the Folders tab

  std::string musicFolder = settings.get_music_folder();

  // First run: ask the user for the music folder and save it.
  if (musicFolder.empty()) {
    std::cout << "Enter your music folder: ";
    std::getline(std::cin, musicFolder);

    if (!settings.save_music_folder(musicFolder)) {
      std::cerr << "Could not save music folder.\n";
      return 1;
    }
  }

  // Load the sub-folders. Quit if there are none.
  if (!musicLibrary.load(musicFolder)) {
    std::cerr << "No folders found in: " << musicFolder << '\n';
    return 1;
  }

  enableRawMode(); // start reading keys/mouse instantly

  // ----- Main loop -----
  while (true) {
    // Draw the current tab.
    if (activeTab == AppTab::Folders) {
      showLibrary(musicLibrary, cursor, activeTab);
    } else {
      // Other tabs are placeholders for now.
      std::cout << "\033[2J\033[H";
      showTabs(activeTab);

      if (activeTab == AppTab::Player)
        std::cout << "PLAYER\n\nPlayer view will be connected next.\n";
      else if (activeTab == AppTab::Queue)
        std::cout << "QUEUE\n\nQueue view will be connected next.\n";
      else if (activeTab == AppTab::Search)
        std::cout << "SEARCH\n\nSearch will be connected next.\n";

      std::cout << "\n[Tab] Next tab  [1-4] Switch tab  [q] Quit\n";
    }

    // Wait (without using much CPU) until the user does something.
    while (!keyAvailable())
      usleep(10000);

    InputEvent event = readInputEvent();

    // Mouse click on the tab bar (row 1) -> switch tab.
    // Each tab is 12 characters wide, starting at column 7.
    if (event.mouseClick && event.mouseY == 1 && event.mouseX >= 7 &&
        event.mouseX < 55) {
      int tabIndex = (event.mouseX - 7) / 12;
      activeTab = static_cast<AppTab>(tabIndex);
      continue;
    }

    char key = event.key;

    if (key == '\t') {
      // Tab key: go to the next tab (wraps around after the last).
      activeTab = static_cast<AppTab>((static_cast<int>(activeTab) + 1) % 4);
    } else if (key == '1') {
      activeTab = AppTab::Player;
    } else if (key == '2') {
      activeTab = AppTab::Folders;
    } else if (key == '3') {
      activeTab = AppTab::Queue;
    } else if (key == '4') {
      activeTab = AppTab::Search;
    } else if (activeTab == AppTab::Folders && key == 'j') {
      cursor.moveDown(musicLibrary.size()); // next folder
    } else if (activeTab == AppTab::Folders && key == 'k') {
      cursor.moveUp(musicLibrary.size()); // previous folder
    } else if (activeTab == AppTab::Folders && key == '\n') {
      // Enter: open the selected folder's song screen.
      if (!musicLibrary.empty()) {
        showSongs(musicLibrary.item(cursor.current()),
                  musicLibrary.itemName(cursor.current()));
      }
    } else if (key == 'q') {
      break; // quit the app
    }
  }

  disableRawMode(); // restore the terminal before exiting
  return 0;
}
