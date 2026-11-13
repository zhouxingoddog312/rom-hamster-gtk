#!/bin/bash
# 建 M3 验收夹具：3 个 NES 单碟，导入后写入元数据、媒体与一个双碟标记。
set -u
cd "$(dirname "$0")/.." || exit 1
BIN="${RH_BIN:-./build/rom-hamster}"
ROOT="$HOME/.cache/romham-m3/check"

rm -rf "$ROOT"
mkdir -p "$ROOT/in" "$ROOT/config/rom-hamster"
printf 'ROM_ROOT=%s/lib\nemulator_launch=retroarch -L core.so\ndat_dir=\n' "$ROOT" \
  > "$ROOT/config/rom-hamster/rom-hamster.conf"
cp data/platforms.conf "$ROOT/config/rom-hamster/platforms.conf"

printf alpha > "$ROOT/in/Alpha.nes"
printf beta  > "$ROOT/in/Beta.nes"
printf gamma > "$ROOT/in/Gamma.nes"

export XDG_CONFIG_HOME="$ROOT/config"
"$BIN" import -y --platforms data/platforms.conf "$ROOT/in" || exit 1

id_of() { "$BIN" list | awk -F'\t' -v n="$1" '$2==n {print $1; exit}'; }
A=$(id_of Alpha); B=$(id_of Beta); G=$(id_of Gamma)
[ -n "$A" ] && [ -n "$B" ] && [ -n "$G" ] || {
  echo "fixture: game id not found"; "$BIN" list; exit 1; }

"$BIN" edit "$A" --year 1990 --genre Action --developer DevA --publisher PubX \
  --region JP --status good --rating 0.85 --players 1 \
  --description "Alpha description" --tag classic --patch-type generic >/dev/null
"$BIN" edit "$B" --year 1995 --genre RPG --developer DevB --publisher PubY \
  --region US --status baddump --rating 0.50 --players 2 \
  --description "Beta description" --tag rpg >/dev/null
"$BIN" edit "$G" --year 1985 --genre Action --developer DevA --publisher PubY \
  --region EU --status good --rating 0.20 --players 1 \
  --description 'Gamma & <tag> "quote"' >/dev/null

# 媒体资产：Alpha 一张封面，Beta 一张截图。M3 CLI 没有 add-media 子命令，
# 这里按 library 的落位规则直接落文件并写库（等价于 M4 GUI 将调用的 store_media_add_sized）。
mkdir -p "$ROOT/lib/nes/Alpha/covers" "$ROOT/lib/nes/Beta/screenshots"
mkdir -p "$ROOT/lib/nes/Beta/rom"
printf two > "$ROOT/lib/nes/Beta/rom/Beta (Disc 2).nes"
# 真实可解码的 PNG（400x300 纯色），GUI 缩略图路径才有东西可测。
python3 - "$ROOT/lib/nes/Alpha/covers/Alpha-image.png" \
         "$ROOT/lib/nes/Beta/screenshots/Beta-thumb.png" <<'PY'
import struct, sys, zlib

def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

def png(path, rgb):
    w = h = 400
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    out = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(out)

png(sys.argv[1], (200, 40, 40))
png(sys.argv[2], (40, 80, 200))
PY
printf 'Beta.nes\nBeta (Disc 2).nes\n' > "$ROOT/lib/nes/Beta/rom/Beta.m3u"

# Beta 标成双碟并补一条 rom 行，用来验 --multi / --single / --sort discs。
python3 - "$ROOT/lib/romhamster.db" "$A" "$B" <<'PY'
import sqlite3, sys
db, a, b = sys.argv[1], sys.argv[2], sys.argv[3]
c = sqlite3.connect(db)
c.execute("insert or replace into media(game_id,kind,filename,size,sort) values(?,?,?,?,?)",
          (a, "covers", "Alpha-image.png", 9, 0))
c.execute("insert or replace into media(game_id,kind,filename,size,sort) values(?,?,?,?,?)",
          (b, "screenshots", "Beta-thumb.png", 8, 0))
c.execute("update games set is_multi_disc=1, rom_format='zip' where id=?", (b,))
c.execute("insert or replace into roms(game_id,filename,disc_no,size,crc32,md5,sha1,sort)"
          " values(?,?,?,?,?,?,?,?)",
          (b, "Beta.m3u", 0, 4, "", "", "", -1))
c.execute("insert or replace into roms(game_id,filename,disc_no,size,crc32,md5,sha1,sort)"
          " values(?,?,?,?,?,?,?,?)",
          (b, "Beta (Disc 2).nes", 2, 4, "", "", "", 1))
c.commit()
PY

echo "fixture ready: $ROOT"
"$BIN" list
