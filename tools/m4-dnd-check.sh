#!/bin/bash
# M4 真实指针拖拽验收：
# 独立 GTK 源窗口模拟文件管理器，用 xdotool 按住源窗口拖动到主窗口释放，
# 走真实的 X11 指针事件与 GtkDropTarget，而不是合成 GdkFileList。
set -u
cd "$(dirname "$0")/.." || exit 1

if [ -z "${RH_DND_IN_XVFB:-}" ]; then
  if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "SKIP real pointer drag: xvfb-run not installed"
    exit 0
  fi
  exec xvfb-run -a -s "-screen 0 2400x1600x24" env RH_DND_IN_XVFB=1 "$0" "$@"
fi

XDOTOOL=${XDOTOOL:-$(command -v xdotool || true)}
if [ -z "$XDOTOOL" ] || [ ! -x "$XDOTOOL" ]; then
  echo "SKIP real pointer drag: xdotool not found (set XDOTOOL=/path/to/xdotool)"
  exit 0
fi
command -v xwininfo >/dev/null 2>&1 || {
  echo "SKIP real pointer drag: xwininfo not found"
  exit 0
}
TWM=${TWM:-$(command -v twm || true)}
if [ -z "$TWM" ] || [ ! -x "$TWM" ]; then
  echo "SKIP real pointer drag: window manager twm not found (set TWM=/path/to/twm)"
  exit 0
fi

ROOT="$HOME/.cache/romham-m4/dnd-check"
BIN=./build-gui/rom-hamster-gui
FAILED=0
pid=""
wm_pid=""
cleanup() {
  [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
  [ -n "$wm_pid" ] && kill "$wm_pid" 2>/dev/null || true
}
trap cleanup EXIT
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

rm -rf "$ROOT"; mkdir -p "$ROOT/lib" "$ROOT/cfg" "$ROOT/in"
for i in $(seq 1 10); do
  printf -v n '%02d' "$i"
  dd if=/dev/zero of="$ROOT/in/D$n.nes" bs=1024 count=32 status=none
done

"$TWM" >"$ROOT/wm.log" 2>&1 &
wm_pid=$!
sleep 0.5

env GDK_BACKEND=x11 WAYLAND_DISPLAY= \
  XDG_CONFIG_HOME="$ROOT/cfg" XDG_CACHE_HOME="$ROOT/cache" \
  RH_PLATFORMS="$PWD/data/platforms.conf" RH_SMOKE_TEST=1 \
  RH_SMOKE_ROOT="$ROOT/lib" \
  RH_SMOKE_DND_PATHS="$ROOT/in" \
  "$BIN" >"$ROOT/dnd.log" 2>&1 &
pid=$!

for i in $(seq 1 50); do
  grep -q "smoke: dnd source" "$ROOT/dnd.log" && break
  sleep 0.1
done
line=$(grep "smoke: dnd source" "$ROOT/dnd.log" | tail -1 || true)
if [ -z "$line" ]; then
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  bad "dnd test source did not report coordinates"
  echo "M4 DND: $FAILED FAILURE(S)"
  exit "$FAILED"
fi
sx=$(printf '%s\n' "$line" | sed -n 's/.*source x=\([0-9.]*\) y=\([0-9.]*\).*/\1/p')
sy=$(printf '%s\n' "$line" | sed -n 's/.*source x=\([0-9.]*\) y=\([0-9.]*\).*/\2/p')
tx=$(printf '%s\n' "$line" | sed -n 's/.*target x=\([0-9.]*\) y=\([0-9.]*\).*/\1/p')
ty=$(printf '%s\n' "$line" | sed -n 's/.*target x=\([0-9.]*\) y=\([0-9.]*\).*/\2/p')
[ -n "$sx" ] && [ -n "$sy" ] && [ -n "$tx" ] && [ -n "$ty" ] || {
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  bad "cannot parse dnd coordinates: $line"
  echo "M4 DND: $FAILED FAILURE(S)"
  exit "$FAILED"
}

mainwin=$(xwininfo -root -tree | awk '/"rom-hamster"/ {print $1; exit}')
srcwin=$(xwininfo -root -tree | awk '/"rh-dnd-source"/ {print $1; exit}')
[ -n "$mainwin" ] && [ -n "$srcwin" ] || {
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  bad "cannot find app windows (main=$mainwin source=$srcwin)"
  echo "M4 DND: $FAILED FAILURE(S)"
  exit "$FAILED"
}

"$XDOTOOL" windowmove "$srcwin" 2000 0
sleep 0.2
src_abs_x=$(xwininfo -id "$srcwin" | awk '/Absolute upper-left X:/ {print $4}')
src_abs_y=$(xwininfo -id "$srcwin" | awk '/Absolute upper-left Y:/ {print $4}')
main_abs_x=$(xwininfo -id "$mainwin" | awk '/Absolute upper-left X:/ {print $4}')
main_abs_y=$(xwininfo -id "$mainwin" | awk '/Absolute upper-left Y:/ {print $4}')
sx=$((src_abs_x + sx))
sy=$((src_abs_y + sy))
tx=$((main_abs_x + tx))
ty=$((main_abs_y + ty))
echo "real pointer drag: source=$sx,$sy target=$tx,$ty main=$mainwin source_win=$srcwin"

"$XDOTOOL" mousemove --sync "$sx" "$sy"
"$XDOTOOL" mousedown 1
sleep 0.2
steps=12
for i in $(seq 1 "$steps"); do
  mx=$((sx + (tx - sx) * i / steps))
  my=$((sy + (ty - sy) * i / steps))
  "$XDOTOOL" mousemove --sync "$mx" "$my"
  sleep 0.05
done
sleep 0.3
"$XDOTOOL" mouseup 1

for i in $(seq 1 200); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.1
done
if kill -0 "$pid" 2>/dev/null; then
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  bad "app did not finish import after real pointer drop"
else
  wait "$pid" 2>/dev/null || true
fi

grep -E "smoke:" "$ROOT/dnd.log"
if grep -q "smoke: import cancelled=0 planned=10 imported=10" "$ROOT/dnd.log"; then
  ok "real pointer drop imports 10 files"
else
  bad "real pointer drop did not import 10 files"
fi
if grep -q "rows=10" "$ROOT/dnd.log"; then
  ok "real pointer drop refreshes list to 10 rows"
else
  bad "real pointer drop did not refresh list to 10 rows"
fi
[ -f "$ROOT/lib/romhamster.db" ] && ok "real pointer drop created/used store" || bad "real pointer drop store missing"

kill "$wm_pid" 2>/dev/null || true
wait "$wm_pid" 2>/dev/null || true
echo
if [ "$FAILED" -eq 0 ]; then echo "M4 DND: ALL PASS"; else echo "M4 DND: $FAILED FAILURE(S)"; fi
exit "$FAILED"
