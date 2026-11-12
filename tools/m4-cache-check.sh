#!/bin/bash
# M4 缩略图缓存上限验收：预置 5 个 1 MiB 旧缓存，上限 2 MiB；
# 打开详情页产生 1 次新写入，第一次写入后应按 mtime 淘汰旧文件到上限。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/cache-check"
BIN=./build-gui/rom-hamster-gui
LIB="$HOME/.cache/romham-m3/check/lib"
THUMBS="$ROOT/cache/rom-hamster/thumbs"
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

[ -d "$LIB" ] || {
  echo "SKIP cache cleanup: $LIB missing; run tools/m3-fixture.sh first"
  exit 0
}
rm -rf "$ROOT"
mkdir -p "$THUMBS" "$ROOT/cfg"
for i in 1 2 3 4 5; do
  dd if=/dev/zero of="$THUMBS/old0$i.png" bs=1M count=1 status=none
  touch -d "2026-01-0$i 00:00:00" "$THUMBS/old0$i.png"
done
before=$(du -sk "$THUMBS" | awk '{print $1}')
echo "cache before: files=$(find "$THUMBS" -name '*.png' | wc -l) size_kb=$before"

env GDK_BACKEND=x11 WAYLAND_DISPLAY= \
  XDG_CONFIG_HOME="$ROOT/cfg" XDG_CACHE_HOME="$ROOT/cache" \
  RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$LIB" RH_SMOKE_EXPECT_ROWS=3 RH_SMOKE_DETAIL=1 \
  RH_THUMB_CACHE_MB=2 \
  timeout 20s xvfb-run -a "$BIN" >"$ROOT/gui.log" 2>&1
rc=$?
echo "gui rc=$rc"
grep -E "smoke:|thumb" "$ROOT/gui.log" | tail -8

after=$(du -sk "$THUMBS" | awk '{print $1}')
files=$(find "$THUMBS" -name '*.png' | wc -l)
echo "cache after: files=$files size_kb=$after"
[ "$after" -le 2048 ] && ok "cache size <= 2 MiB after first write" || bad "cache size still over cap"
[ ! -e "$THUMBS/old01.png" ] && ok "oldest cache entries evicted" || bad "oldest cache entry not evicted"
[ -e "$THUMBS/old05.png" ] && ok "newest pre-existing entry kept" || bad "newest entry wrongly evicted"
new=$(find "$THUMBS" -name '*.png' ! -name 'old0*.png' | head -1)
[ -n "$new" ] && ok "new thumbnail written" || bad "new thumbnail missing"
grep -q "thumbs=1/1" "$ROOT/gui.log" && ok "detail thumbnail loaded" || bad "detail thumbnail not loaded"
[ "$rc" -eq 0 ] && ok "GUI exited cleanly" || bad "GUI exit rc=$rc"

echo
if [ "$FAILED" -eq 0 ]; then echo "M4 CACHE: ALL PASS"; else echo "M4 CACHE: $FAILED FAILURE(S)"; fi
exit "$FAILED"
