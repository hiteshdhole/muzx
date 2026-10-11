# MUZX

**A TUI (terminal) music player for Linux.**

Browse your music folders, play MP3 files, and control everything from the keyboard or mouse, right inside your terminal.

## Images

> Add screenshots here, for example:
>
> `![Folders tab](images/folders.png)`
> `![Playing a song](images/playing.png)`

## Features

- Terminal interface with 4 tabs: **Player**, **Folders**, **Queue**, **Search** (Player, Queue and Search are coming soon)
- Browse music folders and play `.mp3` files
- **Play All** option to start a folder from the first song
- Auto-plays the next song when one ends
- Progress bar with current time and total time
- Volume control
- Mouse support: click a tab to switch to it
- Remembers your music folder

## How to Install

### Requirements

- Linux
- A C++17 compiler (`g++` or `clang++`)
- [SFML](https://www.sfml-dev.org/) (audio and system modules)

Install SFML on Debian/Ubuntu:

```bash
sudo apt install g++ libsfml-dev
```

### Build

```bash
git clone https://github.com/hiteshdhole/muzx.git
cd muzx
g++ -std=c++17 main.cpp audio_player.cpp config.cpp library.cpp selection.cpp \
    -o player -lsfml-audio -lsfml-system
```

### Run

```bash
./player
```

## How to Use

On the first run, MUZX asks for your music folder. It must contain sub-folders with your songs. The path is saved, so you only enter it once.

### Folders screen

| Key       | Action                   |
| --------- | ------------------------ |
| `j` / `k` | Move down / up           |
| `Enter`   | Open the selected folder |
| `q`       | Quit                     |

### Song screen

| Key       | Action                               |
| --------- | ------------------------------------ |
| `j` / `k` | Move down / up                       |
| `Enter`   | Play the selected song (or Play All) |
| `p`       | Pause                                |
| `r`       | Resume                               |
| `x`       | Stop                                 |
| `n`       | Next song                            |
| `b`       | Previous song                        |
| `+` / `-` | Volume up / down                     |
| `q`       | Back to Folders                      |

### Tabs

| Key / Action    | What it does                              |
| --------------- | ----------------------------------------- |
| `Tab`           | Go to the next tab                        |
| `1` `2` `3` `4` | Jump to Player / Folders / Queue / Search |
| Mouse click     | Click a tab name to switch to it          |

## Project Structure

| File             | Purpose                                   |
| ---------------- | ----------------------------------------- |
| `main.cpp`       | App entry point, tabs, input, song screen |
| `audio_player.*` | Plays audio (play, pause, volume, etc.)   |
| `config.*`       | Saves and loads the music folder          |
| `library.*`      | Finds and lists music folders             |
| `selection.*`    | Cursor that moves up and down a list      |
| `queue.*`        | Song queue (in progress)                  |
| `playlist.*`     | Playlists (in progress)                   |

## Roadmap

- [ ] Player tab
- [ ] Queue tab
- [ ] Search tab
- [ ] Shuffle
- [ ] Playlists

## License

No license yet. Add one (for example MIT) if you want others to use your code.
