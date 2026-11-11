# M2 导入 · 实机验收记录

- 日期：2026-11-11
- 基线：`ARCHITECTURE.md` v0.2 §14「M2 导入」
- 环境：WSL2 Ubuntu 26.04.1 / x86_64 / gcc 15.2 / meson 1.10.1 / ninja 1.13.2
  （glib 2.88.0、sqlite 3.46.1、libarchive 3.8.5、libxml2 2.15.2、GTK 4.22.4）
- 一键复跑：`bash tools/m2-check.sh`（完整含 deb）；`SKIP_DEB=1` 只跑功能；
  `SKIP_BUILD=1` 复用现有构建
- 结果：**`tools/m2-check.sh` PASS 43 / FAIL 0 / SKIP 0**（含 deb；`SKIP_DEB=1` 时 42 项）。rar 已实机补测，见 §2 第 32–34 条；
  `tools/m1-check.sh` 回归 **PASS 9 / FAIL 0**（含三 deb 构建、lintian 0/0）

## 1. 交付内容

| 模块 | 文件 | 职责 |
|---|---|---|
| 字符串/命名 | `src/core/rh-str.{h,c}` | trim/lower/ext/base_noext/rom_base_name/sanitize_dirname/parse_disc_no/name_sim |
| 文件操作 | `src/core/rh-fs.{h,c}` | copy_atomic（0 复制 / 2 已存在且一致 / 3 冲突 / 1 失败）、write_text_atomic、walk_files、remove_tree、is_image/is_text |
| 哈希 | `src/core/rh-hash.{h,c}` | 一次读盘算 crc32/md5/sha1 |
| 分类 | `src/core/classify.{h,c}` | 五级优先级链、复合扩展名、dual_role |
| 归档 | `src/core/rh-archive.{h,c}` | list/is_encrypted/extract/create_zip；拒绝绝对路径与 `..` |
| DAT | `src/core/dat.{h,c}` | Logiqx 解析、导入、按 crc/md5/sha1 查找（无全局缓存） |
| 作业 | `src/core/job.{h,c}` | begin/finish/cancel/items/unfinished |
| 导入编排 | `src/core/import.{h,c}` | 两阶段、压缩包六级分流、多碟/m3u/DAT、幂等复制、续做 |
| 整库导入 | `src/core/library.{h,c}` | ES/ES-DE `gamelist.xml`、Pegasus `metadata.pegasus.txt`；≤3 层探测 |
| CLI | `src/cli/{main,cmds}.c` | `init` / `import` / `dat` / `config` |

GUI 侧未改：M2 的 GUI 进度/取消按 §14 属 M3 接线项，本阶段只保证核心 `rh_ui` 回调可用。

## 2. 验收项（43 条全过，含 deb）

