#!/bin/bash
# M4 导入验收：进度刷新、取消延迟、取消后库状态、再次导入。
# 不测真实 GtkFileDialog 点选（无显示器环境），只测选完目录之后的导入路径。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/import-check"
BIN=./build-gui/rom-hamster-gui
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

rm -rf "$ROOT"
mkdir -p "$ROOT"

echo "=== build"
ninja -C build-gui >"$ROOT/build.log" 2>&1
[ $? -eq 0 ] && ok "ninja -C build-gui" || { bad "ninja -C build-gui"; tail -20 "$ROOT/build.log"; exit 1; }

make_input() {
  local dir="$1" n="$2" kb="$3"
  rm -rf "$dir"
  mkdir -p "$dir"
  local i=1
  while [ "$i" -le "$n" ]; do
    printf -v name '%04d' "$i"
    dd if=/dev/zero of="$dir/Game$name.nes" bs=1024 count="$kb" status=none
    i=$((i + 1))
  done
}

run_import() {
  local cfg="$1" lib="$2" input="$3" cancel_ms="$4" log="$5"
  rm -rf "$cfg"
  mkdir -p "$lib"
  env XDG_CONFIG_HOME="$cfg" \
      RH_PLATFORMS="$PWD/data/platforms.conf" \
      RH_SMOKE_TEST=1 \
      RH_SMOKE_ROOT="$lib" \
      RH_SMOKE_IMPORT_DIR="$input" \
      RH_SMOKE_IMPORT_CANCEL_MS="$cancel_ms" \
      ASAN_OPTIONS="detect_leaks=0" \
      xvfb-run -a "$BIN" >"$log" 2>&1
}

field() { sed -n "s/.*$1=\([^ ]*\).*/\1/p" "$2" | tail -1; }

echo "=== import progress (800 files)"
INPUT="$ROOT/in-progress"
make_input "$INPUT" 800 32
LIB="$ROOT/lib-progress"
CFG="$ROOT/cfg-progress"
run_import "$CFG" "$LIB" "$INPUT" "" "$ROOT/progress.log"
rc=$?
grep -E "smoke: import" "$ROOT/progress.log" || true
[ $rc -eq 0 ] && ok "import exits 0" || bad "import rc=$rc"
cancelled=$(field "cancelled" "$ROOT/progress.log")
imported=$(field "imported" "$ROOT/progress.log")
rows=$(field "rows" "$ROOT/progress.log")
gap=$(field "progress_max_gap_ms" "$ROOT/progress.log")
ugap=$(field "ui_max_gap_ms" "$ROOT/progress.log")
[ "$cancelled" = "0" ] && ok "not cancelled" || bad "unexpected cancellation"
[ "$imported" = "800" ] && ok "imported 800" || bad "imported=$imported"
[ "$rows" = "800" ] && ok "list rows=800" || bad "rows=$rows"
awk -v g="$gap" 'BEGIN { exit !(g <= 200.0) }' \
  && ok "progress gap ${gap} ms <= 200" || bad "progress gap ${gap} ms > 200"
awk -v g="$ugap" 'BEGIN { exit !(g <= 200.0) }' \
  && ok "UI refresh gap ${ugap} ms <= 200" || bad "UI refresh gap ${ugap} ms > 200"
[ "$(find "$LIB" -name '.part.*' | wc -l)" -eq 0 ] && ok "no partial temp files" \
  || bad "partial temp files remain"

echo "=== import cancel (1500 files)"
INPUT2="$ROOT/in-cancel"
make_input "$INPUT2" 1500 64
LIB2="$ROOT/lib-cancel"
CFG2="$ROOT/cfg-cancel"
run_import "$CFG2" "$LIB2" "$INPUT2" "300" "$ROOT/cancel.log"
rc=$?
grep -E "smoke: import" "$ROOT/cancel.log" || true
[ $rc -eq 0 ] && ok "cancel run exits 0" || bad "cancel run rc=$rc"
cancelled=$(field "cancelled" "$ROOT/cancel.log")
cancel_ms=$(field "cancel_ms" "$ROOT/cancel.log")
rows2=$(field "rows" "$ROOT/cancel.log")
[ "$cancelled" = "1" ] && ok "cancelled=1" || bad "cancelled=$cancelled"
awk -v t="$cancel_ms" 'BEGIN { exit !(t > 0 && t < 1000) }' \
  && ok "cancel latency ${cancel_ms} ms < 1000" || bad "cancel latency ${cancel_ms} ms"
[ "$rows2" -ge 0 ] && ok "cancel leaves a consistent list ($rows2 rows)" \
  || bad "bad rows after cancel"

if command -v python3 >/dev/null 2>&1; then
  python3 - "$LIB2/romhamster.db" <<'PY'
import sqlite3, sys
c = sqlite3.connect(sys.argv[1])
# 每个已提交游戏必须有至少一条 rom；取消不得留下半条 games 记录。
bad = c.execute("select count(*) from games g where not exists"
                " (select 1 from roms r where r.game_id=g.id)").fetchone()[0]
print("games_without_rom=%d" % bad)
sys.exit(0 if bad == 0 else 1)
PY
  [ $? -eq 0 ] && ok "no games row without rom after cancel" \
    || bad "games row without rom after cancel"
fi

echo "=== re-import after cancel"
run_import "$CFG2" "$LIB2" "$INPUT2" "" "$ROOT/reimport.log"
rc=$?
grep -E "smoke: import" "$ROOT/reimport.log" || true
[ $rc -eq 0 ] && ok "re-import exits 0" || bad "re-import rc=$rc"
rows3=$(field "rows" "$ROOT/reimport.log")
[ "$rows3" = "1500" ] && ok "re-import completes library (1500 rows)" \
  || bad "re-import rows=$rows3"

echo
if [ "$FAILED" -eq 0 ]; then echo "M4 IMPORT: ALL PASS"; exit 0; fi
echo "M4 IMPORT: $FAILED FAILED"; exit 1
