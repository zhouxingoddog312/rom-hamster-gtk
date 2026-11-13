#!/bin/bash
# M5：在 Xvfb 里启动真实 GUI，只抓主窗口（1000x700）的 XWD，再转成 AppStream 用的 PNG。
# 抓的是窗口而不是整个 root（1200x800），截图里不含空桌面。
set -u
cd "$(dirname "$0")/.." || exit 1
LIB="$HOME/.cache/romham-m3/check/lib"
OUT="$PWD/data/screenshots"
BIN="$PWD/build-gui/rom-hamster-gui"

# 同一个脚本在 xvfb-run 里再跑一遍：避免 bash -c '...' 的嵌套引号把窗口 id 解析写坏。
if [ "${1:-}" = "--inside-xvfb" ]; then
  "$BIN" &
  APP=$!
  sleep 2.5
  xwininfo -root -tree >"$OUT/windows.txt" 2>&1
  WID=$(grep -F '"rom-hamster":' "$OUT/windows.txt" | head -1 | awk '{print $1}')
  echo "window id: ${WID:-not found}"
  if [ -n "${WID:-}" ]; then
    xwd -silent -id "$WID" >"$OUT/main.xwd"
  fi
  kill "$APP" 2>/dev/null || true
  wait "$APP" 2>/dev/null || true
  exit 0
fi

mkdir -p "$OUT"
rm -f "$OUT/main.xwd" "$OUT/windows.txt" "$OUT/rom-hamster-main.png"
if [ ! -x "$BIN" ]; then
  echo "SKIP: $BIN missing (build build-gui first)"
  exit 1
fi
if [ ! -d "$LIB" ]; then
  bash tools/m3-fixture.sh || { echo "FAIL: fixture"; exit 1; }
fi

export LD_LIBRARY_PATH="$PWD/build-gui"
export RH_DATADIR="$PWD/data"
export RH_SYSCONFDIR="$PWD/etc/rom-hamster"
export XDG_CACHE_HOME="$HOME/.cache/romham-m4/screenshot-cache"
export XDG_CONFIG_HOME="$HOME/.cache/romham-m4/screenshot-cfg"
export GDK_BACKEND=x11
export WAYLAND_DISPLAY=
export GSK_RENDERER=cairo
export RH_SMOKE_TEST=1
export RH_SMOKE_ROOT="$LIB"
export RH_SMOKE_EXPECT_ROWS=3
export RH_SMOKE_HOLD_MS=9000
rm -rf "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME"
mkdir -p "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME"

xvfb-run -a -s "-screen 0 1200x800x24" "$0" --inside-xvfb
rc=$?

if [ ! -s "$OUT/main.xwd" ]; then
  echo "FAIL: window XWD not captured (xvfb rc=$rc)"
  exit 1
fi
python3 tools/xwd2png.py "$OUT/main.xwd" "$OUT/rom-hamster-main.png" || exit 1
ls -lh "$OUT/rom-hamster-main.png"
exit "$rc"