| # | 判据 | 证据 |
|---|---|---|
| 1 | `build-nogui` 构建通过 | m2-check 第 1 项 |
| 2 | 构建 0 warning | `grep warning` 为空 |
| 3 | 单元测试全过（含新增 `str-fs-hash` 6 例） | `meson test` 5/5 |
| 4 | `init --root --dat-dir` 建库成功 | 退出码 0 |
| 5 | `dat import <platform> <file>` 成功 | 退出码 0 |
| 6 | 导入命令整体退出码 0 | 9 个输入 |
| 7 | 单 ROM zip 包直存 `nes/single_nes/rom/single_nes.zip` | 文件存在 |
| 8 | 单 ROM 7z 包直存 `nes/single_7z/rom/single_7z.7z` | 文件存在 |
| 9 | 混合包 ROM 解压到 `nes/cart/rom/cart.nes` | 文件存在 |
| 10 | 混合包封面 → `covers/pic.png` | 文件存在 |
| 11 | 混合包说明 → `intro/note.txt` | 文件存在 |
| 12 | PSV 特征包整体保留 `psvita/psv_game/rom/psv_game.vpk` | 文件存在 |
| 13 | 街机 `.7z` 直存 `mame/mame_pack/rom/mame_pack.7z` | 文件存在（§5 存疑项闭环） |
| 14 | 二义扩展名 `.pkg` 非交互落到候选首位平台 | glob `*/Update/rom/Update.pkg` |
| 15 | 单文件 ROM 落到 `nes/loose/rom/loose.nes` | 文件存在 |
| 16 | 多碟 CD 第 1 碟落位 | `dreamcast/Quest/rom/Quest (Disc 1).gdi` |
| 17 | 多碟 CD 第 2 碟落位 | `… (Disc 2).gdi` |
| 18 | m3u 生成且按碟号排序 1→2 | 逐行比对 |
| 20 | DB 断言：≥6 游戏；多碟 disc_no>0；DAT 描述改名；rom_status=good；dat_source；媒体 2 行；无“有游戏无 ROM”；每游戏 8 子目录齐备 | db.log 全 PASS |
| 21 | 整库导入命令退出码 0 | lib.log |
| 22 | 整库 ROM 落到 `megadrive/Sonic the Hedgehog/rom/…` | 文件存在 |
| 23 | 整库封面改名 `…-image.png` 并入库 | 文件存在 |
| 24 | 整库元数据（name/developer/year/genre）写入 | DB 断言 |
| 25 | `kill -9` 后库里 0 条半记录 | 导入 0.2 s 后强杀，孤儿计数 0 |
| 26 | 重启后重放导入成功 | 退出码 0 |
| 27 | 续做后 100 条全入库 | DB 断言 |
| 28 | 续做后无半条记录 | DB 断言 |
| 29 | 续做后无重复 ROM 行 | DB 断言 |
| 30 | `dpkg-buildpackage` 构建三 deb | 构建日志 |
| 31 | staging 下三个 deb 产物存在 | 文件存在 |
| 32 | rar 单 ROM 包直存 `nes/single_rar/rom/single_rar.rar` | 文件存在（`rar 7.20` 现场造包） |
| 33 | rar 混合包 ROM 解压 `nes/rarc/rom/rarc.nes` | 文件存在 |
| 34 | rar 混合包封面 `nes/rarc/covers/rar.png` | 文件存在 |
| 35 | 整库同名两条清单项 → 第 1 碟落位 | `dreamcast/Show/rom/Show (Disc 1).gdi` |
| 36 | 整库多碟 m3u 生成 | `Show.m3u` |
| 37 | 整库清单缺一条 ROM：其余仍导入且告警 | `warning: 清单中的 ROM 不存在，跳过` |
| 38 | 整库多碟 `is_multi_disc=1` 且 2 条 rom | DB 断言 |
| 39 | 同一整库清单重复导入幂等（无重复 rom/media） | DB 断言 |
| 40 | 重启后无遗留 `queued` 作业 | DB 断言 |
| 41 | 被 `kill -9` 的作业被标记 interrupted | `jobs.error like '%interrupted%'` |

另：三 deb 已在 root 通道 `dpkg -i` 覆盖安装，`rom-hamster --version`、`import --help`
正常，`RH_SMOKE_TEST=1 xvfb-run rom-hamster-gui` 退出码 0。

## 3. 本轮定位并修掉的真实缺陷

均为实机跑出来的问题，不是测试写错。

1. **GOptionContext 缺 argv[0]**：子命令参数直接喂 `g_option_context_parse`，第一个
   参数被当成程序名，`--root` 读不到 → 打印 usage。修法：`SubArgs` 补占位 argv[0]。
2. **`rh_job_begin` 段错误**：把 `GPtrArray<RhPlanItem*>` 当 `RhJobItem*` 用，
   `bind_str` 解引用 0x1（ASan 抓到）。修法：执行阶段另建 `RhJobItem` 数组。
3. **续做匹配一律不命中**：`ji->seq == it->seq`，而 `RhPlanItem.seq` 从未赋值；
   改为与 `i + 1` 比。
4. **libarchive 内 SEGV**：`archive_read_data_block` 传 NULL `off_t*`；改局部变量。
5. **单碟被误判多碟**：`target_filename` 条件把“已有 1 个 rom”算成多碟，导致
   `(Disc 2)` 且旧文件被改名。改为 `disc>0 || existing->is_multi_disc`。
6. **改名后旧 rom 行残留**：库里 `x.7z` 与 `x (Disc 1).7z` 并存。改为事务内对比
   实际目录文件集，删掉不在集合里的旧行。
7. **混合包媒体找不到归属**：先扫包定 `pkg_platform/pkg_main`，媒体登记时兜底，
   `exec_media` 反推 game id。
8. **媒体先于 ROM 执行**：新增 `order_items()`，rom 前置、media 后置；注意
   `g_ptr_array_set_size(items,0)` 会释放元素，必须先摘掉 free_func。
9. **`dat.c` 静态 `match_cache`** 违反 §1.1 硬约束 1；改为出参 `RhDatMatch**`，
   删除 `rh_dat_match_clear`。
