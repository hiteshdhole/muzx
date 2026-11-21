// key and mouse handling
#include "muzx.hpp"

namespace muzx {

KeyResult handleMouse(AppState &app, const fs::path &queueFile) {
  const MouseEvent m = lastMouse;

  if (m.button == 64 || m.button == 65) { // wheel up / down
    const bool up = m.button == 64;
    auto move = [&](selection &cursor, std::size_t count) {
      if (count == 0)
        return;
      if (up)
        cursor.moveUp(count);
      else
        cursor.moveDown(count);
    };
    switch (app.tab) {
    case AppTab::Player:
      if (up)
        app.player.increaseVolume();
      else
        app.player.decreaseVolume();
      break;
    case AppTab::Folders: move(app.folderCursor, folderMenuSize(app)); break;
    case AppTab::Queue: move(app.queueCursor, app.songQueue.size()); break;
    case AppTab::Search: move(app.searchCursor, app.searchResults.size()); break;
    case AppTab::Settings: move(app.settingsCursor, kSettingCount); break;
    default: break;
    }
    return KeyResult::Handled;
  }
  if (m.button == 32) { // the mouse moved with the left button held down
    if (g_drag.active && app.hasCurrentSong) {
      const double fraction =
          static_cast<double>(m.x - g_drag.col0) / std::max(1, g_drag.col1 - g_drag.col0);
      const double target = std::clamp(fraction, 0.0, 1.0) * g_drag.duration;
      const double delta = target - g_drag.target;
      if (std::fabs(delta) >= 0.05) {
        if (delta > 0)
          app.player.seekForward(static_cast<float>(delta));
        else
          app.player.seekBackward(static_cast<float>(-delta));
        g_drag.target = target;
      }
    }
    return KeyResult::Handled;
  }
  if (!m.press) { // button released: the drag is over
    g_drag.active = false;
    return KeyResult::Handled;
  }
  g_drag.active = false;
  if (m.button != 0) // only a plain left click
    return KeyResult::Handled;

  for (auto it = g_clicks.rbegin(); it != g_clicks.rend(); ++it) {
    if (m.y < it->row0 || m.y > it->row1 || m.x < it->col0 || m.x > it->col1)
      continue;
    switch (it->action) {
    case ClkTab:
      app.tab = static_cast<AppTab>(it->arg);
      break;
    case ClkShuffle:
      setShuffle(app, !app.shuffle);
      break;
    case ClkPrev:
      previousSong(app);
      break;
    case ClkPlayPause:
      togglePause(app);
      break;
    case ClkNext:
      nextSong(app);
      break;
    case ClkRepeat:
      app.repeat = static_cast<RepeatMode>((static_cast<int>(app.repeat) + 1) % 3);
      break;
    case ClkSeek: {
      if (!app.hasCurrentSong)
        break;
      const float duration = app.player.getDuration().asSeconds();
      const float position = app.player.getPlayingOffset().asSeconds();
      if (!std::isfinite(duration) || duration <= 0.1F)
        break;
      const float fraction = static_cast<float>(m.x - it->col0) /
                             static_cast<float>(std::max(1, it->col1 - it->col0));
      const float target = std::clamp(fraction, 0.0F, 1.0F) * duration;
      const float delta = target - position;
      if (delta >= 0.0F)
        app.player.seekForward(delta);
      else
        app.player.seekBackward(-delta);
      // Keep the button held and move: the bar follows the mouse.
      g_drag = {true, static_cast<double>(target), static_cast<double>(duration),
                it->col0, it->col1};
      break;
    }
    case ClkVolume: {
      const double now = app.player.getVolume() / 100.0;
      if (now > 0.02) {
        app.volumeBeforeMute = now;
        setPlayerVolume(app, 0.0);
      } else {
        setPlayerVolume(app, app.volumeBeforeMute > 0.02 ? app.volumeBeforeMute : 0.5);
      }
      break;
    }
    case ClkListItem: {
      selection *cursor = nullptr;
      std::size_t count = 0;
      if (app.tab == AppTab::Folders) { cursor = &app.folderCursor; count = folderMenuSize(app); }
      else if (app.tab == AppTab::Queue) { cursor = &app.queueCursor; count = app.songQueue.size(); }
      else if (app.tab == AppTab::Search) { cursor = &app.searchCursor; count = app.searchResults.size(); }
      const std::size_t target = static_cast<std::size_t>(it->arg);
      if (!cursor || target >= count)
        break;
      if (target == cursor->current())
        return handleKey(app, KeyEnter, queueFile); // click the selected row again = open / play
      while (cursor->current() < target)
        cursor->moveDown(count);
      while (cursor->current() > target)
        cursor->moveUp(count);
      break;
    }
    case ClkSetting:
      if (static_cast<std::size_t>(it->arg) == app.settingsCursor.current())
        return handleKey(app, KeyEnter, queueFile); // change / open the selected setting
      while (app.settingsCursor.current() < static_cast<std::size_t>(it->arg))
        app.settingsCursor.moveDown(kSettingCount);
      while (app.settingsCursor.current() > static_cast<std::size_t>(it->arg))
        app.settingsCursor.moveUp(kSettingCount);
      break;
    }
    break; // the first (topmost) region wins
  }
  return KeyResult::Handled;
}

KeyResult handleKey(AppState &app, int key, const fs::path &queueFile) {
  std::error_code ec;

  if (key == KeyMouse)
    return handleMouse(app, queueFile);

  if (key >= '1' && key <= '7') {
    app.tab = static_cast<AppTab>(key - '1');
    return KeyResult::Handled;
  }
  if (key == '\t') {
    app.tab = static_cast<AppTab>((static_cast<int>(app.tab) + 1) % kTabCount);
    return KeyResult::Handled;
  }
  if (key == KeyShiftTab) {
    app.tab = static_cast<AppTab>(
        (static_cast<int>(app.tab) + kTabCount - 1) % kTabCount);
    return KeyResult::Handled;
  }

  if (app.tab == AppTab::Settings) {
    if (key == 'j' || key == KeyDown) {
      app.settingsCursor.moveDown(kSettingCount);
      return KeyResult::Handled;
    }
    if (key == 'k' || key == KeyUp) {
      app.settingsCursor.moveUp(kSettingCount);
      return KeyResult::Handled;
    }
    const bool forward = key == KeyEnter || key == KeyRight || key == 'l';
    if (forward || key == KeyLeft || key == 'h') {
      const int item = static_cast<int>(app.settingsCursor.current());
      if (item == SetFolder) {
        if (forward)
          askMusicFolder(app);
        return KeyResult::FlushInput;
      }
      changeSetting(app, item, forward ? 1 : -1);
      saveSettings(app);
      return KeyResult::Handled;
    }
  }

  if (key == 'q' || key == KeyEscape)
    return KeyResult::Quit;

  if (key == ' ') {
    togglePause(app);
    return KeyResult::Handled;
  }
  if (key == 'P') {
    playFolder(app, app.musicRoot, true);
    return KeyResult::Handled;
  }
  if (key == 's') {
    setShuffle(app, !app.shuffle);
    return KeyResult::Handled;
  }
  if (key == 'R') {
    app.repeat = static_cast<RepeatMode>((static_cast<int>(app.repeat) + 1) % 3);
    return KeyResult::Handled;
  }
  if (key == 'i') {
    app.showAlbumArt = !app.showAlbumArt;
    return KeyResult::Handled;
  }
  if (key == 'v') {
    app.showVisualizer = !app.showVisualizer;
    return KeyResult::Handled;
  }
  if (key == 'L') {
    // Off -> Right panel -> Below the player -> Off
    app.lyricsMode = app.lyricsMode == LyricsMode::Off
                         ? LyricsMode::Right
                         : (app.lyricsMode == LyricsMode::Right
                                ? LyricsMode::Bottom
                                : LyricsMode::Off);
    return KeyResult::Handled;
  }
  if (key == '[' || key == ']' || key == '\\') {
    // Lyrics timing for this song: [ earlier, ] later, \ reset
    adjustLyricShift(app, key == '[' ? -0.5 : key == ']' ? 0.5 : 0.0);
    return KeyResult::Handled;
  }
  if (key == 'T') {
    app.lcd.toggle(); // LCD on/off
    return KeyResult::Handled;
  }
  if (key == 'm' || key == 'M') {
    // Change what the Arduino LCD shows (needs the muzxlcd program)
    app.lcd.command(key == 'm' ? "mode next" : "mode prev");
    return KeyResult::Handled;
  }
  if (key == 'n' || (app.tab == AppTab::Player && key == 'l')) {
    nextSong(app);
    return KeyResult::Handled;
  }
  if (key == 'b' || (app.tab == AppTab::Player && key == 'h')) {
    previousSong(app);
    return KeyResult::Handled;
  }
  if (key == KeyLeft || key == 'a') {
    seekLeft(app);
    return KeyResult::Handled;
  }
  if (key == KeyRight || key == 'd') {
    seekRight(app);
    return KeyResult::Handled;
  }
  if (key == 'x') {
    app.player.stop();
    app.playbackRunning = false;
    app.paused = false;
    return KeyResult::Handled;
  }
  if (key == '+' || key == '=') {
    app.player.increaseVolume();
    return KeyResult::Handled;
  }
  if (key == '-') {
    app.player.decreaseVolume();
    return KeyResult::Handled;
  }
  if (key == '/') {
    askSearchQuery(app);
    return KeyResult::FlushInput;
  }

  if (key == 'j' || key == KeyDown) {
    if (app.tab == AppTab::Folders && !app.folderEntries.empty())
      app.folderCursor.moveDown(folderMenuSize(app));
    else if (app.tab == AppTab::Queue && !app.songQueue.empty())
      app.queueCursor.moveDown(app.songQueue.size());
    else if (app.tab == AppTab::Search && !app.searchResults.empty())
      app.searchCursor.moveDown(app.searchResults.size());
    return KeyResult::Handled;
  }
  if (key == 'k' || key == KeyUp) {
    if (app.tab == AppTab::Folders && !app.folderEntries.empty())
      app.folderCursor.moveUp(folderMenuSize(app));
    else if (app.tab == AppTab::Queue && !app.songQueue.empty())
      app.queueCursor.moveUp(app.songQueue.size());
    else if (app.tab == AppTab::Search && !app.searchResults.empty())
      app.searchCursor.moveUp(app.searchResults.size());
    return KeyResult::Handled;
  }

  if (app.tab == AppTab::Folders) {
    if (key == KeyBackspace || key == 'h') {
      if (app.browseFolder != app.musicRoot) {
        const fs::path parent = app.browseFolder.parent_path();
        if (!parent.empty() && parent != app.browseFolder)
          openFolder(app, parent);
      }
    } else if (key == 'p') {
      if (!app.folderEntries.empty()) {
        std::size_t index = app.folderCursor.current();
        if (index > 0 && index - 1 < app.folderEntries.size() &&
            fs::is_directory(app.folderEntries[index - 1], ec)) {
          playFolder(app, app.folderEntries[index - 1], true);
        } else {
          playFolder(app, app.browseFolder, true);
        }
      }
    } else if (key == 'e') {
      if (!app.folderEntries.empty()) {
        std::size_t index = app.folderCursor.current();
        if (index == 0) {
          addToQueue(app, app.browseFolder);
        } else if (index - 1 < app.folderEntries.size()) {
          addToQueue(app, app.folderEntries[index - 1]);
        }
        app.songQueue.save(queueFile.string());
      }
    } else if (key == KeyEnter || key == 'l') {
      if (!app.folderEntries.empty()) {
        std::size_t index = app.folderCursor.current();
        if (index == 0) {
          playFolder(app, app.browseFolder, true);
        } else if (index - 1 < app.folderEntries.size()) {
          const fs::path chosen = app.folderEntries[index - 1];
          if (fs::is_directory(chosen, ec))
            openFolder(app, chosen);
          else
            playSongFromFolder(app, chosen);
        }
      }
    }
  } else if (app.tab == AppTab::Queue) {
    // ('d' used to be listed here too, but it is always taken by seek-right
    // above, so that branch could never run.)
    if (key == 'r' && !app.songQueue.empty()) {
      std::size_t index = app.queueCursor.current();
      if (index < app.songQueue.size())
        app.songQueue.remove(index);
      app.queueCursor = selection{};
      app.songQueue.save(queueFile.string());
    } else if (key == 'c') {
      app.songQueue.clear();
      app.queueCursor = selection{};
      app.songQueue.save(queueFile.string());
    } else if ((key == KeyEnter || key == 'l') && !app.songQueue.empty()) {
      std::size_t index = app.queueCursor.current();
      if (index < app.songQueue.size())
        playFromPlaylist(app, queueAsList(app.songQueue), index);
    }
  } else if (app.tab == AppTab::Search) {
    if (key == 'e' && !app.searchResults.empty()) {
      std::size_t index = app.searchCursor.current();
      if (index < app.searchResults.size()) {
        addToQueue(app, app.searchResults[index]);
        app.songQueue.save(queueFile.string());
      }
    } else if (key == KeyEnter && !app.searchResults.empty()) {
      std::size_t index = app.searchCursor.current();
      if (index < app.searchResults.size())
        playSongFromFolder(app, app.searchResults[index]);
    }
  } else if (app.tab == AppTab::Player) {
    if (key == 'e' && app.hasCurrentSong) {
      addToQueue(app, app.currentSong);
      app.songQueue.save(queueFile.string());
    }
  }

  return KeyResult::Handled;
}

} // namespace muzx
