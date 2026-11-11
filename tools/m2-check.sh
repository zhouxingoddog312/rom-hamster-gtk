#!/usr/bin/env bash
# M2 验收：导入（压缩包 / 单 ROM / 混合 / PSV / 二义 / 多碟 CD / DAT / 整库 / kill -9）
# 用法：bash tools/m2-check.sh            （在仓库根或任意目录执行）
#       SKIP_BUILD=1 bash tools/m2-check.sh
#       SKIP_DEB=1  bash tools/m2-check.sh   （deb 由 root 通道另行安装验证）
set -u
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
ok()   { PASS=$((PASS+1)); printf 'PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf 'FAIL  %s\n' "$1"; }
check(){ if [ "$1" = "0" ]; then ok "$2"; else bad "$2 ${3:-}"; fi; }

BIN=./build-nogui/rom-hamster
ROOT="$HOME/.cache/romham-m2"
rm -rf "$ROOT"
mkdir -p "$ROOT"

# ---------------------------------------------------------------- 构建与单测
if [ "${SKIP_BUILD:-0}" = "1" ]; then
  ok "构建（跳过）"
else
  meson setup build-nogui --reconfigure -Dgui=false >/dev/null 2>&1
  ninja -C build-nogui >"$ROOT/build.log" 2>&1
  check $? "构建通过" "(见 $ROOT/build.log)"
  grep -q 'warning:' "$ROOT/build.log" && bad "构建无 warning" || ok "构建无 warning"
fi

meson test -C build-nogui --suite unit >"$ROOT/test.log" 2>&1
check $? "单元测试（含 str-fs-hash）" "(见 $ROOT/test.log)"

# ---------------------------------------------------------------- 夹具
IN="$ROOT/in"
mkdir -p "$IN" "$ROOT/roms" "$ROOT/dat"
export XDG_CONFIG_HOME="$ROOT/config"
head -c 100 /dev/urandom > "$IN/cart.nes"
head -c 80 /dev/urandom > "$IN/cart2.nes"
head -c 10 /dev/urandom > "$IN/pic.png"
printf 'readme text' > "$IN/note.txt"
zip -q -j "$IN/single_nes.zip" "$IN/cart.nes"
7z a -bd -y "$IN/single_7z.7z" "$IN/cart2.nes" >/dev/null
zip -q -j "$IN/mixed.zip" "$IN/cart.nes" "$IN/pic.png" "$IN/note.txt"
zip -q -j "$IN/psv_game.vpk" "$IN/cart.nes"
head -c 30 /dev/urandom > "$IN/Update.pkg"
head -c 12 /dev/urandom > "$IN/a.bin"
head -c 12 /dev/urandom > "$IN/b.rom"
7z a -bd -y "$IN/mame_pack.7z" "$IN/a.bin" "$IN/b.rom" >/dev/null
head -c 64 /dev/urandom > "$IN/loose.nes"
printf 'GDI1' > "$IN/Quest (Disc 1).gdi"
printf 'GDI2' > "$IN/Quest (Disc 2).gdi"

gdi_crc=$(python3 -c 'import zlib,sys;print(format(zlib.crc32(open(sys.argv[1],"rb").read()),"08x"))' "$IN/Quest (Disc 1).gdi")
cat > "$ROOT/dat/dreamcast.dat" <<EOF
<?xml version="1.0"?>
<datafile>
  <header><name>M2 DAT</name></header>
  <game name="Quest (Disc 1)">
    <description>Quest Complete</description>
    <rom name="Quest (Disc 1).gdi" size="4" crc="$gdi_crc"/>
  </game>
</datafile>
EOF

$BIN init --root "$ROOT/roms" --dat-dir "$ROOT/dat" >/dev/null
check $? "rom-hamster init --root --dat-dir"
$BIN dat import dreamcast "$ROOT/dat/dreamcast.dat" >/dev/null
check $? "rom-hamster dat import"

# 只导入压缩包类夹具，单文件类分开导入
mkdir -p "$ROOT/pack"; cp "$IN/single_nes.zip" "$IN/single_7z.7z" "$IN/mixed.zip" \
  "$IN/psv_game.vpk" "$IN/Update.pkg" "$IN/mame_pack.7z" "$IN/Quest (Disc 1).gdi" "$IN/Quest (Disc 2).gdi" "$ROOT/pack"
$BIN import -y --platforms data/platforms.conf "$ROOT/pack" >"$ROOT/import.log" 2>&1
rc=$?
check $rc "压缩包/多碟导入命令退出码 0" "(见 $ROOT/import.log)"

R="$ROOT/roms"
[ -f "$R/nes/single_nes/rom/single_nes.zip" ] && ok "zip 单 ROM 包落到 nes/single_nes/rom" \
  || bad "zip 单 ROM 包落到 nes/single_nes/rom"
[ -f "$R/nes/single_7z/rom/single_7z.7z" ] && ok "7z 单 ROM 包落到 nes/single_7z/rom" \
  || bad "7z 单 ROM 包落到 nes/single_7z/rom"
[ -f "$R/nes/cart/rom/cart.nes" ] && ok "混合包中 ROM 解压落 rom/" \
  || bad "混合包中 ROM 解压落 rom/"
[ -f "$R/nes/cart/covers/pic.png" ] && ok "混合包封面落 covers/" \
  || bad "混合包封面落 covers/"
[ -f "$R/nes/cart/intro/note.txt" ] && ok "混合包说明落 intro/" \
  || bad "混合包说明落 intro/"
[ -f "$R/psvita/psv_game/rom/psv_game.vpk" ] && ok "PSV 特征包整体保留" \
  || bad "PSV 特征包整体保留"
[ -f "$R/mame/mame_pack/rom/mame_pack.7z" ] && ok "街机 .7z 直存 mame/rom" \
  || bad "街机 .7z 直存 mame/rom"
compgen -G "$R/*/Update/rom/Update.pkg" >/dev/null && ok "二义扩展名 .pkg 非交互取默认本体（平台取候选首位）" \
  || bad "二义扩展名 .pkg 非交互取默认本体（平台取候选首位）"
[ -f "$R/dreamcast/Quest/rom/Quest (Disc 1).gdi" ] \
  && ok "多碟 CD 第 1 碟落位" || bad "多碟 CD 第 1 碟落位"
[ -f "$R/dreamcast/Quest/rom/Quest (Disc 2).gdi" ] \
  && ok "多碟 CD 第 2 碟落位" || bad "多碟 CD 第 2 碟落位"
[ -f "$R/dreamcast/Quest/rom/Quest.m3u" ] && ok "多碟 m3u 生成" || bad "多碟 m3u 生成"

$BIN import -y --platforms data/platforms.conf "$IN/loose.nes" >"$ROOT/loose.log" 2>&1
check $? "单文件 ROM 导入退出码 0" "(见 $ROOT/loose.log)"
[ -f "$R/nes/loose/rom/loose.nes" ] && ok "单文件 ROM 落到 nes/loose/rom" \
  || bad "单文件 ROM 落到 nes/loose/rom"

# ---------------------------------------------------------------- DB 断言
python3 - "$R/romhamster.db" "$ROOT" <<'PY' >"$ROOT/db.log" 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
fail = 0
def ck(cond, name):
    global fail
    print(("PASS  " if cond else "FAIL  ") + name)
    if not cond: fail += 1

g = dict((r[0], r[1:]) for r in db.execute('select id,platform,name,game_main_name,is_multi_disc,rom_status,dat_source from games'))
ck(len(g) >= 6, '至少 6 条游戏记录')
disc = [r for r in db.execute("select filename,disc_no from roms where filename like 'Quest (Disc%'").fetchall()]
ck(len(disc) == 2 and all(d[1] > 0 for d in disc), '多碟记录 disc_no 均为正数')
m3u_rows = open(sys.argv[2] + '/roms/dreamcast/Quest/rom/Quest.m3u').read().strip().split('\n')
ck(m3u_rows == ['Quest (Disc 1).gdi', 'Quest (Disc 2).gdi'], 'm3u 顺序按碟号 1→2')
q = [v for v in g.values() if v[1] == 'Quest Complete']
ck(q, 'DAT 描述作为显示名（Quest Complete）')
ck(q and q[0][4] == 'good', 'DAT 命中后 rom_status=good')
ck(q and q[0][5] == 'M2 DAT', 'DAT 来源写入')
med = db.execute('select kind,filename from media').fetchall()
ck(('covers', 'pic.png') in med, '封面媒体入库')
ck(('intro', 'note.txt') in med, '说明媒体入库')
orphan = db.execute('select count(*) from games g where not exists (select 1 from roms r where r.game_id=g.id)').fetchone()[0]
ck(orphan == 0, '无“有游戏无 ROM”的半条记录')

# 8 个子目录齐备
import os, glob
for d in glob.glob(sys.argv[2] + '/roms/*/*/'):
    for sub in ('rom','covers','screenshots','videos','marquees','miximages','intro','patcher'):
        if not os.path.isdir(os.path.join(d, sub)):
            print('FAIL  缺子目录 %s/%s' % (d, sub)); fail += 1
            break
print('DBCHECK_FAIL=%d' % fail)
PY
grep -q 'DBCHECK_FAIL=0' "$ROOT/db.log" && ok "数据库断言（游戏/多碟/DAT/媒体/子目录）" \
  || bad "数据库断言（游戏/多碟/DAT/媒体/子目录）" "(见 $ROOT/db.log)"

if [ "${RAR_SKIP_NOTE:-1}" = "1" ]; then
  printf "SKIP  rar 压缩包（本机只有 unrar、无 rar 压缩器，无法构造夹具；libarchive 读取路径与 zip/7z 相同）\n"
fi

# ---------------------------------------------------------------- 整库导入
LIB="$ROOT/export"
mkdir -p "$LIB/megadrive"
head -c 64 /dev/urandom > "$LIB/megadrive/Sonic.gen"
printf '\x89PNG\r\n\x1a\n' > "$LIB/megadrive/Sonic.png"
cat > "$LIB/megadrive/gamelist.xml" <<'EOF'
<?xml version="1.0"?>
<gameList>
  <game>
    <path>./Sonic.gen</path>
    <name>Sonic the Hedgehog</name>
    <desc>Genesis classic</desc>
    <image>./Sonic.png</image>
    <developer>Sega</developer>
    <year>1991</year>
    <genre>Platform</genre>
  </game>
</gameList>
EOF
$BIN import -y --platforms data/platforms.conf "$LIB" >"$ROOT/lib.log" 2>&1
check $? "整库导入命令退出码 0" "(见 $ROOT/lib.log)"
[ -f "$R/megadrive/Sonic the Hedgehog/rom/Sonic the Hedgehog.gen" ] \
  && ok "整库：ROM 落到平台/标题目录" || bad "整库：ROM 落到平台/标题目录"
[ -f "$R/megadrive/Sonic the Hedgehog/covers/Sonic the Hedgehog-image.png" ] \
  && ok "整库：封面复制并改名" || bad "整库：封面复制并改名"
python3 - "$R/romhamster.db" <<'PY' >"$ROOT/libdb.log" 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
r = [x for x in db.execute("select name,developer,year,genre from games where platform='megadrive'").fetchall() if x[0] == 'Sonic the Hedgehog']
print('PASS' if r and r[0][1] == 'Sega' and r[0][2] == 1991 else 'FAIL', '整库元数据', r)
PY
grep -q 'PASS' "$ROOT/libdb.log" && ok "整库元数据（名称/厂商/年份）" || bad "整库元数据（名称/厂商/年份）"

# ---------------------------------------------------------------- kill -9 续做
RES="$ROOT/resume"
mkdir -p "$RES/in"
for i in $(seq -w 1 100); do head -c 4194304 /dev/urandom > "$RES/in/Game$i.nes"; done
$BIN init --root "$RES/roms" >/dev/null
XDG_CONFIG_HOME="$ROOT/config" $BIN import -y --platforms data/platforms.conf "$RES/in" >/dev/null 2>&1 &
pid=$!
sleep 0.2
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null
python3 - "$RES/roms/romhamster.db" <<'PY' >"$ROOT/resume1.log" 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
games = db.execute('select count(*) from games').fetchone()[0]
orphan = db.execute('select count(*) from games g where not exists (select 1 from roms r where r.game_id=g.id)').fetchone()[0]
print(games, orphan)
PY
read -r killed_games killed_orphan < "$ROOT/resume1.log"
check "$killed_orphan" "kill -9 后无半条记录（已入库 ${killed_games} 条）"

$BIN import -y --platforms data/platforms.conf "$RES/in" >/dev/null 2>&1
check $? "重启后重放导入成功"
python3 - "$RES/roms/romhamster.db" <<'PY' >"$ROOT/resume2.log" 2>&1
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
games = db.execute('select count(*) from games').fetchone()[0]
orphan = db.execute('select count(*) from games g where not exists (select 1 from roms r where r.game_id=g.id)').fetchone()[0]
dup = db.execute('select count(*) from (select game_id,filename,count(*) c from roms group by 1,2 having c>1)').fetchone()[0]
print(games, orphan, dup)
PY
read -r final_games final_orphan final_dup < "$ROOT/resume2.log"
[ "$final_games" = "100" ] && ok "续做后 100 条全部入库" || bad "续做后 100 条全部入库（实际 $final_games）"
[ "$final_orphan" = "0" ] && ok "续做后无半条记录" || bad "续做后无半条记录"
[ "$final_dup" = "0" ] && ok "续做后无重复 ROM 行" || bad "续做后无重复 ROM 行"

# ---------------------------------------------------------------- deb
if [ "${SKIP_DEB:-0}" = "1" ]; then
  ok "deb 构建（跳过）"
else
  # /mnt/c 是 drvfs（一律 0777），debhelper 会把 .install 当可执行脚本执行；
  # 必须在 Linux 侧的 staging 副本里规整权限与换行后再构建。
  STAGE="$ROOT/stage"
  rm -rf "$STAGE"
  mkdir -p "$STAGE/rom-hamster"
  tar cf - --exclude=build --exclude=build-nogui --exclude=build-asan \
          --exclude=.git --exclude=_research --exclude=dist \
          --exclude=debian/tmp . 2>/dev/null | tar xf - -C "$STAGE/rom-hamster"
  find "$STAGE/rom-hamster" -type f -exec sed -i 's/\r//' {} +
  find "$STAGE/rom-hamster" -type f -exec chmod 644 {} +
  chmod 755 "$STAGE/rom-hamster/debian/rules"
  (cd "$STAGE/rom-hamster" && dpkg-buildpackage -b -uc -us >"$ROOT/deb.log" 2>&1)
  check $? "dpkg-buildpackage 构建三 deb" "(见 $ROOT/deb.log)"
  ls "$STAGE"/*.deb >/dev/null 2>&1
  check $? "staging 下存在三 deb"
  if [ -d "$STAGE" ]; then
    mkdir -p dist && cp "$STAGE"/*.deb dist/ 2>/dev/null
  fi
fi

# ---------------------------------------------------------------- 汇总
printf '\n== M2: PASS %d / FAIL %d ==\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
