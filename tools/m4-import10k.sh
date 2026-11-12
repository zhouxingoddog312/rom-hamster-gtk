#!/bin/bash
# M4 整库导入吞吐：10000 个单 ROM NES 平铺目录，CLI 两阶段导入，跑 3 轮。
# 测的是扫描 + 复制 + SQLite 写入整库耗时，不含 GUI 首帧绘制。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/import10k"
BIN=./build/rom-hamster
ROUNDS=${ROUNDS:-3}

rm -rf "$ROOT"
mkdir -p "$ROOT/in" "$ROOT/config/rom-hamster"
python3 - "$ROOT/in" <<'PY'
import os, sys
root = sys.argv[1]
for i in range(1, 10001):
    with open(os.path.join(root, "Perf ROM %05d.nes" % i), "wb") as f:
        f.write(b"x")
PY

for r in $(seq 1 "$ROUNDS"); do
  LIB="$ROOT/lib$r"
  mkdir -p "$LIB"
  /usr/bin/time -f "round=$r elapsed=%e maxrss_kb=%M" \
    -o "$ROOT/time$r.log" \
    env XDG_CONFIG_HOME="$ROOT/config" \
    "$BIN" import -y --root="$LIB" --platforms=data/platforms.conf "$ROOT/in" \
    >"$ROOT/import$r.log" 2>&1
  rc=$?
  python3 - "$LIB/romhamster.db" "$r" <<'PY'
import sqlite3, sys
db, r = sys.argv[1], sys.argv[2]
c = sqlite3.connect(db)
print("round=%s rc=see-time games=%d roms=%d" % (
    r, c.execute("select count(*) from games").fetchone()[0],
    c.execute("select count(*) from roms").fetchone()[0]))
PY
  echo "import_rc_$r=$rc"
  cat "$ROOT/time$r.log"
done

echo "raw logs: $ROOT"
