#pragma once
// MPRIS2 support: makes MUZX a normal "media player" on the desktop, so the
// keyboard media keys (play/pause, next, previous, stop), `playerctl`, the
// KDE/GNOME media widget and the lock-screen controls all work, even when the
// terminal is not focused. Uses sd-bus (part of systemd): link with -lsystemd.
//
// Everything runs on one background thread. The main program only
//   * publish()es its state (what is playing, paused, volume ...), and
//   * takeCommands() to get what the desktop asked for.
// If there is no D-Bus session, the thread simply ends and nothing happens.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <systemd/sd-bus.h>
#include <unistd.h>

enum class MprisCmd {
  Play,
  Pause,
  PlayPause,
  Stop,
  Next,
  Previous,
  Quit,
  SetVolume, // value: 0.0 .. 1.0
  SeekBy,    // value: seconds (may be negative)
  SeekTo,    // value: seconds from the start
  SetShuffle // value: 0 or 1
};

struct MprisCommand {
  MprisCmd type;
  double value = 0.0;
};

struct MprisState {
  std::string status = "Stopped"; // "Playing", "Paused" or "Stopped"
  std::string title, artist;
  std::int64_t lengthUs = 0, positionUs = 0;
  double volume = 1.0;
  bool shuffle = false, canNext = false, canPrevious = false, hasTrack = false;
  unsigned trackId = 0; // changes with every new song
};

class Mpris {
public:
  ~Mpris() { stop(); }

  void start() {
    if (thread_.joinable())
      return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
  }
  void stop() {
    running_ = false;
    if (thread_.joinable())
      thread_.join();
  }
  bool active() const { return active_; }

  void publish(const MprisState &s) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (s.status != state_.status || s.title != state_.title ||
        s.artist != state_.artist || s.lengthUs != state_.lengthUs ||
        s.trackId != state_.trackId || s.shuffle != state_.shuffle ||
        s.canNext != state_.canNext || s.canPrevious != state_.canPrevious ||
        s.hasTrack != state_.hasTrack ||
        std::fabs(s.volume - state_.volume) > 0.001)
      dirty_ = true;
    state_ = s;
  }

  std::vector<MprisCommand> takeCommands() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<MprisCommand> out;
    out.swap(commands_);
    return out;
  }

