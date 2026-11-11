# M3 验收记录：查询 / 详情 / 编辑删除 / 前端导出

日期：2026-11-11　基线：`46b02ca`（M3 改动在工作树）　环境：WSL2 Ubuntu 26.04.1 / gcc 15.2 / meson 1.10.1

## 1. 本阶段交付

| 模块 | 内容 |
|---|---|
| `src/core/detail.c` | `RhGameDetail` 只读视图模型，8 组资源固定顺序，不做文字格式化、不写库 |
| `src/core/query.c` | 排序键白名单 `name/rating/year/discs`，转发 `store_game_list`；无 SQL |
| `src/core/manage.c` | 编辑（12 个 `set_*` 标志）、删除（可选删目录，校验不逃出 `ROM_ROOT`）、媒体增删、完整性检查 |
| `src/core/export.c` | 自写模板词法/语法（`%var%`、`%var:xml\|json\|text%`、`%if/%else/%endif`、`%for/%endfor`）+ 三前端导出；错误在加载期报 `文件:行` |
| `src/core/store.c` | `store_stats()`：总数 + 按平台 `GROUP BY` |
| `src/cli/cmds.c` | `list/search/show/edit/delete/export/stats` 七个子命令 |

## 2. 验收脚本与结果

入口：`SKIP_DEB=0 bash tools/m3-check.sh`（`SKIP_DEB=1` 跳过 deb，并把该开关透传给 `tools/m2-check.sh`）

```
build                 PASS  ninja -C build
unit tests            PASS  meson test (5 suites)
fixture               PASS  3 个 NES + 元数据 + 2 媒体 + 多碟/m3u
query                 PASS  25 passed, 0 failed
template              PASS  7 passed, 0 failed
export                PASS  23 passed, 0 failed
stats                 PASS  games=3 roms=5 media=2
show                  PASS  8 组资源清单 / 缺 id 返回非 0
delete guard          PASS  无 --yes 退出 2；--yes --files 删行且删目录
dest 守卫             PASS  目标目录落在 ROM_ROOT 内被拒
compress               PASS  显式返回 "compression is not implemented yet"
M1/M2 regression+deb  PASS  m2-check 43 项、三 deb
M3 CHECK: ALL PASS
```

### 2.1 查询对照（`tools/m3-query-check.py`，25 项）

16 个筛选用例 + 8 个排序用例：CLI `list` 输出的 id 序列与直接 SQLite 查询逐项比对；另有"未知排序键必须报错"。结果 `25 passed, 0 failed`。

覆盖：platform / genre / developer / publisher / region / patch-type / year-from / year-to / status / format / tag / kw / multi / single / has-patch / no-patch；`name/rating/year/discs` 升降序。

### 2.2 模板加载期报错（`tools/m3-template-check.py`，7 项）

6 类故意写错的模板：未定义变量、未知转义、未闭合 `if`、未知循环、空 `%%`、未闭合 `for`。断言：退出码非 0、stderr 含 `文件:行号`（行号按实际推导）、目标目录未被创建。另加 1 条合法模板必须加载成功并导出。

### 2.3 导出（`tools/m3-export-check.py`，23 项）

- ES / ES-DE / Pegasus 三份清单与 `tests/fixtures/m3/*` **逐字节一致**；
- ES `gamelist.xml` 可解析；`<image>`/`<thumbnail>` 指向的媒体文件真实存在；`<releasedate>=19900101T000000`、`<rating>=0.85`；
- 多碟 `Beta` 的 `<path>` 为 `./Beta.m3u`（库内 m3u 优先），单碟 `Alpha` 为 `./Alpha.nes`；
- ES-DE 清单**不含** `image/thumbnail/marquee/video/fanart`，但 `downloaded_media/nes/covers/Alpha.png` 已落盘；
- Pegasus 两条记录、`launch:`、`extensions: nes`、`assets.boxFront/screenshot` 相对路径文件真实存在；
- `Beta.m3u` 内容按导出的碟文件重写为 `Beta.nes` + `Beta (Disc 2).nes`；
- Gamma 描述 `Gamma & <tag> "quote"` 经 XML 转义后解析往返一致。

