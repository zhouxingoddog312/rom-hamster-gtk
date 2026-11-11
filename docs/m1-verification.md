# M1 存储与规则 —— 验收记录

> 日期：2026-11-11
> 环境：WSL2 / Ubuntu 26.04.1 / x86_64 / gcc 15.2 / meson 1.10.1 / ninja 1.13.2 /
> glib 2.88.0 / sqlite 3.46.1 / libarchive 3.8.5 / libxml2 2.15.2 / gtk4 4.22.4 /
> debhelper 13.31 / lintian 2.129.0
> 目标：`store_*` 全量、`platform.c` 五段规则、`debian/` 骨架与**首个可安装 deb**。

## 1. 交付物

| 文件 | 说明 |
|---|---|
| `src/core/rh-records.h/.c` | 10 个记录结构体 + `rh_now_iso()` / `rh_game_gen_id()`；纯数据结构，不碰 SQL、不打印、不读文件 |
| `src/core/store.h/.c` | §4 全量 API 与**全部 SQL**（约 1100 行）；schema v1 的 9 表 + 9 索引 + 3 个 FTS 触发器，单事务建库 |
| `src/core/platform.h/.c` | `platforms.conf` 五段编译成 `RhPlatformRules`：`by_id` / `platforms` / `by_ext` / `by_alias` + 4 个集合 |
| `tests/unit/test-platform.c` | 5 例；把 101 平台与相对 1.0.0 的四处修正钉成断言 |
| `tests/unit/test-store-crud.c` | 8 例；每例独立小库，覆盖 CRUD、约束、级联、筛选、DAT、作业、检索 |
| `debian/` | `control`（三包）、`rules`（`dh --buildsystem=meson`）、三个 `.install`、`changelog`、`copyright`、`source/format` |
| `etc/rom-hamster/` | `rom-hamster.conf`（14 键注释模板）、`platforms.conf`（系统覆盖层，默认无条目） |
| `man/rom-hamster.1`、`man/rom-hamster-gui.1` | 两个前端的手册页 |
| `ARCHITECTURE.md` §4 / §13 / §14 | 出参所有权、DAT 三段式、计数签名、100 → 101 平台 |

## 2. 验收证据

| # | 验收项（ARCHITECTURE §14 M1） | 实测结果 |
|---|---|---|
| 1 | 101 个平台全部解析成功 | `test-platform /platform/extensions` OK；`platform_count(r) == 101` 断言通过 |
| 2 | 5 段规则全部解析成功 | `/platform/other-sections` OK：`.rom` 是街机后缀、`.zip/.7z/.rar` 是归档、`covers` 目录、`.png → covers`、`.ips → patcher`、`rom.nes` 不是媒体 |
| 3 | 媒体类型识别有单测 | 同上：`clip.mp4 → videos`、`readme.txt → intro`、`weird.xyz/noext → NULL` |
| 4 | `store_*` CRUD 有测试 | `/store/crud`、`/store/auto-id` OK |
| 5 | `dir_path` UNIQUE 有测试 | `/store/dir-path-unique` 返回 `RH_ERR_EXISTS`；`/store/auto-id` 同目录二次 upsert 后 `count == 1` |
| 6 | 级联删除有测试 | `/store/children-and-cascade`：删 game 后 roms / media / game_tags 计数为 0，二次删除报 `RH_ERR_NOT_FOUND` |
| 7 | `dpkg -i` 后 `rom-hamster --version` 可运行 | 三包安装成功，输出 `rom-hamster 0.1.0` + 维护者/主页/许可/schema |
| 8 | `lintian` 无 error | 三包 `lintian` 输出为空（0 error、0 warning） |
| 9 | 私有库目录与 rpath | `dpkg -L` 显示 `/usr/lib/x86_64-linux-gnu/rom-hamster/libromham-1.0.so`；`ldd /usr/bin/rom-hamster` 解析到该私有路径 |
| 10 | conffile 登记 | `/var/lib/dpkg/info/rom-hamster.conffiles` 含 `/etc/rom-hamster/platforms.conf` 与 `rom-hamster.conf` |
| 11 | 配置分层在装好的包里有效 | 往 `/etc/rom-hamster/rom-hamster.conf` 写两个键 → `config --explain` 报 `:61 (system (/etc))`；再写 `~/.config` 层 → 报 `overrides /srv/roms from /etc/...:61` |
| 12 | GUI 仍可启动 | `RH_SMOKE_TEST=1 GSK_RENDERER=cairo xvfb-run -a rom-hamster-gui` → `smoke: window mapped`，rc=0 |
| 13 | 无未定义行为 | `build-asan`（`-Db_sanitize=address,undefined`）4/4 测试通过（`detect_leaks=0`，GTK 与 sqlite 自带分配不计） |
| 14 | SQL 只在 `store.c` | `src/core` 内除 `store.c` 外无 `SELECT/INSERT/UPDATE/DELETE` 字面量 |

复跑：`tools/m1-check.sh`（非 root 时会跳过安装段，deb 段可用 `SKIP_DEB=1` 跳过）。

