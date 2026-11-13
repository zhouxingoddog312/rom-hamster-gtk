#!/bin/bash
# CI 矩阵的唯一执行入口：本地和流水线跑同一组命令。
#
# 用法：
#   tools/ci-matrix.sh            # 依次跑 gui / nogui / asan 三组
#   tools/ci-matrix.sh gui        # 只跑某一组
#
# 依 ARCHITECTURE §8.4：至少 -Dgui=true、-Dgui=false、
# -Db_sanitize=address,undefined 三组构建；每组都执行单元测试。
# 另含 §6 的 SQL 归属检查与 gettext 目录/格式校验。
#
# 依赖（Ubuntu 24.04 / Debian 13）：
#   sudo apt-get install -y build-essential meson ninja-build pkg-config \
#     gettext libglib2.0-dev libsqlite3-dev libarchive-dev libxml2-dev \
#     zlib1g-dev libgtk-4-dev xvfb x11-utils python3
#
# 构建目录固定为 build-ci-*，不碰本地的 build/build-gui 等目录。
set -u

cd "$(dirname "$0")/.." || exit 1

JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
LOG_DIR="${CI_LOG_DIR:-${TMPDIR:-/tmp}}"
FAILED=0

ok()  { printf 'PASS  %s\n' "$*"; }
bad() { printf 'FAIL  %s\n' "$*"; FAILED=$((FAILED + 1)); }

need_tools() {
  local missing=() t
  for t in meson ninja pkg-config cc msgfmt xgettext python3; do
    command -v "$t" >/dev/null 2>&1 || missing+=("$t")
  done
  if [ ${#missing[@]} -gt 0 ]; then
    printf 'missing tools: %s\n' "${missing[*]}" >&2
    printf 'apt: build-essential meson ninja-build pkg-config gettext libglib2.0-dev libsqlite3-dev libarchive-dev libxml2-dev zlib1g-dev libgtk-4-dev xvfb\n' >&2
    exit 2
  fi
}

setup_build() {
  local dir="$1"; shift
  rm -rf "$dir"
  if ! meson setup "$dir" "$@" >"$LOG_DIR/rh-ci-$dir-setup.log" 2>&1; then
    bad "$dir setup"
    tail -30 "$LOG_DIR/rh-ci-$dir-setup.log"
    return 1
  fi
  if ! meson compile -C "$dir" -j "$JOBS" >"$LOG_DIR/rh-ci-$dir-build.log" 2>&1; then
    bad "$dir compile"
    tail -40 "$LOG_DIR/rh-ci-$dir-build.log"
    return 1
  fi
  ok "$dir compile"
  return 0
}

run_tests() {
  local dir="$1"; shift
  if ! "$@" meson test -C "$dir" --print-errorlogs >"$LOG_DIR/rh-ci-$dir-test.log" 2>&1; then
    bad "$dir tests"
    tail -40 "$LOG_DIR/rh-ci-$dir-test.log"
    return 1
  fi
  ok "$dir tests"
  return 0
}

sql_guard() {
  local hits
  hits=$(grep -rnE '\b(SELECT|INSERT INTO|UPDATE[[:space:]]|DELETE FROM|CREATE TABLE|ALTER TABLE|DROP TABLE)\b' \
           src --include='*.c' 2>/dev/null | grep -v '^src/core/store\.c:' || true)
  if [ -n "$hits" ]; then
    bad "SQL outside store.c"
    printf '%s\n' "$hits" | head -10
  else
    ok "SQL confined to store.c"
  fi
}

i18n_check() {
  local po
  for po in po/*.po; do
    [ -e "$po" ] || continue
    if msgfmt --check -o /dev/null "$po"; then
      ok "msgfmt --check $po"
    else
      bad "msgfmt --check $po"
    fi
  done
  if meson compile -C build-ci-gui rom-hamster-pot \
       >"$LOG_DIR/rh-ci-pot.log" 2>&1; then
    ok "xgettext pot extraction"
  else
    bad "xgettext pot extraction"
    tail -20 "$LOG_DIR/rh-ci-pot.log"
  fi
}

gui_smoke() {
  local lib="$HOME/.cache/romham-m3/check/lib"
  local cfg="$LOG_DIR/rh-ci-smoke-cfg"
  local log="$LOG_DIR/rh-ci-smoke.log"

  if ! command -v xvfb-run >/dev/null 2>&1; then
    bad "xvfb-run missing (GUI smoke is part of the matrix)"
    return
  fi
  if ! RH_BIN="$PWD/build-ci-gui/rom-hamster" bash tools/m3-fixture.sh >/dev/null 2>&1; then
    bad "fixture setup for gui smoke"
    return
  fi
  rm -rf "$cfg"
  if ! XDG_CONFIG_HOME="$cfg" RH_SMOKE_TEST=1 RH_SMOKE_ROOT="$lib" \
       RH_SMOKE_EXPECT_ROWS=3 xvfb-run -a build-ci-gui/rom-hamster-gui \
       >"$log" 2>&1; then
    bad "gui smoke exited non-zero"
    tail -20 "$log"
    return
  fi
  grep -E 'smoke:' "$log" || true
  if grep -q 'expected' "$log"; then
    bad "gui smoke expectation mismatch"
  else
    ok "gui smoke (3 rows)"
  fi
}

run_gui() {
  setup_build build-ci-gui -Dgui=true -Dtests=true || return
  if ldd build-ci-gui/libromham-1.0.so 2>/dev/null | grep -Eiq 'gtk|gdk|pango'; then
    bad "libromham links GTK"
  else
    ok "libromham has no GTK dependency"
  fi
  run_tests build-ci-gui
  i18n_check
  gui_smoke
}

run_nogui() {
  setup_build build-ci-nogui -Dgui=false -Dtests=true || return
  run_tests build-ci-nogui
}

run_asan() {
  setup_build build-ci-asan -Dgui=false -Dtests=true \
    -Db_sanitize=address,undefined -Db_lundef=false || return
  # GLib/GTest 在退出阶段有已知的一次性分配，泄漏检测会淹没真正的错误。
  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    run_tests build-ci-asan
}

need_tools
echo "=== matrix: $* (jobs=$JOBS)"

case "${1:-all}" in
  all)
    run_gui
    run_nogui
    run_asan
    ;;
  gui)   run_gui ;;
  nogui) run_nogui ;;
  asan)  run_asan ;;
  *)
    echo "usage: tools/ci-matrix.sh [all|gui|nogui|asan]" >&2
    exit 2
    ;;
esac

sql_guard

echo
if [ "$FAILED" -eq 0 ]; then
  echo "CI MATRIX: ALL PASS"
  exit 0
fi
echo "CI MATRIX: $FAILED FAILED"
exit 1
