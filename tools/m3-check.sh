#!/bin/bash
# M3 验收：查询/详情/编辑/删除/导出/模板加载期报错 + M1/M2 回归入口。
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./build/rom-hamster
ROOT="$HOME/.cache/romham-m3/check"
FAILED=0
SKIP_DEB="${SKIP_DEB:-0}"

ok()   { echo "PASS  $*"; }
bad()  { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

section() { echo; echo "=== $*"; }

section "build"
ninja -C build >/dev/null 2>&1 && ok "ninja -C build" || bad "ninja -C build"

section "unit tests"
meson test -C build --print-errorlogs >/dev/null 2>&1 && ok "meson test (5 suites)" \
  || bad "meson test"

section "fixture"
bash tools/m3-fixture.sh >/dev/null 2>&1 && ok "fixture import + edit + media" \
  || { bad "fixture"; exit 1; }
export XDG_CONFIG_HOME="$ROOT/config"

section "query: 14 filters + 4 sort keys vs direct SQL"
python3 tools/m3-query-check.py >"$ROOT/query.log" 2>&1
if [ $? -eq 0 ]; then
  ok "query acceptance $(tail -1 "$ROOT/query.log")"
else
  bad "query acceptance"; cat "$ROOT/query.log"
fi

section "template: load-time file:line errors"
python3 tools/m3-template-check.py >"$ROOT/template.log" 2>&1
if [ $? -eq 0 ]; then
  ok "template acceptance $(tail -1 "$ROOT/template.log")"
else
  bad "template acceptance"; cat "$ROOT/template.log"
fi

section "export: ES / ES-DE / Pegasus + golden + media + m3u"
python3 tools/m3-export-check.py >"$ROOT/export.log" 2>&1
if [ $? -eq 0 ]; then
  ok "export acceptance $(tail -1 "$ROOT/export.log")"
else
  bad "export acceptance"; cat "$ROOT/export.log"
fi

section "stats"
STATS=$("$BIN" stats)
echo "$STATS" | grep -q "^games:       3$" && ok "stats games=3" || bad "stats games"
echo "$STATS" | grep -q "^roms:        5$" && ok "stats roms=5" || bad "stats roms"
echo "$STATS" | grep -q "^media files: 2$" && ok "stats media=2" || bad "stats media"

section "show"
"$BIN" show nes-c9a7d94a2f3f >"$ROOT/show.log" 2>&1 \
  && grep -q "^covers (1):" "$ROOT/show.log" && ok "show detail has covers" \
  || bad "show detail"
"$BIN" show no-such-id >/dev/null 2>&1 && bad "show missing id should fail" \
  || ok "show missing id fails"

section "delete guard"
"$BIN" delete nes-f5fc753a8b2f >/dev/null 2>&1
[ $? -eq 2 ] && ok "delete without --yes exits 2" || bad "delete without --yes"
"$BIN" delete nes-f5fc753a8b2f --yes --files >/dev/null 2>&1 \
  && ok "delete --yes --files" || bad "delete --yes --files"
[ ! -d "$ROOT/lib/nes/Gamma" ] && ok "game directory removed" || bad "game dir still there"
[ "$("$BIN" list | tail -1)" = "2 games" ] && ok "row removed" || bad "row still listed"

section "export refuses dest inside ROM_ROOT"
"$BIN" export "$ROOT/lib/out" --frontend data/frontends/es.conf >"$ROOT/inside.log" 2>&1
[ $? -ne 0 ] && grep -q "ROM_ROOT" "$ROOT/inside.log" \
  && ok "dest inside ROM_ROOT rejected" || bad "dest inside ROM_ROOT allowed"

section "compress is explicitly unsupported"
"$BIN" export "$ROOT/out/zip" --frontend data/frontends/es.conf --compress >"$ROOT/zip.log" 2>&1
[ $? -ne 0 ] && grep -qi "not implemented" "$ROOT/zip.log" \
  && ok "compress returns UNSUPPORTED" || bad "compress not rejected"

section "M1/M2 regression + deb"
SKIP_DEB="$SKIP_DEB" bash tools/m2-check.sh >"$ROOT/m2.log" 2>&1
if [ $? -eq 0 ]; then
  ok "m2-check (SKIP_DEB=$SKIP_DEB)"
else
  bad "m2-check"; tail -20 "$ROOT/m2.log"
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo "M3 CHECK: ALL PASS"
  exit 0
fi
echo "M3 CHECK: $FAILED FAILED"
exit 1