## 3. 与文档的偏差（已回写 ARCHITECTURE，理由在此）

1. **出参所有权**：§4 原文写 `RhErr store_game_get(RhCtx *, const char *id, RhGame *out)`，即调用方预置结构体。实现改为 `RhGame **out`，函数分配、`rh_game_free` 释放；数组统一 `g_ptr_array_unref`。
   理由：预置结构体要求调用方知道每个字段的默认哨兵（year 0、rating -1、size -1），这层知识会泄漏到 CLI/GUI 两个前端各写一遍；函数分配后前端只负责释放，"NULL 与哨兵"的换算只发生在 `store.c` 一处。

2. **DAT 导入拆成三段**：§4 原文只有一个 `store_dat_import_file(ctx, platform, path)`。实现为 `store_dat_begin` / `store_dat_entries_add` / `store_dat_finish`。
   理由：§5.1 边界表把 XML 解析划给 `dat.c`，`store.c` 不应打开 DAT 文件；拆开后 `store.c` 只收已解析的 `RhDatEntry` 数组，且天然支持流式插入大 DAT（>10 万条时不必先全量驻留内存）。

3. **`store_game_count` 签名**：原文 `int store_game_count(RhCtx *)`，实现为 `RhErr store_game_count(RhCtx *, gint64 *out)`。
   理由：库规模按 §14.1 要支持 10 万条，`int` 返回无法区分"0 条"与"出错"；统一成 `RhErr` + 出参后，错误处理与其它 API 一致。

4. **§14 M1 文字里的"100 平台"**：实际是 101（`data/platforms.conf` 在 M0 前已按 ES-DE 一手源补出 `plus4`）。已把 §13、§14 的 100 全部订正为 101；§0 对照基线的"100 个平台规则"指的是 bash 版 1.0.0，保持不动。

5. **打包时不编译单测**：`debian/rules` 传 `-Dtests=false`。单测依赖源码树 `data/platforms.conf` 与临时目录，属于开发验收，不该让每次 `dpkg-buildpackage` 都付编译与执行成本。单测由 `tools/m1-check.sh` 在开发机执行。

6. **`/etc` 层两个 conffile 默认不含生效条目**：`platforms.conf` 全注释、`rom-hamster.conf` 全注释。这样"装完即默认行为"，管理员改动能在升级时按 conffile 语义保留。

## 4. M1 编码中修掉的真实缺陷（全部由单测或实机安装暴露）

| 缺陷 | 症状 | 修法 |
|---|---|---|
| `g_ascii_strdown()` 返回值被丢弃 | `platform_is_archive(r,"ZIP")` 为假；`"Nintendo - NES"` 反查失败 | 三处（`ext_norm`、`add_aliases`、平台 ID/目录键归一）改为接收返回值并释放原串 |
| `patch_type`（NOT NULL）被绑成 NULL | `upsert` 报 `NOT NULL constraint failed: games.patch_type` | 该列单独绑空字符串，不经"空串当 NULL"的通用路径 |
| 绑定队列存的是临时串指针 | 关键字筛选先 `push_text` 后 `g_free`，`MATCH` 拿到已释放内存，结果恒为 0 条 | `BindVal` 自己 `g_strdup`，数组配 `bindval_free` |
| `apply_binds` 的 `db` 形参未用 | `-Wunused-parameter` | 删掉形参 |
| 手册页按 `.gz` 名写进 `.install` | `dh_install: missing files` | `.install` 写未压缩名，压缩交给 `dh_compress` |
| 源树里也有 `etc/rom-hamster/` | `dh_install` 优先从 `./` 取，`debian/tmp` 的副本被判"未安装"，`dh_missing` 报错 | `.install` 对 `/etc` 条目显式写 `debian/tmp/...` 前缀 |
| 私有目录库触发 ldconfig trigger | lintian `package-has-unnecessary-activation-of-ldconfig-trigger` | `override_dh_makeshlibs:` 置空（私有目录不进 ldconfig 缓存） |
| GUI 缺 man page | lintian `no-manual-page` | 补 `man/rom-hamster-gui.1`，随 `-Dgui=true` 安装 |

另有一次**环境层**误判：先前 M0 手工 `ninja install` 留下的 `/usr/share/rom-hamster/platforms.conf` 让 `rom-hamster` 包"拥有"了本应属于 `rom-hamster-data` 的文件，导致 data 包解压冲突。属于测试机脏状态，`dpkg --purge` 三包并删除残留后重装即消失；包本身的内容经 `dpkg-deb -c` 核对无误。

## 5. 未做 / 留给后续

- CLI 目前只有 `--version` 与 `config`；`store_*` 与 `platform_*` 尚无命令入口（M2 导入时接上）。
- `debian/` 没有 `rom-hamster-data` 之外的图标/`.desktop`/AppStream（M5）。
- 没有 `emulators.conf`（模拟器启动在 M4）。
- `docs/rules-verification.md` §5 的 5 条存疑项（街机 `.7z` 直存、psvita `.vpk` 等）需要 M2 真实导入验证。
- 首个 tag 待本次提交后打 `v0.1.0-M1`。
