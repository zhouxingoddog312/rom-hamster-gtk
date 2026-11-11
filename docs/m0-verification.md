# M0 验收记录

执行时间：2026-11-11
执行环境：WSL2 / Ubuntu 26.04.1 LTS / x86_64 / gcc 15.2.0 / meson 1.10.1 / ninja 1.13.2
依赖实测版本：glib 2.88.0、sqlite 3.46.1、libarchive 3.8.5、libxml2 2.15.2、gtk4 4.22.4

一键复跑：`tools/m0-check.sh`（脚本内的每一步都可单独执行）

## 结果：PASS 16 / FAIL 0

| # | 验收项（ARCHITECTURE §14 M0） | 结果 | 证据 |
|---|---|---|---|
| 1 | `meson setup build` + `ninja` + `meson test` | PASS | `/tmp/m0-setup.log`、`/tmp/m0-build.log`、`/tmp/m0-test.log` |
| 2 | `-Dgui=false` 构建并跑测试，且不生成 GUI 二进制 | PASS | `build-nogui/rom-hamster-gui` 不存在 |
| 3 | `-Db_sanitize=address,undefined` 构建并跑测试 | PASS | `/tmp/m0-asan-test.log` |
| 4 | `ldd build/libromham-1.0.so` 中无 gtk/gdk/pango | PASS | 脚本第 4 段 |
| 5 | `rom-hamster --version` 可跑，维护者信息由 CLI 层打印 | PASS | 输出见下 |
| 6 | `config --explain` 报出层与行号，并显示被覆盖值 | PASS | 脚本第 5 段 |
| 7 | `rom-hamster-gui` 能起窗口且窗口真的 map | PASS | `RH_SMOKE_TEST=1` + xvfb + cairo 渲染 |
| 8 | 项目硬约束：SQL 只在 `store.c` | PASS | 脚本第 7 段 |

`--version` 实际输出：

```
rom-hamster 0.1.0
maintainer: Zhou Xin <zhouxin312@qq.com>
homepage: https://gitee.com/goddog312/rom-hamster-gtk
license: GPL-3.0-or-later
schema: v1
```

## 本阶段实际交付

- `meson.build` / `meson_options.txt`：`gui`、`tests` 两个开关；核心库与两个前端分开链接
- `src/core/rh-err.{h,c}`：错误码 + `RhErr`（消息字符串，不打印）
- `src/core/rh-config.{h,c}`：五层加载、逐键记录 `{value, layer, source_file, line}` 与被覆盖值
- `src/core/rh-ctx.{h,c}`：`RhCtx`；`rh_log`/`rh_progress` 在没有 ui 时静默丢弃
- `src/core/rh-ui.h`：11 回调 vtable（§6.1）
- `src/core/store.{h,c}` + `store-schema.h`：`store_open`/`store_close`/`store_migrate`、事务、`user_version` 检查、schema v1（7 表 + 索引 + FTS5 + 三个同步触发器）
- `src/cli/main.c` + `ui-cli.c`：`--version`、`config --explain|--list`；终端版 rh_ui
- `src/gui/main.c`：GTK4 空窗口 + `RH_SMOKE_TEST` 冒烟钩子
- `tests/unit/test-config.c`：分层覆盖、行号、忽略非 `[default]` 段、语法错误报文件名行号
- `tests/unit/test-store.c`：建库、重开幂等、只读打开不存在的库必须失败、事务嵌套拒绝
- `data/rom-hamster.conf.template`：含全部 14 个键的注释模板

## 与文档的偏差（如实记录）

1. **配置解析没有用 `GKeyFile`**。`GKeyFile` 不暴露行号，而 `config --explain` 必须报"文件:行"。改为在 `rh-config.c` 内实现 GKeyFile 的最小子集（`[default]` / `key = value` / `#`、`;` 注释 / 空行）。代价：不支持 GKeyFile 的转义与本地化键，这两项本项目也用不到。
2. **FTS5 的 trigram 是探测后决定**：`store_open` 时先建一个临时表试 `tokenize="trigram"`，失败则退回默认分词并把 `ctx->fts_trigram` 置 `FALSE`，检索层据此退化（§3.3 的要求，M3 才会真正用到）。
3. **GUI 冒烟需要软件渲染**：本机 WSL 无 GPU，默认 EGL 初始化失败会让 GTK 直接退出；脚本用 `GSK_RENDERER=cairo` 跑。这只影响 CI 环境，不影响真实桌面。
4. `build*` 已加入 `.gitignore`，三套构建目录不入库。

## 尚未做（M1 范围）

- `store_*` 的其余 API（`RhGame`/`RhFilter`/子表/作业）
- `platform.c` 解析 `data/platforms.conf` 的 101 平台
- `debian/` 打包骨架与首个可安装 deb
- 首个 git tag `v0.1.0-M0`
