#!/bin/bash
# M4 第一步验收：首次运行（选目录→建库→主窗口）+ 主窗口列表/搜索。
# 真实 GtkFileDialog 在无显示器环境无法自动点选，这里用 RH_SMOKE_ROOT 走同一条
# open_library() 路径；文件选择器本身只做代码审查，不假装已测。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/gui"
BIN=./build-gui/rom-hamster-gui
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

rm -rf "$ROOT"
mkdir -p "$ROOT"

echo "=== build"
if [ ! -d build-gui ]; then
  meson setup build-gui -Dgui=true >/dev/null 2>&1
fi
ninja -C build-gui >"$ROOT/build.log" 2>&1
if [ $? -eq 0 ]; then ok "ninja -C build-gui"; else bad "ninja -C build-gui"; tail -20 "$ROOT/build.log"; fi

# 核心库不得因 GUI 反向依赖 GTK
if ldd build/libromham-1.0.so | grep -Eiq "gtk|gdk|pango"; then
  bad "libromham links GTK"
else
  ok "libromham has no GTK dependency"
fi

echo "=== unit tests"
meson test -C build --print-errorlogs >"$ROOT/unit.log" 2>&1
if [ $? -eq 0 ]; then ok "meson test (6 suites incl config-save)"; else bad "meson test"; tail -20 "$ROOT/unit.log"; fi

echo "=== fixture"
bash tools/m3-fixture.sh >/dev/null 2>&1
[ $? -eq 0 ] && ok "fixture (3 games)" || { bad "fixture"; exit 1; }
LIB="$HOME/.cache/romham-m3/check/lib"

run_gui() {
  local cfg="$1" root="$2" expect="$3" search="${4:-}" expect_search="${5:-}"
  rm -rf "$cfg"
  XDG_CONFIG_HOME="$cfg" RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$root" \
    RH_SMOKE_EXPECT_ROWS="$expect" \
    RH_SMOKE_SEARCH="$search" RH_SMOKE_EXPECT_SEARCH="$expect_search" \
    xvfb-run -a "$BIN" >"$ROOT/gui.log" 2>&1
  grep -E "smoke:" "$ROOT/gui.log"
  ! grep -q "expected" "$ROOT/gui.log"
}

echo "=== first run: empty folder -> db + main window"
EMPTY="$ROOT/empty-lib"
mkdir -p "$EMPTY"
run_gui "$ROOT/cfg-empty" "$EMPTY" 0
[ $? -eq 0 ] && ok "empty root maps window with 0 rows" || bad "empty root"
[ -f "$EMPTY/romhamster.db" ] && ok "store created" || bad "store not created"
[ "$(cat "$ROOT/cfg-empty/rom-hamster/rom-hamster.conf")" = "ROM_ROOT=$EMPTY" ] \
  && ok "ROM_ROOT persisted" || bad "ROM_ROOT not persisted"

echo "=== existing library: 3 rows"
run_gui "$ROOT/cfg-lib" "$LIB" 3
[ $? -eq 0 ] && ok "library maps window with 3 rows" || bad "library rows"

echo "=== search"
run_gui "$ROOT/cfg-search" "$LIB" 3 "Alpha" 1
[ $? -eq 0 ] && ok "search Alpha -> 1 row" || bad "search Alpha"
run_gui "$ROOT/cfg-search2" "$LIB" 3 "zzzz" 0
[ $? -eq 0 ] && ok "search zzzz -> 0 rows" || bad "search zzzz"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "M4 GUI STEP1: ALL PASS"
  exit 0
fi
echo "M4 GUI STEP1: $FAILED FAILED"
exit 1
