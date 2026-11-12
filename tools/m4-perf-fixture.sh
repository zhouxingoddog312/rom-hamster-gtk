#!/bin/bash
# 生成 M4 性能验收夹具：10000 条游戏记录，每 10 条一个多碟，每 20 条三张截图。
# 列表/详情只读 SQLite 与文件，不经过 CLI 的逐条导入，避免夹具生成时间污染测速。
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m4/perf"
LIB="$ROOT/lib"
BIN=./build/rom-hamster

rm -rf "$ROOT"
mkdir -p "$ROOT/in" "$ROOT/config/rom-hamster" "$LIB"
printf 'ROM_ROOT=%s\n' "$LIB" > "$ROOT/config/rom-hamster/rom-hamster.conf"
cp data/platforms.conf "$ROOT/config/rom-hamster/platforms.conf"
printf x > "$ROOT/in/Perf Game 00001.nes"

export XDG_CONFIG_HOME="$ROOT/config"
"$BIN" import -y --platforms data/platforms.conf "$ROOT/in" >/dev/null 2>&1 || {
  echo "perf fixture: initial import failed"; exit 1;
}

python3 - "$LIB/romhamster.db" "$LIB" <<'PY'
import os, sqlite3, struct, sys, zlib

db, lib = sys.argv[1], sys.argv[2]

def chunk(tag, data):
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

def png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    out = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(out)

c = sqlite3.connect(db)
c.execute("delete from media")
c.execute("delete from roms")
c.execute("delete from games")

created = "2026-01-01T00:00:00Z"
genres = ["Action", "RPG", "Puzzle", "Shooter"]
games, roms, media = [], [], []
for i in range(1, 10001):
    name = "Perf Game %05d" % i
    gid = "perf-%05d" % i
    dpath = "nes/" + name
    multi = 1 if i % 10 == 0 else 0
    games.append((gid, "nes", name, None, dpath, "PerfDev", "PerfPub",
                  1980 + (i % 40), genres[i % len(genres)], 1,
                  (i % 100) / 100.0, "JP" if i % 2 else "US",
                  "Description for " + name, multi, 0, "raw", "good",
                  None, "", created, created))
    roms.append((gid, "Game.nes", 0, 1, "", "", "", 0))
    if multi:
        roms.append((gid, "Game (Disc 2).nes", 2, 1, "", "", "", 1))
    if i == 1:
        media.append((gid, "covers", "cover.png", 1060, 0))
    if i % 20 == 0:
        for k in range(1, 4):
            media.append((gid, "screenshots", "shot%d.png" % k, 67, k))

c.executemany(
    "insert into games(id,platform,name,game_main_name,dir_path,developer,publisher,"
    "year,genre,players,rating,region,description,is_multi_disc,is_arcade,"
    "rom_format,rom_status,dat_source,patch_type,created_at,updated_at) "
    "values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", games)
c.executemany(
    "insert into roms(game_id,filename,disc_no,size,crc32,md5,sha1,sort) "
    "values(?,?,?,?,?,?,?,?)", roms)
c.executemany(
    "insert into media(game_id,kind,filename,size,sort) values(?,?,?,?,?)", media)
c.commit()

# 只为第一条准备可解码的封面/截图；其余游戏的目录与文件也落盘，避免详情页
# 因测试数据缺文件而走“全部 missing”的假路径。
cover = os.path.join(lib, "nes", "Perf Game 00001", "covers", "cover.png")
os.makedirs(os.path.dirname(cover), exist_ok=True)
png(cover, 400, 400, (40, 120, 200))
for i in range(1, 10001):
    name = "Perf Game %05d" % i
    romdir = os.path.join(lib, "nes", name, "rom")
    os.makedirs(romdir, exist_ok=True)
    with open(os.path.join(romdir, "Game.nes"), "wb") as f:
        f.write(b"x")
    if i % 10 == 0:
        with open(os.path.join(romdir, "Game (Disc 2).nes"), "wb") as f:
            f.write(b"x")
    if i % 20 == 0:
        shotdir = os.path.join(lib, "nes", name, "screenshots")
        os.makedirs(shotdir, exist_ok=True)
        for k in range(1, 4):
            png(os.path.join(shotdir, "shot%d.png" % k), 1, 1, (200, 80, 40))

print("perf fixture: games=%d roms=%d media=%d" %
      (len(games), len(roms), len(media)))
PY

echo "perf fixture ready: $ROOT"
