#!/usr/bin/env python3
"""muzx-lyrics-batch: download timed lyrics for a whole music folder.

For every song without lyrics it looks for timed lyrics online (lrclib.net, and
the optional `syncedlyrics` tool) and saves them as "Song.lrc" next to the song.
MUZX picks those files up automatically. Songs where nothing is found can
optionally be transcribed with Whisper (--whisper), one at a time.

Safe to stop (Ctrl+C) and run again: songs that already have a .lrc / .txt file
are skipped, and songs that were not found are not asked again for 3 days.

Examples:
    python3 muzx-lyrics-batch.py ~/Hitesh/Music
    python3 muzx-lyrics-batch.py ~/Hitesh/Music --whisper --model base
    python3 muzx-lyrics-batch.py ~/Hitesh/Music --dry-run
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

AUDIO_EXT = {".mp3", ".ogg", ".wav", ".flac", ".aiff", ".au", ".m4a", ".opus", ".aac"}
TLDS = {
    "com",
    "org",
    "net",
    "io",
    "in",
    "co",
    "app",
    "me",
    "fm",
    "to",
    "cc",
    "tv",
    "xyz",
    "info",
    "site",
    "online",
}
SYNC_TOLERANCE = 5.0  # seconds: an online version must be this close in length
MISS_MEMORY = 72 * 3600  # do not ask again for 3 days
UA = "muzx-lyrics-batch/1.0"
STAMP = re.compile(r"\[(\d+):(\d+(?:[.:]\d+)?)\]")


class Transient(Exception):
    """Network trouble: says nothing about whether the lyrics exist."""


# --------------------------------------------------------------------------
# Names, tags, LRC text
# --------------------------------------------------------------------------
def looks_like_domain(word):
    head, _, tld = word.lower().rpartition(".")
    return bool(head) and tld in TLDS and re.fullmatch(r"[a-z0-9.-]+", head) is not None


def clean_title(path):
    """'SpotiDownloader.com - Main Agar Saamne - Abhijeet.mp3' -> 'Main Agar Saamne - Abhijeet'"""
    stem = Path(path).stem
    s = re.sub(r"_?spotdown\.org", "", stem, flags=re.I)
    out, depth = [], 0
    for c in s:
        if c in "([{":
            depth += 1
        elif c in ")]}" and depth > 0:
            depth -= 1
        elif depth == 0:
            out.append(" " if c == "_" else c)
    words = [w for w in "".join(out).split() if not looks_like_domain(w)]
    text = " ".join(words)
    while text.startswith("- "):
        text = text[2:]
    while text.endswith(" -"):
        text = text[:-2]
    if text == "-":
        text = ""
    return text.strip() or stem


def split_dashes(title):
    return [p.strip() for p in title.split(" - ") if p.strip()]


def match_key(text):
    return "".join(c.lower() for c in text if c.isalnum())


def parse_lrc(text):
    """-> sorted list of (seconds, line) for lines that carry a time stamp."""
    lines = []
    for raw in text.replace("\r\n", "\n").split("\n"):
        stamps = STAMP.findall(raw)
        if not stamps:
            continue
        body = STAMP.sub("", raw).strip()
        for minutes, secs in stamps:
            lines.append((int(minutes) * 60 + float(secs.replace(":", ".")), body))
    lines.sort(key=lambda x: x[0])
    return lines


def probe(path):
    info = {"title": "", "artist": "", "album": "", "duration": 0.0}
    if not shutil.which("ffprobe"):
        return info
    try:
        run = subprocess.run(
            [
                "ffprobe",
                "-v",
                "error",
                "-show_entries",
                "format=duration:format_tags",
                "-of",
                "json",
                str(path),
            ],
            capture_output=True,
            text=True,
            timeout=30,
            stdin=subprocess.DEVNULL,
        )
        data = json.loads(run.stdout or "{}")
    except (OSError, ValueError, subprocess.SubprocessError):
        return info
    fmt = data.get("format") or {}
    tags = {str(k).lower(): str(v) for k, v in (fmt.get("tags") or {}).items()}
    for key in ("title", "artist", "album"):
        info[key] = tags.get(key, "").strip()
    try:
        info["duration"] = float(fmt.get("duration") or 0)
    except ValueError:
        pass
    return info


# --------------------------------------------------------------------------
# lrclib.net
# --------------------------------------------------------------------------
def lrclib_base():
    return (os.environ.get("MUZX_LRCLIB_URL") or "https://lrclib.net").rstrip("/")


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=8) as response:
            return response.read(4 * 1024 * 1024).decode("utf-8", "replace")
    except urllib.error.HTTPError as err:
        if err.code == 429 or err.code >= 500:
            raise Transient("server busy (%d)" % err.code)
        return ""  # 404 etc.: simply not there
    except (urllib.error.URLError, OSError, ValueError) as err:
        raise Transient(str(err))


def pick(body, duration, wanted_titles):
    """Best timed lyrics (text) and any plain lyrics from an lrclib answer."""
    try:
        data = json.loads(body)
    except ValueError:
        return None, None
    if isinstance(data, dict):
        data = [data]
    if not isinstance(data, list):
        return None, None
    keys = [k for k in (match_key(t) for t in wanted_titles) if len(k) >= 4]
    best, best_diff, plain = None, 1e18, None
    for obj in data:
        if not isinstance(obj, dict):
            continue
        if keys and "trackName" in obj:  # must be this song, not just this artist
            name = match_key(str(obj.get("trackName") or ""))
            if not any(len(name) >= 3 and (w in name or name in w) for w in keys):
                continue
        synced, plain_text = obj.get("syncedLyrics"), obj.get("plainLyrics")
        try:
            obj_dur = float(obj.get("duration") or 0)
        except (TypeError, ValueError):
            obj_dur = 0.0
        if isinstance(synced, str) and synced.strip():
            diff = abs(obj_dur - duration) if duration > 0 and obj_dur > 0 else 0.0
            if diff <= SYNC_TOLERANCE and diff < best_diff and parse_lrc(synced):
                best, best_diff = synced, diff
        if plain is None and isinstance(plain_text, str) and plain_text.strip():
            plain = plain_text
    return best, plain


def lookup_lrclib(info, path):
    """-> ('synced'|'plain'|None, text). Raises Transient on network trouble."""
    base = lrclib_base()
    cleaned = clean_title(path)
    title = info["title"] or cleaned
    parts = split_dashes(title)
    flat = " ".join(parts) or title
    wanted = [flat] + parts
    plain = None

    def consider(body, filtered=True):
        nonlocal plain
        synced, plain_text = pick(body, info["duration"], wanted if filtered else [])
        if synced:
            return synced
        if plain is None and plain_text:
            plain = plain_text
        return None

    q = urllib.parse.quote
    if info["title"] and info["artist"]:
        url = "%s/api/get?track_name=%s&artist_name=%s" % (
            base,
            q(info["title"]),
            q(info["artist"]),
        )
        if info["album"]:
            url += "&album_name=" + q(info["album"])
        if info["duration"] > 0:
            url += "&duration=%d" % round(info["duration"])
        found = consider(fetch(url), False)
        if found:
            return "synced", found
    if info["artist"]:
        found = consider(
            fetch(
                "%s/api/search?track_name=%s&artist_name=%s"
                % (base, q(title), q(info["artist"]))
            )
        )
        if found:
            return "synced", found
    queries = []
    for candidate in [(info["artist"] + " " + flat).strip(), flat] + (
        parts if len(parts) > 1 else []
    ):
        if candidate and candidate not in queries:
            queries.append(candidate)
    for query in queries[:3]:
        found = consider(fetch("%s/api/search?q=%s" % (base, q(query))))
        if found:
            return "synced", found
    return ("plain", plain) if plain else (None, None)


def lookup_syncedlyrics(info, path):
    """Second source: the optional `syncedlyrics` tool (pipx install syncedlyrics)."""
    binary = shutil.which("syncedlyrics")
    if not binary:
        return None
    title = info["title"] or clean_title(path)
    term = (info["artist"] + " " + " ".join(split_dashes(title) or [title])).strip()
    if not term:
        return None
    with tempfile.TemporaryDirectory(prefix="muzx-sl-") as tmp:
        out = os.path.join(tmp, "out.lrc")
        try:
            run = subprocess.run(
                [binary, "-o", out, "--", term],
                capture_output=True,
                timeout=25,
                stdin=subprocess.DEVNULL,
            )
        except (OSError, subprocess.SubprocessError):
            return None
        if run.returncode != 0 or not os.path.exists(out):
            return None
        text = Path(out).read_text(encoding="utf-8", errors="replace")
    lines = parse_lrc(text)
    if not lines:
        return None
    if (
        info["duration"] > 0 and lines[-1][0] > info["duration"] + 15
    ):  # a longer version
        return None
    return text


# --------------------------------------------------------------------------
# Whisper (optional, slow)
# --------------------------------------------------------------------------
def whisper_binary():
    custom = os.environ.get("MUZX_WHISPER")
    if custom:
        return shutil.which(custom)
    return shutil.which("whisper") or shutil.which("whisper-ctranslate2")


def to_lrc(lines):
    out = []
    for t, text in lines:
        cs = int(round(t * 100))
        out.append(
            "[%02d:%02d.%02d]%s" % (cs // 6000, (cs % 6000) // 100, cs % 100, text)
        )
    return "\n".join(out) + "\n"


def whisper_generate(path, binary, model, language):
    with tempfile.TemporaryDirectory(prefix="muzx-whisper-") as tmp:
        args = [
            binary,
            str(path),
            "--model",
            model,
            "--output_format",
            "json",
            "--output_dir",
            tmp,
            "--condition_on_previous_text",
            "False",
        ]
        if Path(binary).name == "whisper":
            args += ["--fp16", "False"]
        if language:
            args += ["--language", language]
        log_path = os.path.join(tmp, "log.txt")
        with open(log_path, "w") as log:
            run = subprocess.run(
                args,
                stdout=log,
                stderr=log,
                stdin=subprocess.DEVNULL,
                preexec_fn=lambda: os.nice(10),
                timeout=40 * 60,
            )
        if run.returncode != 0:
            tail = Path(log_path).read_text(errors="replace").strip().splitlines()[-1:]
            raise RuntimeError(tail[0][:150] if tail else "Whisper failed")
        lines, prev1, prev2 = [], None, None
        for name in os.listdir(tmp):
            if not name.endswith(".json"):
                continue
            data = json.loads(Path(tmp, name).read_text(encoding="utf-8"))
            for seg in data.get("segments", []):
                text = " ".join(str(seg.get("text", "")).split())
                stripped = re.sub(r"[♪♫♬]", "", text)
                if not any(c.isalnum() for c in stripped):
                    continue  # only notes / punctuation
                if text == prev1 == prev2:
                    continue  # stuck in a repetition loop
                prev2, prev1 = prev1, text
                lines.append((max(0.0, float(seg.get("start", 0))), text))
        if not lines:
            raise RuntimeError("no lyrics detected (instrumental?)")
        lines.sort(key=lambda x: x[0])
        return to_lrc(lines)


# --------------------------------------------------------------------------
# Files and state
# --------------------------------------------------------------------------
def has_lyrics_file(song):
    base = song.with_suffix("")
    return any(Path(str(base) + ext).exists() for ext in (".lrc", ".LRC", ".txt"))


CONTROL = re.compile(r"[\x00-\x08\x0b-\x1f\x7f-\x9f]")


def write_atomic(path, text):
    text = CONTROL.sub("", text)  # lyrics come from the internet: no terminal codes
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def config_dir():
    return Path(os.environ.get("HOME", ".")) / ".config" / "muzx"


def load_misses(path):
    try:
        return {k: float(v) for k, v in json.loads(path.read_text()).items()}
    except (OSError, ValueError, AttributeError):
        return {}


def find_songs(folder):
    songs = []
    for root, dirs, files in os.walk(folder):
        dirs.sort()
        for name in sorted(files):
            if Path(name).suffix.lower() in AUDIO_EXT:
                songs.append(Path(root) / name)
    return songs


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Download timed lyrics for a music folder."
    )
    ap.add_argument("folder", nargs="?", help="music folder (or set MUZX_MUSIC)")
    ap.add_argument(
        "--whisper",
        action="store_true",
        help="transcribe songs nothing was found for (slow)",
    )
    ap.add_argument(
        "--model",
        default=os.environ.get("MUZX_WHISPER_MODEL", "small"),
        help="Whisper model (default: small)",
    )
    ap.add_argument(
        "--language",
        default=os.environ.get("MUZX_WHISPER_LANG", ""),
        help="Whisper language, e.g. hi, ja, en",
    )
    ap.add_argument(
        "--plain", action="store_true", help="also save untimed lyrics as Song.txt"
    )
    ap.add_argument(
        "--retry",
        action="store_true",
        help="ask again for songs that were not found recently",
    )
    ap.add_argument(
        "--workers", type=int, default=4, help="parallel online lookups (default: 4)"
    )
    ap.add_argument(
        "--limit", type=int, default=0, help="stop after this many songs (for a test)"
    )
    ap.add_argument(
        "--dry-run", action="store_true", help="only count what would be done"
    )
    args = ap.parse_args()

    folder = args.folder or os.environ.get("MUZX_MUSIC")
    if not folder or not Path(folder).expanduser().is_dir():
        ap.error("music folder missing or not a folder")
    folder = Path(folder).expanduser().resolve()

    songs = find_songs(folder)
    cfg = config_dir()
    cfg.mkdir(parents=True, exist_ok=True)
    miss_file = cfg / "lyrics-batch-missing.json"
    misses = {} if args.retry else load_misses(miss_file)
    now = time.time()

    todo, have, recent_miss = [], 0, 0
    for song in songs:
        if has_lyrics_file(song):
            have += 1
        elif now - misses.get(str(song), 0) < MISS_MEMORY:
            recent_miss += 1
        else:
            todo.append(song)
    if args.limit:
        todo = todo[: args.limit]
    print(
        "%d songs: %d already have lyrics, %d not found recently (use --retry), %d to look up"
        % (len(songs), have, recent_miss, len(todo))
    )
    if args.dry_run or not todo:
        return 0

    stop = threading.Event()
    state = {"transient": 0}
    results = {}  # song -> 'found' | 'plain' | 'missing' | 'error'

    def work(song):
        if stop.is_set():
            return song, "skipped", ""
        info = probe(song)
        try:
            kind, text = lookup_lrclib(info, song)
            if kind != "synced":
                better = lookup_syncedlyrics(info, song)
                if better:
                    kind, text = "synced", better
        except Transient as err:
            return song, "error", str(err)
        if kind == "synced":
            write_atomic(
                song.with_suffix(".lrc"), text if text.endswith("\n") else text + "\n"
            )
            return song, "found", ""
        if kind == "plain" and args.plain:
            write_atomic(song.with_suffix(".txt"), text.strip() + "\n")
            return song, "plain", ""
        return song, "missing", ""

    print(
        "Looking up lyrics online (%d at a time)... Ctrl+C stops safely.\n"
        % args.workers
    )
    pool = ThreadPoolExecutor(max_workers=max(1, args.workers))
    futures = [pool.submit(work, s) for s in todo]
    done = 0
    try:
        for fut in as_completed(futures):
            song, status, note = fut.result()
            if status == "skipped":
                continue
            done += 1
            results[song] = status
            state["transient"] = state["transient"] + 1 if status == "error" else 0
            label = {
                "found": "FOUND  ",
                "plain": "plain  ",
                "missing": "missing",
                "error": "network",
            }[status]
            print(
                "[%*d/%d] %s %s%s"
                % (
                    len(str(len(todo))),
                    done,
                    len(todo),
                    label,
                    song.relative_to(folder),
                    ("  (%s)" % note) if note else "",
                )
            )
            if state["transient"] >= 8:
                print("\nThe network looks unreachable: stopping. Try again later.")
                break
    except KeyboardInterrupt:
        print("\nStopped. Run the command again to continue.")
    finally:
        stop.set()
        pool.shutdown(wait=False, cancel_futures=True)

    for song, status in results.items():
        if status == "missing":
            misses[str(song)] = time.time()
    miss_file.write_text(json.dumps(misses))
    found = sum(1 for s in results.values() if s == "found")
    missing = [
        s
        for s, st in results.items()
        if st in ("missing", "plain") and not (st == "plain" and args.plain)
    ]
    print("\nFound timed lyrics for %d of %d songs." % (found, len(results)))

    if args.whisper and missing:
        binary = whisper_binary()
        if not binary:
            print(
                "Whisper is not installed (pipx install whisper-ctranslate2): skipping generation."
            )
        else:
            print(
                "\nGenerating lyrics with Whisper (%s) for %d songs, one at a time..."
                % (args.model, len(missing))
            )
            started = time.time()
            for i, song in enumerate(missing, 1):
                eta = ""
                if i > 1:
                    left = (time.time() - started) / (i - 1) * (len(missing) - i + 1)
                    eta = "  ~%d min left" % (left / 60)
                print(
                    "[%d/%d] %s%s" % (i, len(missing), song.relative_to(folder), eta),
                    flush=True,
                )
                try:
                    text = whisper_generate(song, binary, args.model, args.language)
                    write_atomic(song.with_suffix(".lrc"), text)
                    misses.pop(str(song), None)
                    miss_file.write_text(json.dumps(misses))
                except KeyboardInterrupt:
                    print(
                        "\nStopped. Run the command with --whisper again to continue."
                    )
                    break
                except Exception as err:  # one bad song must not stop the rest
                    print("   failed: %s" % err)
    elif missing:
        report = cfg / "lyrics-batch-missing.txt"
        report.write_text("\n".join(str(s) for s in missing) + "\n")
        print("%d songs have no online lyrics. List: %s" % (len(missing), report))
        print(
            "Add --whisper to generate those with Whisper (slow), or press G in MUZX when playing them."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