### 2.4 deb

`dpkg-buildpackage` 产出三包（M2 验收同一条链路，43/43）：

| 包 | 大小 |
|---|---|
| `rom-hamster_0.1.0_amd64.deb` | 88088 B |
| `rom-hamster-gui_0.1.0_amd64.deb` | 5972 B |
| `rom-hamster-data_0.1.0_all.deb` | 10974 B |

从 CLI deb 解包后直接运行 `rom-hamster stats` 成功，证明包内二进制包含 M3 命令。

## 3. 本阶段修掉的真实缺陷

| # | 缺陷 | 发现方式 | 修法 |
|---|---|---|---|
| 1 | `render_template` 第 3 参是 `GHashTable *lists`，四处调用误传 `GPtrArray *none` | 编译/实跑 | 无循环体时统一传 `NULL` |
| 2 | `rh_frontend_load` 把 `full` 作 key 后又 `g_free` | ASAN heap-use-after-free | key 归 `GHashTable` 所有，删除该 free |
| 3 | `fe->media` 哈希表无 key 销毁函数 | ASAN 泄漏 | `g_hash_table_new_full(..., g_free, NULL)` |
| 4 | 模板区把 `.conf` 尾随的 `#`/`;` 注释行当正文渲染 | 导出清单末尾混入三行注释 | 模板段内 trim 后以 `#`/`;` 开头直接跳过（保留行号） |
| 5 | `join_extensions` 结果先 `g_free` 再 `g_strdup` | ASAN UAF | 只在使用点之后释放 |
| 6 | `frontend.media_dir` 模板串未释放 | ASAN 泄漏 | 补 `g_free(mtpl)` |
| 7 | `--frontend` 默认值 `"es"` 被 GOption 复制成自有串、`platforms/ids` 列表未释放 | ASAN 泄漏 + 归属错 | 默认 `NULL`、回退 `"es"`，各出口 `g_free` + `g_strfreev` |
| 8 | 缺 `setlocale(LC_ALL, "")`，`zh_CN.UTF-8` 下含中文路径参数触发 `Invalid byte sequence in conversion input` | 模板验收合法用例 rc=2 | CLI/GUI `main()` 首行加 `setlocale` + `<locale.h>` |
| 9 | 修 #5 时把 `g_free(exts)` 提前，造成二次释放 | ASAN 回归 | 只在循环末尾释放 |
| 10 | `store_stats` 忽略 `sqlite3_step` 错误仍返回 OK，并有符号性 warning | M2 "构建无 warning" 拦下 | 错误分支释放并返回；列文本显式 cast |
| 11 | `export.c` 的 `tagstr`/`desc_line` 在 `goto loop_out` 之后才初始化，早退路径会 free 未初始化指针 | `-O3` `-Wmaybe-uninitialized` | 声明提到循环顶部并置 `NULL`，清理处判空；顺带修 `store_game_tags` 恒真假判断 |

另：`list`/`show` 的 rating 由 `*` 改为 `%.2f`（`show` 空值打印 `unset`），便于与 SQL 对照。

## 4. 已知未闭环（如实记录）

1. **没有在真实 ES-DE / Pegasus 中加载验证**。当前只做到 XML/结构可解析、字段正确、golden 逐字节一致。
2. `[export.media]` 段缺失的退化路径、`media_all/media_kinds` 全 FALSE 只算相对路径不复制、`intro`/`patcher` 两类媒体导出，均未单独实跑。
3. Pegasus 清单 footer 注释行被剥离后末尾有两个连续空行（`$\n$\n`），可读但不干净。
4. `--compress` 在 M3 明确不支持，只返回 `UNSUPPORTED`；是否顺延 M4 未决。
5. GUI 侧 M3/M4 未接：`rh_ui` vtable 尚无查询/详情视图。
6. `tools/m2-check.sh` 的 6 条历史未闭环仍在（见 `docs/m2-verification.md` §4）。
