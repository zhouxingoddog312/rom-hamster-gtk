#!/bin/bash
# M4 验收：首次运行 + 主窗口列表/搜索 + 详情页（元数据、8 组资源、缩略图、返回）。
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
if [ $? -eq 0 ]; then ok "meson test (5 suites incl config-save)"; else bad "meson test"; tail -20 "$ROOT/unit.log"; fi

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

echo "=== detail page: metadata + 8 groups + thumbnails + video + back"
mkdir -p "$LIB/nes/Alpha/videos"
: > "$LIB/nes/Alpha/videos/Alpha-video.mp4"
cp tests/fixtures/video/sample.ogv "$LIB/nes/Alpha/videos/Alpha-video.ogv"
VIDEO_SIZE=$(stat -c%s tests/fixtures/video/sample.ogv)
python3 - "$LIB/romhamster.db" "$VIDEO_SIZE" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
gid = c.execute("select id from games where name='Alpha'").fetchone()[0]
size = int(sys.argv[2])
c.execute("insert or replace into media(game_id,kind,filename,size,sort)"
          " values(?,?,?,?,?)", (gid, "videos", "Alpha-video.mp4", 0, 0))
c.execute("insert or replace into media(game_id,kind,filename,size,sort)"
          " values(?,?,?,?,?)", (gid, "videos", "Alpha-video.ogv", size, 1))
c.commit()
PY
DCFG="$ROOT/cfg-detail"
DCACHE="$ROOT/detail-cache"
rm -rf "$DCFG" "$DCACHE"
run_detail() {
  local log="$1"
  XDG_CONFIG_HOME="$DCFG" XDG_CACHE_HOME="$DCACHE" \
    RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$LIB" \
    RH_SMOKE_EXPECT_ROWS=3 RH_SMOKE_DETAIL=1 RH_SMOKE_BACK=1 \
    RH_SMOKE_VIDEO_TAB=1 RH_SMOKE_VIDEO_PLAY=1 \
    RH_PERF=1 \
    xvfb-run -a "$BIN" >"$log" 2>&1
}
run_detail "$ROOT/detail-cold.log"
grep -E "smoke:|perf: first_thumb" "$ROOT/detail-cold.log"
grep -q "detail pages=8" "$ROOT/detail-cold.log" \
  && ok "detail has 8 media groups" || bad "detail group count"
grep -Eq "thumbs=[1-9]/[1-9]" "$ROOT/detail-cold.log" \
  && ok "cover thumbnail decoded" || bad "thumbnail not decoded"
grep -q "videos=2" "$ROOT/detail-cold.log" \
  && ok "video widgets built (2 videos)" || bad "video widgets missing"
grep -q "video_ready=1" "$ROOT/detail-cold.log" \
  && ok "valid Ogg prepared by media backend" || bad "valid Ogg not prepared"
grep -q "video_playing=1" "$ROOT/detail-cold.log" \
  && ok "valid Ogg entered playing state" || bad "valid Ogg did not play"
grep -q "video_errors=1" "$ROOT/detail-cold.log" \
  && ok "invalid video degraded to external open" || bad "invalid video not degraded"
grep -q "back stack=list" "$ROOT/detail-cold.log" \
  && ok "back returns to list" || bad "back did not return"
grep -q "cache_hits=0" "$ROOT/detail-cold.log" \
  && ok "cold thumbnail cache miss" || bad "cold cache unexpectedly hit"
! grep -q "expected" "$ROOT/detail-cold.log" \
  && ok "detail smoke had no expectation mismatch" || bad "detail expectation mismatch"

run_detail "$ROOT/detail-warm.log"
grep -E "smoke:|perf: first_thumb" "$ROOT/detail-warm.log"
grep -Eq "cache_hits=[1-9]" "$ROOT/detail-warm.log" \
  && ok "warm thumbnail cache hit" || bad "warm cache did not hit"

echo "=== video backend unavailable: explicit degrade, no abort"
NOCFG="$ROOT/cfg-nobackend"
rm -rf "$NOCFG"
GST_PLUGIN_SYSTEM_PATH=/nonexistent GST_PLUGIN_SYSTEM_PATH_1_0=/nonexistent \
  GST_PLUGIN_PATH= GST_REGISTRY="$ROOT/empty-gst-registry" \
  XDG_CONFIG_HOME="$NOCFG" XDG_CACHE_HOME="$DCACHE" \
  RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$LIB" \
  RH_SMOKE_EXPECT_ROWS=3 RH_SMOKE_DETAIL=1 RH_SMOKE_BACK=1 \
  RH_SMOKE_VIDEO_TAB=1 \
  xvfb-run -a "$BIN" >"$ROOT/detail-nobackend.log" 2>&1
grep -E "smoke:" "$ROOT/detail-nobackend.log"
grep -q "video_ready=0" "$ROOT/detail-nobackend.log" \
  && ok "no backend: no inline stream prepared" || bad "no backend: stream unexpectedly prepared"
grep -q "video_playing=0" "$ROOT/detail-nobackend.log" \
  && ok "no backend: nothing played" || bad "no backend: unexpected playback"
grep -q "video_errors=2" "$ROOT/detail-nobackend.log" \
  && ok "no backend: all videos degraded" || bad "no backend: restore invalid video"
! grep -q "GStreamer-Play-ERROR" "$ROOT/detail-nobackend.log" \
  && ok "no backend: process did not abort" || bad "no backend: GStreamer abort"
CACHE_FILES=$(find "$DCACHE/rom-hamster/thumbs" -type f -name '*.png' 2>/dev/null | wc -l)
if [ "$CACHE_FILES" -ge 1 ]; then
  ok "thumbnail cache files: $CACHE_FILES"
else
  bad "thumbnail cache files missing"
fi
COLD_MS=$(grep -o "first_thumb_ms=[0-9.]*" "$ROOT/detail-cold.log" | head -1 | cut -d= -f2)
WARM_MS=$(grep -o "first_thumb_ms=[0-9.]*" "$ROOT/detail-warm.log" | head -1 | cut -d= -f2)
COLD_LOAD=$(grep -o "thumb_load_ms=[0-9.]*" "$ROOT/detail-cold.log" | head -1 | cut -d= -f2)
WARM_LOAD=$(grep -o "thumb_load_ms=[0-9.]*" "$ROOT/detail-warm.log" | head -1 | cut -d= -f2)
echo "first_thumb_ms cold=${COLD_MS:-?} warm=${WARM_MS:-?} (noisy UI); thumb_load_ms cold=${COLD_LOAD:-?} warm=${WARM_LOAD:-?}"

echo
if [ "$FAILED" -eq 0 ]; then
  echo "M4 GUI STEP2: ALL PASS"
  exit 0
fi
echo "M4 GUI STEP2: $FAILED FAILED"
exit 1
