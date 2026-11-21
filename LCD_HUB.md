# MUZX LCD hub

One small program (`muzxlcd`) owns the Arduino LCD and shows different things
on it. MUZX sends its lyrics to it automatically.

| Mode | Shows |
|---|---|
| `lyrics` | Synced lyrics of whatever is playing: Spotify, YouTube Music, a browser tab, MUZX ... (found through the desktop media info, then lrclib) |
| `captions` | Captions of the YouTube video playing in the browser (needs `yt-dlp`) |
| `nowplaying` | Title and artist |
| `spectrum` | Live sound bars of whatever the PC plays (needs `parec`, see below) |
| `clock` | Time and date |
| `status` | CPU, RAM, temperature, network speed, battery, uptime (pages change every 3 s) |

## Setup

1. **Wire the button** (optional but nice): one leg to **D2**, the other leg to
   **GND**. No resistor needed.
2. **Upload `muzx_lcd.ino`** to the Uno again (it adds the button, the
   two-row screens and the bar characters for volume and spectrum). Close the Serial Monitor afterwards.
3. **Build and start the program**:
   ```
   make muzxlcd
   make install-lcd      # copies it to ~/.local/bin and starts it at login
   ```
   or just run `./muzxlcd &` to try it.

Only one program may use the Uno at a time. While `muzxlcd` runs, MUZX sends
its lyrics to it; when it is not running, MUZX talks to the Uno directly like
before.

## Switching modes

- **Button:** short press = next mode, long press (0.7 s) = previous mode.
  The screen shows the mode name for a moment.
- **PC:** `muzxlcd ctl mode next`, `... prev`, `... clock`, `... lyrics`,
  `... captions`, `... nowplaying`, `... spectrum`, `... status`. Bind these to keyboard
  shortcuts in your window manager if you like. `muzxlcd ctl status` shows the
  current mode and port, `muzxlcd ctl modes` lists them.

The last mode is remembered.

## Popups

On top of every mode, the screen shows for a moment:

- **Volume:** when the volume of the PC (or of Spotify / MUZX) changes, a
  volume bar appears for 2 seconds ("Volume 60%" or "Volume muted").
  Needs `pactl` (comes with `pipewire-pulse` / `libpulse`).
- **Notifications:** desktop notifications (Discord, Firefox, mail ...) scroll
  across the screen for 6 seconds. Needs `busctl` (part of systemd) and a
  notification program running (dunst, mako, KDE, GNOME). MUZX's own
  notifications are skipped.

Turn them on or off in MUZX: Settings tab (`7`), section **LCD HUB**. The same
section chooses which modes the button cycles through. Or from a terminal:

```
muzxlcd ctl set volume 0          # volume popup off (1 = on)
muzxlcd ctl set notify 0          # notification popup off
muzxlcd ctl set modes lyrics,spectrum,clock
```

These are saved in `lcd.conf`.

## Spectrum

The `spectrum` mode records what the PC plays with `parec` (the monitor of the
default output) and draws one bar per column, two rows high. It records only
while this mode is on screen. If it says "Install parec":
`sudo pacman -S libpulse`. To use another source set `spectrum_source=` in
`lcd.conf` (list them with `pactl list short sources`).

The bars adjust to the volume by themselves (automatic gain), so quiet and loud
music both fill the screen, and silence stays empty. Bass is on the left, treble
on the right; the sound is analysed 60 times a second. The numbers (frequency
range, how fast the bars fall, how much range they show) are at the top of
`spectrum.hpp` if you want to change the look.

## Settings (optional)

`~/.config/muzx/lcd.conf`, one `key=value` per line:

```
modes=lyrics,captions,nowplaying,spectrum,clock,status   # which modes, in this order
cols=16                                         # 20 for a 20x4 screen
port=/dev/ttyUSB0
captions_lang=en                                # language for YouTube captions
page_seconds=3                                  # time per PC status page
kana=0                                          # 1 = Japanese as katakana
volume_popup=1                                  # volume bar on volume change
notifications=1                                 # show desktop notifications
notify_seconds=6                                # how long a notification stays
spectrum_source=@DEFAULT_MONITOR@               # what the spectrum listens to
```

## Notes

- **Spotify / YouTube lyrics** are looked up by artist and title on lrclib and
  cached in `~/.cache/muzxlcd/`. Browser titles like `Artist - Song (Official
  Video)` are cleaned up first. If lrclib has no synced lyrics for a song, the
  screen shows the title and artist instead.
- **YouTube captions** need `yt-dlp` (`sudo pacman -S yt-dlp`) and a browser
  that reports the video address to the desktop (Chromium and Firefox do).
  Captions are fetched once per video, so the first seconds can show
  "Getting captions". Live streams have no captions this way.
- **MUZX lyrics** are used while MUZX is the one playing. For that,
  "LCD display" in MUZX Settings must be On.
  While MUZX is open, its "LCD Japanese" setting (romaji or katakana) is the
  one used for the whole screen; without MUZX the `kana=` line in `lcd.conf`
  applies.
- **Retries:** if the network is down when a song starts, lyrics are looked up
  again after 20 s; captions are retried after 2 min, or 15 s if `yt-dlp` was
  missing (so installing it is picked up without a restart).
- **Switch from MUZX:** press `m` (next) or `M` (previous) in the player.
- **Uploading a new sketch:** stop the program first, otherwise the upload
  fails ("programmer is not responding"):
  `systemctl --user stop muzxlcd`, upload, then
  `systemctl --user start muzxlcd`.
- The service sets its own `PATH` (`~/.local/bin:/usr/local/bin:/usr/bin`), so
  `yt-dlp` installed with pip or pipx is found.
- If nothing happens: `muzxlcd ctl status` (is it running, is the Uno found),
  and `journalctl --user -u muzxlcd` when started with `make install-lcd`.
