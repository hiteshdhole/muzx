# MUZX

A terminal music player for Linux with a Spotify-style lyrics panel, album art,
a live audio visualizer, desktop media-key support and an optional Arduino LCD
that shows the line being sung.

Built with C++17 and SFML. Made for the [Kitty](https://sw.kovidgoyal.net/kitty/)
terminal (cover art uses Kitty's graphics protocol); everything else works in
any modern terminal.

## Features

- Browse folders, queue songs, search by filename, shuffle and repeat
- **Synced lyrics**, like Spotify: the line being sung is highlighted
  - looked up automatically online (lrclib) and cached
  - songs without lyrics can be **generated with Whisper** (timed, offline)
  - **timing offset** per song: `[` / `]` shift the lyrics earlier / later
  - **background preparation** of lyrics for the other songs (Settings)
- Album art, audio visualizer, listening statistics
- **Mouse support**: clickable tabs, shuffle / previous / play-pause / next /
  repeat buttons, list rows, **drag the progress bar to seek**, **click the
  volume number to mute**; the wheel scrolls and changes volume
- **Media keys and `playerctl`** work everywhere on the desktop (MPRIS)
- **Settings tab** with themes, a custom colour editor, font size, and more
- Desktop notification when a new song starts
- Lyrics on an **I2C LCD** (Arduino Uno), with Japanese, Hindi, Punjabi, Urdu
  and other scripts converted to readable letters

## Requirements

| Needed for                                    | Package (Arch)                     |
| --------------------------------------------- | ---------------------------------- |
| everything                                    | `gcc`, `sfml`, `systemd-libs`      |
| cover art, font size                          | `kitty`                            |
| Whisper lyrics (optional)                     | `pipx install whisper-ctranslate2` |
| Japanese kanji on the LCD (optional)          | `kakasi` (AUR)                     |
| notifications (optional)                      | `libnotify`                        |
| media keys on i3 / Sway / Hyprland (optional) | `playerctl`                        |

## Build

```bash
g++ -std=c++17 -O2 -pthread main.cpp lyrics.cpp audio_player.cpp cover.cpp \
    queue.cpp selection.cpp config.cpp -o player \
    -lsfml-audio -lsfml-system -lsystemd
./player
```

Do not compile the test files (`*_test.cpp`, `keytest.cpp`): they have their
own `main()`.

On the first start MUZX asks for your music folder.

## Keys

| Key                          | Action                                                |
| ---------------------------- | ----------------------------------------------------- |
| `Space`                      | Play / pause                                          |
| `←` `→` or `a` `d`           | Seek backward / forward                               |
| `n` / `b`                    | Next / previous song                                  |
| `s`                          | Shuffle on / off                                      |
| `R`                          | Repeat: off, all songs, one song                      |
| `+` `-`                      | Volume                                                |
| `x`                          | Stop                                                  |
| `i` / `v`                    | Album art / visualizer on or off                      |
| `L`                          | Lyrics panel: right, below, off                       |
| `G`                          | Generate lyrics with Whisper (press again to cancel)  |
| `A`                          | Auto-generate lyrics when a song has none             |
| `[` `]`                      | Lyrics earlier / later by 0.5 s (saved for this song) |
| `\`                          | Reset the lyrics timing of this song                  |
| `T`                          | Lyrics on the Arduino LCD, on or off                  |
| `Tab` `Shift+Tab` or `1`–`7` | Switch tabs                                           |
| `j` `k` or `↑` `↓`           | Move in lists                                         |
| `Enter` or `l`               | Open folder / play song                               |
| `Backspace` or `h`           | Parent folder                                         |
| `p` / `P`                    | Play this folder / play everything                    |
| `e`                          | Add to queue                                          |
| `r` / `c`                    | Remove from queue / clear queue                       |
| `/`                          | Search                                                |
| `q` or `Esc`                 | Quit                                                  |

**Mouse:** click a tab, a button or a list row (click a selected row again to
open or play it). Press on the progress bar and drag to seek; click the
"Volume: NN%" text to mute and unmute. The wheel scrolls lists and changes
the volume on the Player tab. Hold `Shift` to select text normally.
Turn the mouse off with `MUZX_MOUSE=0`.

## Settings (tab 7)

Move with `j`/`k`, change with `Enter` or `←`/`→` (or click). Saved in
`~/.config/muzx/settings.conf`.

Repeat, lyrics panel position, auto-generate, Whisper model and language,
prepare lyrics (background queue, see below), LCD
on/off and Japanese mode, album art, visualizer, layout (normal or compact),
control buttons, notifications, theme (Purple, Ocean, Forest, Sunset, Mono,
Custom), the four custom colours, font size, and the music folder.

**Font size** works only in Kitty and needs this line in
`~/.config/kitty/kitty.conf` (then restart Kitty):

```
allow_remote_control yes
```

## Lyrics

MUZX uses a `.lrc` file next to the song, lyrics stored in the file's tags,
its own cache, and online sources (lrclib, then `syncedlyrics` if it is
installed). If nothing is found and auto-generate is on, Whisper creates timed
lyrics in the background (a few minutes per song on a CPU). Everything is
cached in `~/.config/muzx/lyrics/`, so each song is only processed once.

Set the language (`ja`, `hi`, `en`, ...) in Settings for better results.

**Lyrics out of sync?** Press `]` if the lyrics come too early, `[` if they
come too late (0.5 s per press, shown as "sync +1.0s" in the panel title).
It is remembered for that song in `~/.config/muzx/lyrics-offsets.txt`;
`\` resets it.

**Prepare lyrics (Settings):** instead of waiting until you play a song, MUZX
can look up lyrics for the other songs in the background, the rest of the
playlist first, then the whole music folder, one song at a time.

- _Off_ (default): nothing happens in the background.
- _Online only_: only the web lookup, which is light.
- _Online + Whisper_: songs that no source has are generated with Whisper.
  This is heavy (it runs at low priority with one CPU thread and pauses when
  you press `G` or a generation for the current song runs), so use it when
  the computer is idle. A song Whisper heard nothing in is not retried.

Background lookups do not know the song length, so a version of the lyrics
with a different length may be picked; use `[` / `]` to fix the timing.

## Media keys

MUZX appears as a media player on the desktop (MPRIS). Play/pause, next,
previous and stop keys work, and so does `playerctl`:

```bash
playerctl -p muzx play-pause
playerctl -p muzx volume 0.5
```

On i3, Sway or Hyprland bind the keys yourself, for example
`bind = , XF86AudioPlay, exec, playerctl play-pause`.
Turn it off with `MUZX_MPRIS=0`.

## Arduino LCD

1. Wire the I2C LCD to the Uno: GND, 5V, SDA to A4, SCL to A5.
2. Install the Arduino library **LiquidCrystal I2C**, open `muzx_lcd.ino`
   (set `0x27` or `0x3F` and the 16x2 / 20x4 size), and upload it.
3. Let your user open the port: `sudo usermod -aG uucp $USER`, then log in again.
4. Start MUZX and press `T`.

`lcdtest.cpp` checks the Arduino without MUZX. Environment variables:
`MUZX_LCD=1` (on at start), `MUZX_LCD_PORT=/dev/ttyUSB0`, `MUZX_LCD_KANA=1`
(katakana for LCDs with the Japanese ROM).

## Environment variables

| Variable                                                  | Meaning                          |
| --------------------------------------------------------- | -------------------------------- |
| `MUZX_MOUSE=0`                                            | turn the mouse off               |
| `MUZX_MPRIS=0`                                            | turn desktop media keys off      |
| `MUZX_LYRICS_ONLINE=0`                                    | no online lyrics lookup          |
| `MUZX_WHISPER`, `MUZX_WHISPER_MODEL`, `MUZX_WHISPER_LANG` | Whisper command, model, language |
| `MUZX_LCD`, `MUZX_LCD_PORT`, `MUZX_LCD_KANA`              | LCD options (see above)          |

## Files

`~/.config/muzx/` holds `queue.txt`, `stats.txt`, `settings.conf`,
`lyrics-offsets.txt` and the `lyrics/` cache.

## Troubleshooting

- **Media keys do nothing:** on i3/Sway/Hyprland install `playerctl` and bind
  the keys. If a browser is playing, use `playerctl -p muzx ...`.
- **No lyrics generated:** run `pipx install whisper-ctranslate2`; Settings
  shows "(Whisper not installed)" until it is found.
- **LCD stays blank:** try address `0x3F`, turn the contrast knob, close the
  Arduino serial monitor, and check port permissions.
- **Font size fails:** add `allow_remote_control yes` to `kitty.conf`.
