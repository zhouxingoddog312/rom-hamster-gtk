#!/bin/bash
# M4 启动游戏验收：emulators.conf 平台模板、CLI run、GUI Launch、无配置失败。
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./build-gui/rom-hamster
GUI=./build-gui/rom-hamster-gui
ROOT="$HOME/.cache/romham-m4/launch-check"
FAILED=0

ok() { echo "PASS  $1"; }
bad() { echo "FAIL  $1"; FAILED=$((FAILED + 1)); }

rm -rf "$ROOT"
mkdir -p "$ROOT/config/rom-hamster" "$ROOT/lib"

# 用 M3 夹具的库（Alpha / Beta / Gamma），不改原夹具。
if [ ! -f "$HOME/.cache/romham-m3/check/lib/romhamster.db" ]; then
  echo "SKIP  m3 fixture library missing" >&2
  exit 1
fi
cp -a "$HOME/.cache/romham-m3/check/lib/." "$ROOT/lib/"
printf 'ROM_ROOT=%s\nstore_path=%s\n' "$ROOT/lib" "$ROOT/lib/romhamster.db" \
  > "$ROOT/config/rom-hamster/rom-hamster.conf"
cat > "$ROOT/config/rom-hamster/emulators.conf" <<'EOF'
nes: /bin/sh -c 'printf "%s" "%ROM%" > "$LAUNCH_OUT"'
EOF

export XDG_CONFIG_HOME="$ROOT/config"
export LAUNCH_OUT="$ROOT/cli.out"
ALPHA=$($BIN list 2>/dev/null | awk -F'\t' '$2=="Alpha" {print $1; exit}')
BETA=$($BIN list 2>/dev/null | awk -F'\t' '$2=="Beta" {print $1; exit}')
[ -n "$ALPHA" ] && [ -n "$BETA" ] || { echo "SKIP  fixture ids missing" >&2; exit 1; }

rm -f "$LAUNCH_OUT"
"$BIN" run "$ALPHA" > "$ROOT/cli.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ] && grep -q "launched:" "$ROOT/cli.log"; then
  ok "CLI run exits 0"
else
  bad "CLI run failed rc=$rc: $(tail -2 "$ROOT/cli.log")"
fi
if [ -f "$LAUNCH_OUT" ] && grep -q "/nes/Alpha/rom/Alpha.nes" "$LAUNCH_OUT"; then
  ok "CLI run passed %ROM% to command"
else
  bad "CLI run did not pass ROM"
fi

# 多碟 Beta 应优先启动 m3u。
export LAUNCH_OUT="$ROOT/cli-beta.out"
rm -f "$LAUNCH_OUT"
"$BIN" run "$BETA" > "$ROOT/cli-beta.log" 2>&1
if [ -f "$LAUNCH_OUT" ] && grep -q "/nes/Beta/rom/Beta.m3u" "$LAUNCH_OUT"; then
  ok "multi-disc prefers m3u"
else
  bad "multi-disc did not prefer m3u"
fi

# GUI smoke：真实点击详情页 Launch 按钮。
export LAUNCH_OUT="$ROOT/gui.out"
rm -f "$LAUNCH_OUT"
RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$ROOT/lib" RH_SMOKE_EXPECT_ROWS=3 \
RH_SMOKE_DETAIL=1 RH_SMOKE_LAUNCH=1 \
  xvfb-run -a "$GUI" > "$ROOT/gui.log" 2>&1
grep -q "smoke: launch_status=Launched: Alpha.nes" "$ROOT/gui.log" \
  && ok "GUI Launch status shows ROM" || bad "GUI Launch status missing"
for i in 1 2 3 4 5; do [ -f "$LAUNCH_OUT" ] && break; sleep 0.2; done
if [ -f "$LAUNCH_OUT" ] && grep -q "/nes/Alpha/rom/Alpha.nes" "$LAUNCH_OUT"; then
  ok "GUI Launch passed %ROM% to command"
else
  bad "GUI Launch did not pass ROM"
fi

# 没有配置时必须失败，而不是静默启动一个不存在的程序。
export XDG_CONFIG_HOME="$ROOT/noconfig"
mkdir -p "$XDG_CONFIG_HOME/rom-hamster"
printf 'ROM_ROOT=%s\nstore_path=%s\n' "$ROOT/lib" "$ROOT/lib/romhamster.db" \
  > "$XDG_CONFIG_HOME/rom-hamster/rom-hamster.conf"
"$BIN" run "$ALPHA" > "$ROOT/none.log" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q "no emulator command for platform" "$ROOT/none.log"; then
  ok "missing emulator config fails loudly"
else
  bad "missing emulator config did not fail as expected rc=$rc"
fi

echo
if [ "$FAILED" -eq 0 ]; then
  echo "M4 LAUNCH: ALL PASS"
  exit 0
fi
echo "M4 LAUNCH: $FAILED FAILED"
exit 1