private:
  using Self = Mpris;
  static Self *self(void *userdata) { return static_cast<Self *>(userdata); }

  void push(MprisCmd type, double value = 0.0) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.size() < 64)
      commands_.push_back({type, value});
  }
  MprisState snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  // ---- methods --------------------------------------------------------------
  template <MprisCmd C>
  static int simple(sd_bus_message *m, void *u, sd_bus_error *) {
    self(u)->push(C);
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int raise(sd_bus_message *m, void *, sd_bus_error *) {
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int seek(sd_bus_message *m, void *u, sd_bus_error *) {
    std::int64_t offset = 0;
    if (sd_bus_message_read(m, "x", &offset) < 0)
      return -EINVAL;
    self(u)->push(MprisCmd::SeekBy, static_cast<double>(offset) / 1e6);
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int setPosition(sd_bus_message *m, void *u, sd_bus_error *) {
    const char *track = nullptr;
    std::int64_t position = 0;
    if (sd_bus_message_read(m, "ox", &track, &position) < 0)
      return -EINVAL;
    self(u)->push(MprisCmd::SeekTo, static_cast<double>(position) / 1e6);
    return sd_bus_reply_method_return(m, nullptr);
  }
  static int openUri(sd_bus_message *m, void *, sd_bus_error *) {
    return sd_bus_reply_method_return(m, nullptr);
  }

  // ---- properties -----------------------------------------------------------
  static int getRoot(sd_bus *, const char *, const char *, const char *property,
                     sd_bus_message *reply, void *, sd_bus_error *) {
    if (!std::strcmp(property, "Identity"))
      return sd_bus_message_append(reply, "s", "MUZX");
    if (!std::strcmp(property, "SupportedUriSchemes") ||
        !std::strcmp(property, "SupportedMimeTypes"))
      return sd_bus_message_append(reply, "as", 0);
    return sd_bus_message_append(reply, "b",
                                 !std::strcmp(property, "CanQuit") ? 1 : 0);
  }

  static int appendEntry(sd_bus_message *m, const char *key, const char *type,
                         const void *a, const void *b = nullptr) {
    int r = sd_bus_message_open_container(m, 'e', "sv");
    if (r < 0)
      return r;
    if ((r = sd_bus_message_append(m, "s", key)) < 0)
      return r;
    if ((r = sd_bus_message_open_container(m, 'v', type)) < 0)
      return r;
    if (!std::strcmp(type, "o") || !std::strcmp(type, "s"))
      r = sd_bus_message_append(m, type, static_cast<const char *>(a));
    else if (!std::strcmp(type, "x"))
      r = sd_bus_message_append(m, "x", *static_cast<const std::int64_t *>(a));
    else if (!std::strcmp(type, "as"))
      r = sd_bus_message_append(m, "as", 1, static_cast<const char *>(a));
    (void)b;
    if (r < 0)
      return r;
    if ((r = sd_bus_message_close_container(m)) < 0)
      return r;
    return sd_bus_message_close_container(m);
  }

  static int getMetadata(sd_bus_message *reply, const MprisState &s) {
    int r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0)
      return r;
    const std::string trackPath =
        s.hasTrack
            ? "/org/mpris/MediaPlayer2/Track/" + std::to_string(s.trackId)
            : std::string("/org/mpris/MediaPlayer2/TrackList/NoTrack");
    if ((r = appendEntry(reply, "mpris:trackid", "o", trackPath.c_str())) < 0)
      return r;
    if (s.hasTrack) {
      if ((r = appendEntry(reply, "mpris:length", "x", &s.lengthUs)) < 0)
        return r;
      if ((r = appendEntry(reply, "xesam:title", "s", s.title.c_str())) < 0)
        return r;
      if (!s.artist.empty() &&
          (r = appendEntry(reply, "xesam:artist", "as", s.artist.c_str())) < 0)
        return r;
    }
    return sd_bus_message_close_container(reply);
  }

  static int getPlayer(sd_bus *, const char *, const char *,
                       const char *property, sd_bus_message *reply, void *u,
                       sd_bus_error *) {
    const MprisState s = self(u)->snapshot();
    const std::string p = property;
    if (p == "PlaybackStatus")
      return sd_bus_message_append(reply, "s", s.status.c_str());
    if (p == "LoopStatus")
      return sd_bus_message_append(reply, "s", "None");
    if (p == "Rate" || p == "MinimumRate" || p == "MaximumRate")
      return sd_bus_message_append(reply, "d", 1.0);
    if (p == "Shuffle")
      return sd_bus_message_append(reply, "b", s.shuffle ? 1 : 0);
    if (p == "Metadata")
      return getMetadata(reply, s);
    if (p == "Volume")
      return sd_bus_message_append(reply, "d", s.volume);
    if (p == "Position")
      return sd_bus_message_append(reply, "x", s.positionUs);
    if (p == "CanGoNext")
      return sd_bus_message_append(reply, "b", s.canNext ? 1 : 0);
    if (p == "CanGoPrevious")
      return sd_bus_message_append(reply, "b", s.canPrevious ? 1 : 0);
    return sd_bus_message_append(reply, "b",
                                 1); // CanPlay, CanPause, CanSeek, CanControl
  }

  static int setPlayer(sd_bus *, const char *, const char *,
                       const char *property, sd_bus_message *value, void *u,
                       sd_bus_error *) {
    if (!std::strcmp(property, "Volume")) {
      double v = 0.0;
      if (sd_bus_message_read(value, "d", &v) < 0)
        return -EINVAL;
      self(u)->push(MprisCmd::SetVolume, v < 0 ? 0 : v > 1 ? 1 : v);
    } else if (!std::strcmp(property, "Shuffle")) {
      int b = 0;
      if (sd_bus_message_read(value, "b", &b) < 0)
        return -EINVAL;
      self(u)->push(MprisCmd::SetShuffle, b ? 1.0 : 0.0);
    }
    return 1;
  }

  // ---- the bus thread -------------------------------------------------------
  void run() {
    static const sd_bus_vtable rootVt[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Raise", "", "", raise, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Quit", "", "", simple<MprisCmd::Quit>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("CanQuit", "b", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanRaise", "b", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("HasTrackList", "b", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Identity", "s", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SupportedUriSchemes", "as", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SupportedMimeTypes", "as", getRoot, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END};
    static const sd_bus_vtable playerVt[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD("Next", "", "", simple<MprisCmd::Next>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Previous", "", "", simple<MprisCmd::Previous>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Pause", "", "", simple<MprisCmd::Pause>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("PlayPause", "", "", simple<MprisCmd::PlayPause>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Stop", "", "", simple<MprisCmd::Stop>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Play", "", "", simple<MprisCmd::Play>,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("Seek", "x", "", seek, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("SetPosition", "ox", "", setPosition,
                      SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD("OpenUri", "s", "", openUri, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("PlaybackStatus", "s", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("LoopStatus", "s", getPlayer, 0, 0),
        SD_BUS_PROPERTY("Rate", "d", getPlayer, 0, 0),
        SD_BUS_WRITABLE_PROPERTY("Shuffle", "b", getPlayer, setPlayer, 0,
                                 SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Metadata", "a{sv}", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_WRITABLE_PROPERTY("Volume", "d", getPlayer, setPlayer, 0,
                                 SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("Position", "x", getPlayer, 0, 0),
        SD_BUS_PROPERTY("MinimumRate", "d", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MaximumRate", "d", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanGoNext", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CanGoPrevious", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("CanPlay", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanPause", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanSeek", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CanControl", "b", getPlayer, 0,
                        SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_VTABLE_END};

    sd_bus *bus = nullptr;
    sd_bus_slot *rootSlot = nullptr, *playerSlot = nullptr;
    const char *path = "/org/mpris/MediaPlayer2";
    bool ok =
        sd_bus_open_user(&bus) >= 0 &&
        sd_bus_add_object_vtable(bus, &rootSlot, path, "org.mpris.MediaPlayer2",
                                 rootVt, this) >= 0 &&
        sd_bus_add_object_vtable(bus, &playerSlot, path,
                                 "org.mpris.MediaPlayer2.Player", playerVt,
                                 this) >= 0;
    if (ok && sd_bus_request_name(bus, "org.mpris.MediaPlayer2.muzx", 0) < 0) {
      // A second MUZX is already running: take a unique name instead.
      const std::string other =
          "org.mpris.MediaPlayer2.muzx.instance" + std::to_string(getpid());
      ok = sd_bus_request_name(bus, other.c_str(), 0) >= 0;
    }
    if (ok) {
      active_ = true;
      while (running_) {
        const int r = sd_bus_process(bus, nullptr);
        if (r < 0)
          break;
        if (dirty_.exchange(false))
          sd_bus_emit_properties_changed(
              bus, path, "org.mpris.MediaPlayer2.Player", "PlaybackStatus",
              "Metadata", "Volume", "Shuffle", "CanGoNext", "CanGoPrevious",
              nullptr);
        if (r > 0)
          continue;
        sd_bus_wait(bus, 150000); // wakes for incoming calls; 150 ms at most
      }
      active_ = false;
    }
    sd_bus_slot_unref(playerSlot);
    sd_bus_slot_unref(rootSlot);
    sd_bus_flush_close_unref(bus);
  }

  mutable std::mutex mutex_;
  MprisState state_;
  std::vector<MprisCommand> commands_;
  std::atomic<bool> running_{false}, active_{false}, dirty_{false};
  std::thread thread_;
};
