#!/bin/bash
# M4 收口：解包三个 deb 做 payload 与二进制验证；系统级 dpkg -i/-P 需要 root。
set -u
cd "$(dirname "$0")/.." || exit 1
STAGE="$HOME/.cache/romham-m4/close-stage"
PKG="$STAGE/pkgroot"
ROOT="$HOME/.cache/romham-m4/package-check"
LIB="$HOME/.cache/romham-m3/check/lib"
FAILED=0
ok()  { echo "PASS  $*"; }
bad() { echo "FAIL  $*"; FAILED=$((FAILED + 1)); }

rm -rf "$PKG" "$ROOT"
mkdir -p "$PKG" "$ROOT/cfg" "$ROOT/cache"
for f in "$STAGE"/rom-hamster_0.1.0_amd64.deb \
         "$STAGE"/rom-hamster-gui_0.1.0_amd64.deb \
         "$STAGE"/rom-hamster-data_0.1.0_all.deb; do
  [ -f "$f" ] || { bad "missing $(basename "$f")"; continue; }
  dpkg-deb -x "$f" "$PKG" || bad "extract $(basename "$f")"
done

emulators=$(find "$PKG" -name emulators.conf | wc -l)
[ "$emulators" -eq 0 ] && ok "no emulators.conf in deb payload" || bad "emulators.conf found in payload"
[ -x "$PKG/usr/bin/rom-hamster" ] && ok "CLI binary present" || bad "CLI binary missing"
[ -x "$PKG/usr/bin/rom-hamster-gui" ] && ok "GUI binary present" || bad "GUI binary missing"
[ -f "$PKG/usr/share/man/man1/rom-hamster.1.gz" ] && ok "man page present" || bad "man page missing"

LIBDIR=$(dirname $(find "$PKG/usr/lib" -name libromham-1.0.so | head -1))
DATADIR="$PKG/usr/share/rom-hamster"
SYSCONFDIR="$PKG/etc/rom-hamster"
env LD_LIBRARY_PATH="$LIBDIR" RH_DATADIR="$DATADIR" RH_SYSCONFDIR="$SYSCONFDIR" \
  "$PKG/usr/bin/rom-hamster" --version >"$ROOT/version.log" 2>&1
rc=$?
[ "$rc" -eq 0 ] && ok "packaged CLI --version rc=0" || bad "packaged CLI --version rc=$rc"
head -3 "$ROOT/version.log"

if [ -d "$LIB" ]; then
  env GDK_BACKEND=x11 WAYLAND_DISPLAY= \
    LD_LIBRARY_PATH="$LIBDIR" RH_DATADIR="$DATADIR" RH_SYSCONFDIR="$SYSCONFDIR" \
    XDG_CONFIG_HOME="$ROOT/cfg" XDG_CACHE_HOME="$ROOT/cache" \
    RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$LIB" RH_SMOKE_EXPECT_ROWS=3 RH_SMOKE_DETAIL=1 \
    timeout 20s xvfb-run -a "$PKG/usr/bin/rom-hamster-gui" >"$ROOT/gui.log" 2>&1
  rc=$?
  [ "$rc" -eq 0 ] && ok "packaged GUI smoke rc=0" || bad "packaged GUI smoke rc=$rc"
  grep -E "smoke: detail|thumbs=" "$ROOT/gui.log" | tail -2
  grep -q "detail pages=8" "$ROOT/gui.log" && ok "packaged GUI opens detail" || bad "packaged GUI detail failed"
else
  echo "SKIP packaged GUI: $LIB missing"
fi

if command -v lintian >/dev/null 2>&1; then
  lintian "$STAGE"/rom-hamster_0.1.0_amd64.deb \
    "$STAGE"/rom-hamster-gui_0.1.0_amd64.deb \
    "$STAGE"/rom-hamster-data_0.1.0_all.deb >"$ROOT/lintian.log" 2>&1
  echo "lintian rc=$? (full log: $ROOT/lintian.log)"
  grep -E "^E:|error" "$ROOT/lintian.log" | head -10
else
  echo "SKIP lintian: not installed"
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "SKIP dpkg -i / dpkg -P: current uid=1000, non-interactive sudo unavailable"
else
  dpkg -i "$STAGE"/rom-hamster_0.1.0_amd64.deb \
          "$STAGE"/rom-hamster-gui_0.1.0_amd64.deb \
          "$STAGE"/rom-hamster-data_0.1.0_all.deb && ok "dpkg -i three debs" || bad "dpkg -i failed"
fi

echo
if [ "$FAILED" -eq 0 ]; then echo "M4 PACKAGE: ALL PASS"; else echo "M4 PACKAGE: $FAILED FAILURE(S)"; fi
exit "$FAILED"
