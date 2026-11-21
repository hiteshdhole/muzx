#!/bin/bash
# End-to-end test of the muzxlcd program with a fake LCD, fake players, fake
# volume / notifications / sound and a fake lyrics server. Needs: python3 with
# dbus_next (pip install dbus-next), dbus-run-session, busctl.
# Run with:  make test-e2e     (it starts its own private session bus)
if [ -z "$DBUS_SESSION_BUS_ADDRESS" ] || [ -z "$MUZX_E2E_INNER" ]; then
  command -v dbus-run-session >/dev/null || { echo "SKIP: dbus-run-session not found"; exit 0; }
  python3 -c "import dbus_next" 2>/dev/null || { echo "SKIP: python module dbus_next missing (pip install dbus-next)"; exit 0; }
  MUZX_E2E_INNER=1 exec dbus-run-session -- bash "$0" "$@"
fi
HERE=$(cd "$(dirname "$0")" && pwd)
HUB=${MUZXLCD_BIN:-$HERE/../../muzxlcd}
[ -x "$HUB" ] || { echo "build the hub first: make muzxlcd"; exit 1; }
T=$(mktemp -d /tmp/muzxlcd_e2e_XXXXXX); mkdir -p "$T/home" "$T/audio"
export HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_CACHE_HOME=$T/home/.cache
export MUZXLCD_SOCKET=$T/ctl.sock FAKE_AUDIO_DIR=$T/audio MUZX_LCD_PORT=$T/port
export MUZX_LRCLIB_URL=http://127.0.0.1:18767 PATH=$HERE/fakebin:$PATH
echo 50 > "$T/audio/vol"
PIDS=""
cleanup() {
  "$HUB" ctl quit >/dev/null 2>&1; sleep 0.3
  for p in $PIDS; do kill "$p" 2>/dev/null; done
  pkill -f "$T" 2>/dev/null; rm -rf "$T"
}
trap cleanup EXIT
start() { "$@" & PIDS="$PIDS $!"; }
start python3 "$HERE/lrclib.py" "$T/server.log"
start python3 "$HERE/fakelcd.py" "$T"
start python3 "$HERE/fakenotify.py"
start python3 "$HERE/fakeplayer.py" fakespotify "Test Song" "Test Artist" "" Playing 0
sleep 1
"$HUB" 2> "$T/daemon.err" & PIDS="$PIDS $!"
ctl() { "$HUB" ctl "$@"; }
mark() { echo "--- $1" >> "$T/lcd.log"; }
notify() { busctl --user call org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications Notify susssasa\{sv\}i "$1" 0 "" "$2" "$3" 0 0 5000 >/dev/null; }
FAILED=0; CHECKED=0
# section NAME: the lines of lcd.log written after "--- NAME" up to the next mark
section() { awk -v n="--- $1" '$0==n{on=1;next} /^---/{on=0} on' "$T/lcd.log"; }
expect() { # expect SECTION REGEX DESCRIPTION
  CHECKED=$((CHECKED+1))
  if section "$1" | grep -Eq -- "$2"; then echo "  ok    $3"; else echo "  FAIL  $3   (section '$1', wanted /$2/)"; FAILED=$((FAILED+1)); fi
}
refute() {
  CHECKED=$((CHECKED+1))
  if section "$1" | grep -Eq -- "$2"; then echo "  FAIL  $3   (section '$1' has /$2/)"; FAILED=$((FAILED+1)); else echo "  ok    $3"; fi
}
check() { # check DESCRIPTION command...
  CHECKED=$((CHECKED+1)); local d=$1; shift
  if "$@"; then echo "  ok    $d"; else echo "  FAIL  $d"; FAILED=$((FAILED+1)); fi
}

mark lyrics;  sleep 5
expect lyrics "Second line|Third line|First line" "synced lyrics of the playing song (found on the fake lrclib)"
check "status line lists the modes and the popups" bash -c "'$HUB' ctl status | grep -q 'modes=lyrics,captions,nowplaying,spectrum,clock,status.*volume=1 notify=1'"

