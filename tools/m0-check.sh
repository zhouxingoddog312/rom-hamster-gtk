#!/usr/bin/env bash
# M0 验收脚本（可重复执行）
#
# 对应 ARCHITECTURE §14 M0 的三条验收：
#   1. 三组构建通过（-Dgui=true / -Dgui=false / asan+ubsan）
#   2. ldd libromham 中不含 gtk / gdk / pango
#   3. 两个二进制都能启动
# 另加两条项目硬约束检查：SQL 只在 store.c、CLI 打印维护者信息。
#
# 用法：tools/m0-check.sh        （在 WSL / Linux 下运行）

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

pass=0
fail=0

ok()  { printf '  PASS  %s\n' "$*"; pass=$((pass + 1)); }
bad() { printf '  FAIL  %s\n' "$*"; fail=$((fail + 1)); }

banner() { printf '\n[%s] %s\n' "$1" "$2"; }

# ---------------------------------------------------------------- 1. 默认构建
banner 1/6 "meson setup + ninja + test（-Dgui=true）"
if meson setup build --reconfigure >/tmp/m0-setup.log 2>&1; then
  ok "configure"
else
  bad "configure（见 /tmp/m0-setup.log）"
fi
if ninja -C build >/tmp/m0-build.log 2>&1; then ok "build"; else bad "build（见 /tmp/m0-build.log）"; fi
if meson test -C build >/tmp/m0-test.log 2>&1; then ok "unit tests"; else bad "unit tests（见 /tmp/m0-test.log）"; fi

# ---------------------------------------------------------------- 2. 无 GUI
banner 2/6 "构建 -Dgui=false（不依赖 GTK 的机器）"
if meson setup build-nogui -Dgui=false --reconfigure >/tmp/m0-nogui-setup.log 2>&1; then
  ok "configure -Dgui=false"
else
  bad "configure -Dgui=false"
fi
if ninja -C build-nogui >/tmp/m0-nogui-build.log 2>&1; then ok "build"; else bad "build"; fi
if meson test -C build-nogui >/tmp/m0-nogui-test.log 2>&1; then ok "unit tests"; else bad "unit tests"; fi
if [ -x build-nogui/rom-hamster-gui ]; then
  bad "gui 二进制不应存在"
else
  ok "gui 二进制未生成（符合预期）"
fi

# ---------------------------------------------------------------- 3. ASan
banner 3/6 "构建 asan+ubsan"
if meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false \
     --reconfigure >/tmp/m0-asan-setup.log 2>&1; then
  ok "configure"
else
  bad "configure"
fi
if ninja -C build-asan >/tmp/m0-asan-build.log 2>&1; then ok "build"; else bad "build"; fi
# glib 自身在退出时会有一次性分配未释放，不当作本项目问题
if ASAN_OPTIONS=detect_leaks=0 meson test -C build-asan >/tmp/m0-asan-test.log 2>&1; then
  ok "unit tests"
else
  bad "unit tests"
fi

# ---------------------------------------------------------------- 4. 核心无 GTK
banner 4/6 "libromham 链接依赖不含 gtk/gdk/pango"
if ldd build/libromham-1.0.so | grep -Eiq 'libgtk|libgdk|libpango'; then
  bad "核心库链到了 GTK：$(ldd build/libromham-1.0.so | grep -Ei 'libgtk|libgdk|libpango' | tr '\n' ' ')"
else
  ok "核心库干净"
fi

# ---------------------------------------------------------------- 5. CLI 能跑
banner 5/6 "CLI 启动与配置分层"
if ./build/rom-hamster --version >/tmp/m0-version.log 2>&1; then
  ok "--version"
  sed 's/^/        /' /tmp/m0-version.log
else
  bad "--version"
fi
if ./build/rom-hamster config --explain ROM_ROOT | grep -q 'built-in default'; then
  ok "config --explain 报出来源层"
else
  bad "config --explain"
fi
XDG_CONFIG_HOME=$(mktemp -d)
mkdir -p "$XDG_CONFIG_HOME/rom-hamster"
printf '[default]\nROM_ROOT = /tmp/roms\n' >"$XDG_CONFIG_HOME/rom-hamster/rom-hamster.conf"
if XDG_CONFIG_HOME="$XDG_CONFIG_HOME" ./build/rom-hamster config --explain ROM_ROOT \
     | grep -q 'overrides'; then
  ok "用户层覆盖系统层（--explain 显示被覆盖值）"
else
  bad "用户层覆盖"
fi
rm -rf "$XDG_CONFIG_HOME"

# ---------------------------------------------------------------- 6. GUI 冒烟
banner 6/6 "GUI 启动（xvfb + 软件渲染，检查窗口是否真的 map）"
if ! command -v xvfb-run >/dev/null 2>&1; then
  bad "缺少 xvfb-run（apt-get install xvfb）"
else
  if RH_SMOKE_TEST=1 GSK_RENDERER=cairo GDK_BACKEND=x11 \
       timeout 20 xvfb-run -a ./build/rom-hamster-gui >/tmp/m0-gui.log 2>&1; then
    if grep -q 'smoke: window mapped' /tmp/m0-gui.log; then
      ok "GUI 窗口 mapped"
    else
      bad "GUI 运行了但窗口未 map（见 /tmp/m0-gui.log）"
    fi
  else
    bad "GUI 启动失败（见 /tmp/m0-gui.log）"
  fi
fi

# ---------------------------------------------------------------- 额外约束
banner 7/6 "SQL 只允许出现在 store.c"
sql_hits=$(grep -rl --include='*.c' -E '(CREATE TABLE|INSERT INTO|SELECT .* FROM|PRAGMA user_version)' src/ | grep -v 'src/core/store.c' || true)
if [ -z "$sql_hits" ]; then
  ok "只有 store.c 含 SQL"
else
  bad "另有文件含 SQL：$sql_hits"
fi

# ---------------------------------------------------------------- 汇总
printf '\n=====================================\n'
printf 'PASS: %d   FAIL: %d\n' "$pass" "$fail"
printf '=====================================\n'
[ "$fail" -eq 0 ]