10. **追加碟号覆盖 DAT 名字/状态**：只在 DAT 命中时改名，并写 `rom_status/dat_source`。
11. **整库导入读不到库根**：`library.c` 误用不存在的 `ctx->rom_root`；改为按
    `opts->rom_root` → 配置 `ROM_ROOT` 解析（本轮新写模块时发现）。
12. **二义后缀平台选错**：`.32x` 同时是 megadrive 与 sega32x 的扩展名，旧逻辑取
    “第一个声明 ambiguous 的平台”得到 megadrive。改为在声明二义的平台里优先
    “该后缀也是它主扩展名”的那个。
13. **二义包平台为 NULL 时造出 `(null)/` 目录**：`st.platform` 为空仍建 item；
    记录 `ambig_platform` 兜底，两者都空则跳过并告警。
14. **CLI 参数内存泄漏**：`g_option_context_parse` 会把已消费参数移出 argv，
    `g_strfreev` 收不全。`SubArgs` 改为自行记账释放（ASan 抓到的 35 byte/3 obj）。
15. **`platform.c` 键名转小写泄漏**：`g_ascii_strdown(key,-1)` 返回值丢弃，
    每次加载规则泄漏 15×130 byte（ASan 抓到的既有缺陷）。
16. **测试夹具泄漏**：`test-store-crud` 覆盖 `patch_type` 前未释放默认 `""`（1 byte）。
17. **街机 `.7z` 被当混合包解压**：街机直存分支只认 `.zip`，`.7z` 会解出
    `a.bin`/`b.rom` 并落到 atari2600；改为 `.zip/.7z` 均可直存，`rom_format`
    按实际后缀记（`docs/rules-verification.md` §5 第 5 条由此闭环）。
18. **整库同名多碟被压成一碟**：`library.c` 的碟号只从“目录里已有文件”推断，
    不看源文件名，两个 `<name>` 相同的条目都落成 `Show.gdi`，第二张碟覆盖/跳过。
    改为用 `rh_parse_disc_no(base_name)` 取碟号，目标名 `Show (Disc N).gdi`，
    并据此置 `is_multi_disc`（本轮新增整库多碟夹具后暴露）。

其余小坑：`RH_ERR_USAGE` 不存在、`GStatBuf` 无 `st_mtim`、缺 `<glib/gstdio.h>`、
`\x01\x02` 被当续位十六进制、`false`→`FALSE`、`&(RhErr){...}` 不能直接比较、
`unused parameter` 等，构建已回到 0 warning。

## 4. 已知未闭环（M2 范围内接受，记录在案）

1. **非交互多候选后缀取第一个**：`.bin` → atari2600、`.cue` → 3do、`.pkg` → ps3，
   与 bash 的“弹菜单”语义不同；`-y` 是显式选择默认，交互模式仍会提问。
   平台归属要精确，需 `--platforms` 自定义规则或交互选择。
2. **中断语义是“标记 + 重放”，不是“自动续做”**：导入启动时把上次未完成
   job 置 `failed` + `error='interrupted by restart'`，随后整批重放；
   `size+mtime` 一致即跳过，所以不会重复复制。`rh_job_unfinished` 已有消费者
   （清扫逻辑），但仍没有 `job list/resume` CLI 入口。
3. **整库“追加到已有游戏”的语义**：同名同目录再次导入按幂等处理（覆盖元数据、
   保留已有 rom），但没有“只追加新碟、不碰旧元数据”的显式开关。
4. **GLib 自身 23 byte 泄漏**：`g_option_context_parse` 内部
   `g_locale_to_utf8` 的单次分配在 `--help` 也出现，非本项目代码；ASan 单元
   测试与库导入路径均无泄漏。
5. **Pegasus 多行描述**不做保留（单行读取），影响面在 M3 导出侧。
6. **GUI M2 接线未做**：进度/取消按 §14 安排在 M3。

## 5. 复跑方式

```bash
# 功能 + deb（用户态）
bash tools/m2-check.sh

# 只要功能，跳过 deb
SKIP_DEB=1 bash tools/m2-check.sh

# M1 回归（含 deb/lintian）
bash tools/m1-check.sh

# 安装验证（root）
dpkg -i dist/rom-hamster-data_*.deb dist/rom-hamster_*.deb dist/rom-hamster-gui_*.deb
RH_SMOKE_TEST=1 GSK_RENDERER=cairo GDK_BACKEND=x11 xvfb-run -a rom-hamster-gui
```

冒烟数据固定在 `~/.cache/romham-m2`（含 `roms/`、`dat/`、`pack/`、`export/`、
`resume/`），测试脚本每次重建，不污染 `$HOME` 其他目录。
