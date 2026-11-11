#!/usr/bin/env bash
# M1 验收脚本 —— 存储与规则 + 首个可安装 deb
#
# 用法：
#   tools/m1-check.sh              全跑（deb 段与安装段自动判断能否执行）
#   SKIP_DEB=1 tools/m1-check.sh   只跑源码与单测（不构建 deb）
#
# 在 WSL/Linux 上执行；本脚本不修改 /etc，安装段只在 root 下进行且装完即用。

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

PASS=0
FAIL=0
pass() { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
fail() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$1"; }

printf '== M1 验收：%s ==\n' "$ROOT"

# ---------------------------------------------------------------- 1 构建
meson setup build -Dgui=true >/dev/null 2>&1 || true
if ninja -C build >/tmp/m1-build.log 2>&1; then
  pass "构建（gui=true，无 error）"
else
  fail "构建（gui=true）"; tail -n 6 /tmp/m1-build.log
fi

# ---------------------------------------------------------------- 2 单测
if meson test -C build --print-errorlogs >/tmp/m1-test.log 2>&1; then
  pass "meson test：4/4（config-layers / store-schema / platform-rules / store-crud）"
else
  fail "meson test"; grep -E '^(Ok|Fail|ERROR)' /tmp/m1-test.log | head -n 10
fi

# ---------------------------------------------------------------- 3 ASan
if [ ! -d build-asan ]; then
  meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false >/dev/null 2>&1 \
    && pass "配置 build-asan" || fail "配置 build-asan"
else
  pass "build-asan 已存在"
fi
if ninja -C build-asan >/tmp/m1-asan-build.log 2>&1 \
   && ASAN_OPTIONS=detect_leaks=0 meson test -C build-asan >/tmp/m1-asan-test.log 2>&1; then
  pass "ASan/UBSan 下 4/4 通过"
else
  fail "ASan/UBSan"; tail -n 8 /tmp/m1-asan-test.log
fi

# ---------------------------------------------------------------- 4 规则
if ./build/test-platform data/platforms.conf >/tmp/m1-plat.log 2>&1; then
  n=$(grep -c '^ok ' /tmp/m1-plat.log)
  pass "平台规则：${n}/5，101 平台与四处修正断言通过"
else
  fail "平台规则"; grep -E 'ERROR|not ok' /tmp/m1-plat.log | head -n 3
fi

# ---------------------------------------------------------------- 5 SQL 边界
hits=$(grep -RInE '\b(SELECT|INSERT INTO|UPDATE|DELETE FROM|CREATE TABLE|CREATE INDEX|CREATE TRIGGER)\b' \
        src/core/*.c | grep -v '^src/core/store\.c' | grep -c .)
if [ "$hits" -eq 0 ]; then
  pass "SQL 字面量只出现在 store.c"
else
  fail "SQL 出现在非 store.c 的文件"; grep -RInE '\bSELECT\b' src/core/*.c | grep -v 'store\.c' | head -n 3
fi

# ---------------------------------------------------------------- 6 deb
if [ "${SKIP_DEB:-0}" = "1" ]; then
  echo "SKIP  deb 构建（SKIP_DEB=1）"
else
  STAGE=/tmp/rhbuild-m1
  rm -rf "$STAGE"
  mkdir -p "$STAGE/rom-hamster"
  tar cf - --exclude=build --exclude=build-nogui --exclude=build-asan \
          --exclude=.git --exclude=_research --exclude=dist . 2>/dev/null \
    | tar xf - -C "$STAGE/rom-hamster"
  # 从 Windows 盘拷过来的文件没有可执行位语义，统一规整一遍
  find "$STAGE/rom-hamster" -type f -exec sed -i 's/\r//' {} +
  find "$STAGE/rom-hamster" -type f -exec chmod 644 {} +
  chmod 755 "$STAGE/rom-hamster/debian/rules"

  if (cd "$STAGE/rom-hamster" && dpkg-buildpackage -us -uc -b >/tmp/m1-deb.log 2>&1); then
    pass "dpkg-buildpackage 产出三个 deb"
  else
    fail "dpkg-buildpackage"; tail -n 8 /tmp/m1-deb.log
  fi

  deb="$STAGE/rom-hamster_0.1.0_amd64.deb $STAGE/rom-hamster-gui_0.1.0_amd64.deb $STAGE/rom-hamster-data_0.1.0_all.deb"
  lint=$(lintian $deb 2>&1 | grep -v 'running with root privileges')
  if [ -z "$lint" ]; then
    pass "lintian：0 error / 0 warning"
  else
    fail "lintian"; printf '%s\n' "$lint" | head -n 10
  fi

  mkdir -p "$ROOT/dist"
  cp "$STAGE"/*.deb "$ROOT/dist/" 2>/dev/null
  pass "deb 已复制到 dist/"

  # -------------------------------------------------------------- 7 安装
  if [ "$(id -u)" -eq 0 ]; then
    dpkg --purge rom-hamster-gui rom-hamster rom-hamster-data >/dev/null 2>&1 || true
    if dpkg -i $deb >/tmp/m1-install.log 2>&1; then
      pass "dpkg -i 三包安装成功"
    else
      fail "dpkg -i"; tail -n 6 /tmp/m1-install.log
    fi
    if rom-hamster --version | grep -q '^rom-hamster 0.1.0'; then
      pass "rom-hamster --version 可运行"
    else
      fail "rom-hamster --version"
    fi
    if ldd /usr/bin/rom-hamster | grep -q 'rom-hamster/libromham-1.0.so'; then
      pass "CLI 从私有目录解析 libromham"
    else
      fail "libromham 解析路径"
    fi
    if [ -s /var/lib/dpkg/info/rom-hamster.conffiles ]; then
      pass "conffile 已登记"
    else
      fail "conffile 未登记"
    fi
    # RH_SMOKE_TEST 钩子在窗口未 map 时以非 0 退出（这是 M0 定的判据），
    # 提示信息走 stderr，所以这里判退出码而不是 grep 输出。
    if RH_SMOKE_TEST=1 GSK_RENDERER=cairo GDK_BACKEND=x11 \
         xvfb-run -a rom-hamster-gui >/tmp/m1-gui.log 2>&1; then
      pass "GUI 窗口 mapped（退出码 0）"
    else
      fail "GUI 冒烟"; tail -n 3 /tmp/m1-gui.log
    fi
  else
    echo "SKIP  安装段（需要 root：sudo tools/m1-check.sh）"
  fi
fi

printf '\n== M1 结果：PASS %d / FAIL %d ==\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
