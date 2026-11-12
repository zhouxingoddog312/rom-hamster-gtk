#!/bin/bash
# M4 拖拽导入验收：用 GdkFileList 合成 drop，不假装是真实指针拖拽事件。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/drop-check"
BIN=./build-gui/rom-hamster-gui
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

rm -rf "$ROOT"; mkdir -p "$ROOT/lib" "$ROOT/cfg" "$ROOT/in1" "$ROOT/in2"
for i in $(seq 1 60); do
  printf -v n '%03d' "$i"
  dd if=/dev/zero of="$ROOT/in1/A$n.nes" bs=1024 count=32 status=none
done
for i in $(seq 1 40); do
  printf -v n '%03d' "$i"
  dd if=/dev/zero of="$ROOT/in2/B$n.nes" bs=1024 count=32 status=none
done

env XDG_CONFIG_HOME="$ROOT/cfg" XDG_CACHE_HOME="$ROOT/cache" \
  RH_PLATFORMS="$PWD/data/platforms.conf" RH_SMOKE_TEST=1 \
  RH_SMOKE_ROOT="$ROOT/lib" \
  RH_SMOKE_DROP_PATHS="$ROOT/in1:$ROOT/in2" \
  xvfb-run -a "$BIN" >"$ROOT/drop.log" 2>&1
grep -E "smoke:" "$ROOT/drop.log"
if grep -q "smoke: import cancelled=0 planned=100 imported=100" "$ROOT/drop.log"; then
  ok "drop imports both folders (100 files)"
else
  bad "drop did not import 100 files"
fi
if grep -q "smoke: import progress_max_gap_ms=" "$ROOT/drop.log"; then
  ok "drop reuses progress reporting"
else
  bad "drop progress not reported"
fi
if grep -q "rows=100" "$ROOT/drop.log"; then
  ok "drop list refreshes to 100 rows"
else
  bad "drop list did not refresh to 100 rows"
fi
[ -f "$ROOT/lib/romhamster.db" ] && ok "drop created/used store" || bad "drop store missing"

echo
if [ "$FAILED" -eq 0 ]; then echo "M4 DROP: ALL PASS"; else echo "M4 DROP: $FAILED FAILURE(S)"; fi
exit "$FAILED"
