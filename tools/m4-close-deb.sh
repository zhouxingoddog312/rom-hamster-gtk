#!/bin/bash
# M4 收口：在 Linux staging 重建三个 deb（避开 /mnt/c 的 drvfs 权限）。
set -u
cd "$(dirname "$0")/.." || exit 1
STAGE="$HOME/.cache/romham-m4/close-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/rom-hamster"
tar cf - \
  --exclude=build --exclude=build-nogui --exclude=build-asan \
  --exclude=build-gui --exclude=build-gui-asan \
  --exclude=.git --exclude=_research --exclude=dist --exclude=debian/tmp \
  . 2>/dev/null | tar xf - -C "$STAGE/rom-hamster"
find "$STAGE/rom-hamster" -type f -exec sed -i 's/\r//' {} +
find "$STAGE/rom-hamster" -type f -exec chmod 644 {} +
chmod 755 "$STAGE/rom-hamster/debian/rules"
(cd "$STAGE/rom-hamster" && dpkg-buildpackage -b -uc -us >"$STAGE/deb.log" 2>&1)
rc=$?
echo "dpkg-buildpackage rc=$rc"
tail -20 "$STAGE/deb.log"
ls -lh "$STAGE"/*.deb 2>/dev/null
exit "$rc"
