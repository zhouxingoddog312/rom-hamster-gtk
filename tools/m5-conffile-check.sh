#!/bin/bash
# M5 验收：conffile 升级保留 + purge 无残留。
#
# 做法：用 dpkg 的隔离 root（--root，不需要系统 root 权限）装 0.1.0，
# 修改 /etc/rom-hamster/rom-hamster.conf，再装一个只有版本号不同的 0.1.1，
# 检查管理员修改逐字节保留；最后 purge 并检查没有文件残留。
#
# 前置：先跑 tools/m4-close-deb.sh 产出 0.1.0 的三个 deb。
# 依赖：dpkg、dpkg-buildpackage、meson、ninja、pkg-config 与构建依赖。
set -u
cd "$(dirname "$0")/.." || exit 1

STAGE="$HOME/.cache/romham-m4/close-stage"
WORK="$HOME/.cache/romham-m5"
R="$WORK/conffile-root"
SRC2="$WORK/src-0.1.1"
OUT2="$WORK/deb-0.1.1"
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

for f in rom-hamster_0.1.0_amd64.deb rom-hamster-gui_0.1.0_amd64.deb \
         rom-hamster-data_0.1.0_all.deb; do
  [ -f "$STAGE/$f" ] || { echo "missing $STAGE/$f (run tools/m4-close-deb.sh first)" >&2; exit 2; }
done

rm -rf "$R" "$SRC2" "$OUT2"
mkdir -p "$R/var/lib/dpkg/info" "$R/var/lib/dpkg/updates" \
         "$R/var/lib/dpkg/triggers" "$R/var/log" "$SRC2" "$OUT2"
: > "$R/var/lib/dpkg/status"

install_into_root() {
  local ver="$1"; shift
  dpkg --root="$R" --force-not-root --force-depends --force-confdef --force-confold \
       -i "$@" >"$WORK/install-$ver.log" 2>&1
  local rc=$?
  echo "--- dpkg -i ($ver) rc=$rc"
  tail -4 "$WORK/install-$ver.log"
  return $rc
}

echo "=== install 0.1.0"
install_into_root 0.1.0 "$STAGE"/rom-hamster-data_0.1.0_all.deb \
  "$STAGE"/rom-hamster_0.1.0_amd64.deb "$STAGE"/rom-hamster-gui_0.1.0_amd64.deb
[ $? -eq 0 ] && ok "dpkg -i 0.1.0" || bad "dpkg -i 0.1.0"

CONF="$R/etc/rom-hamster/rom-hamster.conf"
[ -f "$CONF" ] && ok "conffile installed" || bad "conffile missing"
printf '\n# admin-local-change\nduplicate_check_threshold = 42\n' >> "$CONF"
BEFORE=$(md5sum "$CONF" | cut -d' ' -f1)

echo "=== build 0.1.1 (same sources, only version bumped)"
tar cf - --exclude=build --exclude='build-*' --exclude='obj-*' --exclude=.git \
  --exclude=_research --exclude=dist --exclude=debian/tmp --exclude=debian/.debhelper \
  . 2>/dev/null | tar xf - -C "$SRC2"
find "$SRC2" -type f ! -name '*.png' ! -name '*.xwd' ! -name '*.ogv' \
  -exec sed -i 's/\r//' {} +
find "$SRC2" -type f -exec chmod 644 {} +
chmod 755 "$SRC2/debian/rules"
sed -i "s/version : '0.1.0'/version : '0.1.1'/" "$SRC2/meson.build"
{
  printf 'rom-hamster (0.1.1) unstable; urgency=medium\n\n'
  printf '  * Test build: conffile upgrade verification only.\n\n'
  printf ' -- Zhou Xin <zhouxin312@qq.com>  Sat, 01 Jan 2027 00:00:00 +0800\n\n'
  cat debian/changelog
} > "$SRC2/debian/changelog"
(cd "$SRC2" && dpkg-buildpackage -b -uc -us >"$WORK/build-0.1.1.log" 2>&1)
rc=$?
[ $rc -eq 0 ] && ok "build 0.1.1" || { bad "build 0.1.1"; tail -20 "$WORK/build-0.1.1.log"; }
cp "$SRC2"/../rom-hamster*_0.1.1_*.deb "$OUT2"/ 2>/dev/null

echo "=== upgrade to 0.1.1"
install_into_root 0.1.1 "$OUT2"/rom-hamster-data_0.1.1_all.deb \
  "$OUT2"/rom-hamster_0.1.1_amd64.deb "$OUT2"/rom-hamster-gui_0.1.1_amd64.deb
[ $? -eq 0 ] && ok "dpkg -i upgrade to 0.1.1" || bad "dpkg -i upgrade to 0.1.1"

AFTER=$(md5sum "$CONF" | cut -d' ' -f1)
[ "$BEFORE" = "$AFTER" ] && ok "admin conffile byte-identical after upgrade" \
  || bad "conffile changed on upgrade"
grep -q 'admin-local-change' "$CONF" && ok "admin comment preserved" || bad "admin comment lost"
grep -q '^duplicate_check_threshold = 42' "$CONF" && ok "admin value preserved" \
  || bad "admin value lost"
ls "$R/etc/rom-hamster/"*.dpkg-dist "$R/etc/rom-hamster/"*.dpkg-new >/dev/null 2>&1 \
  && bad "unexpected dpkg-dist/new artifact" || ok "no dpkg-dist/new artifact"
grep -q 'Version: 0.1.1' "$R/var/lib/dpkg/status" && ok "dpkg status updated to 0.1.1" \
  || bad "dpkg status not updated"

echo "=== purge"
dpkg --root="$R" --force-not-root -P rom-hamster-gui rom-hamster rom-hamster-data \
  >"$WORK/purge.log" 2>&1
rc=$?
[ $rc -eq 0 ] && ok "dpkg -P rc=0" || { bad "dpkg -P rc=$rc"; tail -10 "$WORK/purge.log"; }
dpkg --root="$R" --force-not-root -s rom-hamster >/dev/null 2>&1 \
  && bad "rom-hamster still registered after purge" || ok "rom-hamster deregistered"
leftover=$(find "$R" \( -name '*rom-hamster*' -o -name '*romham*' \) 2>/dev/null \
           | grep -v '^'"$R"'/var/lib/dpkg' | head -10)
[ -z "$leftover" ] && ok "no leftover installed files" \
  || { bad "leftover files"; echo "$leftover"; }

echo
if [ "$FAILED" -eq 0 ]; then echo "M5 CONFFILE: ALL PASS"; else echo "M5 CONFFILE: $FAILED FAILED"; fi
exit "$FAILED"