mark clock;      ctl mode clock >/dev/null;      sleep 2.5
expect clock "[0-9]{2}:[0-9]{2}:[0-9]{2}" "clock mode shows the time"
mark status;     ctl mode status >/dev/null;     sleep 3
expect status "CPU +[0-9]+% RAM +[0-9]+%" "PC status mode shows CPU and RAM"
mark nowplaying; ctl mode nowplaying >/dev/null; sleep 2.5
expect nowplaying "Test Song - Test Artist" "now playing mode shows title and artist"
mark button_next; printf 'B\n' > "$T/inject"; sleep 2.5
expect button_next "Mode 4/6.*Spectrum" "short button press goes to the next mode"
mark button_prev; printf 'L\n' > "$T/inject"; sleep 2.5
expect button_prev "Mode 3/6.*Now playing" "long button press goes to the previous mode"
check "an unknown mode is refused" bash -c "'$HUB' ctl mode foo | grep -qi 'unknown'"

ctl mode lyrics >/dev/null; sleep 2
mark volume;  echo 80 > "$T/audio/vol"; touch "$T/audio/fire"; sleep 1.3
expect volume "Volume 80%" "volume popup shows the new volume"
mark volume_gone; sleep 3
refute volume_gone "Volume" "volume popup goes away"
mark muted;   echo yes > "$T/audio/mute"; touch "$T/audio/fire"; sleep 1.3
expect muted "Volume muted" "volume popup shows muted"
sleep 2.5
mark note;    notify firefox "New message" "Hello from a friend"; sleep 1.5
expect note "New message: Hello from a friend" "desktop notification pops up"
sleep 6.5
mark note_muzx; notify MUZX "Own song" "Own artist"; sleep 1.5
refute note_muzx "Own song" "notifications from MUZX itself are skipped"
ctl set notify 0 >/dev/null
mark note_off; notify firefox "Hidden" "never shown"; sleep 1.5
refute note_off "Hidden" "notification popup can be turned off"
ctl set notify 1 >/dev/null

mark spectrum; ctl mode spectrum >/dev/null; sleep 4
check "spectrum bars at the right columns (200 Hz and 1 kHz tones)" python3 - "$T/lcd.log" <<'PY'
import ast, sys
rows = None
for line in open(sys.argv[1]):
    if not line.strip() or line.startswith('---'): continue
    try: s = ast.literal_eval(line.split(' ', 2)[2].strip())
    except Exception: continue
    if '\x01' in s and any('\x10' <= c <= '\x17' for c in s): rows = s.split('\x01')
if not rows: sys.exit(1)
bottom = rows[1]
lit = [i for i, c in enumerate(bottom) if '\x15' <= c <= '\x17']
ok = any(i in (3, 4) for i in lit) and 9 in lit and not any(i >= 13 for i in lit) and not any(i <= 1 for i in lit)
sys.exit(0 if ok else 1)
PY
check "the sound recorder runs while spectrum is shown" bash -c "pgrep -f 'python3.*fakebin/[p]arec' >/dev/null"
ctl mode clock >/dev/null; sleep 1.5
check "the sound recorder stops when spectrum is left" bash -c "! pgrep -f 'python3.*fakebin/[p]arec' >/dev/null"

check "set modes is accepted"      bash -c "'$HUB' ctl set modes lyrics,spectrum,clock | grep -q ok"
check "set modes needs a valid mode" bash -c "'$HUB' ctl set modes bogus | grep -qi 'at least one'"
check "status shows the new modes"  bash -c "'$HUB' ctl status | grep -q 'modes=lyrics,spectrum,clock '"
ctl set volume 0 >/dev/null
check "settings are saved in lcd.conf" bash -c "grep -q 'modes=lyrics,spectrum,clock' '$T/home/.config/muzx/lcd.conf' && grep -q 'volume_popup=0' '$T/home/.config/muzx/lcd.conf'"
mark volume_off; echo 20 > "$T/audio/vol"; touch "$T/audio/fire"; sleep 1.5
refute volume_off "Volume" "volume popup can be turned off"

ctl set modes lyrics,captions,nowplaying,spectrum,clock,status >/dev/null; ctl set volume 1 >/dev/null
start python3 "$HERE/fakeplayer.py" firefox.instance1 "Some Video" "" "https://www.youtube.com/watch?v=abcdefghijk" Playing 3.5
sleep 1.5
mark captions; ctl mode captions >/dev/null; sleep 5
expect captions "hello everyone|this is a caption & more|after a pause" "YouTube captions (fake yt-dlp)"

echo "$CHECKED checks, $FAILED failed"
[ "$FAILED" = 0 ]
