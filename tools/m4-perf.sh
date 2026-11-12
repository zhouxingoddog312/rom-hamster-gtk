#!/bin/bash
# M4 性能基线：10k 夹具 + 本机实测。只用于人工记录，不接入 CI。
set -u
cd "$(dirname "$0")/.." || exit 1

ROOT="$HOME/.cache/romham-m4/perf"
RUNS="$ROOT/runs"
BIN="./build-gui/rom-hamster-gui"
CONFIG="$ROOT/config"

[ -x "$BIN" ] || { echo "perf: build-gui missing"; exit 1; }

bash tools/m4-perf-fixture.sh >"$ROOT/fixture-run.log" 2>&1 || {
  cat "$ROOT/fixture-run.log"; exit 1;
}
rm -rf "$RUNS"
mkdir -p "$RUNS"

run_three() {
  local name="$1"; shift
  for i in 1 2 3; do
    local log="$RUNS/${name}-${i}.log"
    timeout 20 env XDG_CONFIG_HOME="$CONFIG" RH_PERF=1 "$@" \
      xvfb-run -a "$BIN" >"$log" 2>&1
    # 保留退出码为 124 的超时日志，报告里显式记录。
    echo "run $name-$i rc=$?" >>"$RUNS/rc.log"
  done
}

run_three startup
run_three search RH_PERF_SEARCH=99999
run_three detail RH_PERF_DETAIL=1
run_three scroll RH_PERF_SCROLL=1

python3 tools/m4-perf-report.py "$ROOT" "$RUNS"
