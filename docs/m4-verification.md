# M4 验收记录：首次运行 + 主窗口 + 详情页

日期：2026-11-12　基线：`ae16ec7`（M3）

## 1. 两步的范围

| 步 | 内容 |
|---|---|
| 1 | 首次运行弹 `GtkFileDialog` 选 `ROM_ROOT`；写用户配置、建库、直接进主窗口；`GtkColumnView` 五列；搜索 100 ms 防抖走 `rh_query_list()` |
| 2 | 列表激活进入详情页：`GtkGrid` 元数据、只读描述、`GtkStackSwitcher` + `GtkStack` 的 8 组资源、图片异步缩略图、视频内嵌 `GtkVideo` + 外部打开按钮、返回列表 |
| 2 | 详情页与列表在同一个主窗口 `GtkStack` 内切换，不另开窗口 |

**明确未做**：真实视频后端的播放验证、真实指针拖拽事件的自动化。启动游戏已在本轮补做。

## 2. 验收结果

入口：`bash tools/m4-gui-check.sh`（xvfb）

```
build                  PASS  ninja -C build-gui
核心库                 PASS  ldd libromham 无 gtk/gdk/pango
unit tests             PASS  meson test 6 suites
fixture                PASS  3 games（Alpha 封面为真实 400x400 PNG）
首次运行空目录         PASS  窗口 map、0 行、romhamster.db 已建、ROM_ROOT 已写
已有库                 PASS  3 行
搜索 Alpha             PASS  1 行
搜索 zzzz              PASS  0 行
详情页                 PASS  pages=8 title=Alpha · nes · 1990 thumbs=1/1 videos=1
返回                   PASS  back stack=list
M4 GUI STEP2: ALL PASS
```

补充验证：

- AddressSanitizer 构建 `build-gui-asan` 跑详情页 + 返回，无 ASAN 报错。ASAN 下等待 2.5 s 仍可能只到 `thumbs=0/1`（ASAN 下缩略图线程明显更慢），普通构建为 `1/1`；不把 ASAN 的缩略图计数当功能失败。
- `SKIP_DEB=0 bash tools/m2-check.sh` → 43/43，三 deb 构建成功。
- `SKIP_DEB=1 bash tools/m3-check.sh` 全绿。
- 解包 deb 后 `rom-hamster-gui` 在 xvfb 下进入详情页，8 组、缩略图 1/1、返回列表均正常（证明 `.ui` 资源确已进包）。

## 3. 第一步修掉的缺陷

| # | 缺陷 | 发现方式 | 修法 |
|---|---|---|---|
| 1 | `GtkStack` 两个 child 没有名字，`set_visible_child_name()` 报 `Child name 'empty' not found` | 首启 smoke 的 Gtk-WARNING | 建窗口后 `gtk_stack_page_set_name()` 设 `list`/`empty` |
| 2 | `gui_new()` 没保存 `GtkApplication` 指针，`g_application_quit(G_APPLICATION(NULL))` 断言失败 | smoke 退出时 GLib-CRITICAL | 结构体保存 `app` |
| 3 | `open_library()` 先 `rh_config_set("ROM_ROOT")`（释放同键旧值），再把原 `root` 指针传给保存函数 → use-after-free，写出乱码配置并建出乱码目录 | 第二次启动读回 `ROM_ROOT=�� ISd`，项目根出现乱码目录 | 先 `g_strdup`，先落盘再覆盖运行时配置 |
| 4 | `GtkSearchEntry::search-changed` 在程序化 `gtk_editable_set_text()` 时不触发，搜索用例不更新 | smoke 搜索 2 行不变 | 改接 `GtkEditable::changed` + 仍保留 100 ms 防抖 |
| 5 | GUI 链接私有 soname `libromham-1.0.so`，`dpkg-shlibdeps` 找不到 shlibs 信息，deb 构建失败 | `SKIP_DEB=0` 回归 | `debian/rules` 覆盖 `dh_shlibdeps -- --ignore-missing-info`；GUI 已显式 `Depends: rom-hamster (= ${binary:Version})` |

## 4. 第二步修掉的缺陷

| # | 缺陷 | 发现方式 | 修法 |
|---|---|---|---|
| 1 | `detail_pane_new()` 把 builder 的借用对象直接返回，`g_object_unref(builder)` 后根窗口被销毁；主窗口拿到悬空指针，`GTK_IS_WIDGET=0`，`gtk_stack_add_titled` 断言失败并段错误 | 详情页 smoke 立即崩溃；打印指针/类型确认 | 返回前 `g_object_ref(root)`；主窗口加入 stack 后 `g_object_unref` 自己那份 |
| 2 | `gtk_stack_add_titled()` 按 GTK4 签名传了多余的 `NULL`，编译失败 | 首次编译 | 改为 3 个参数 |
| 3 | `gtk_text_view_set_margin_start/end/top/bottom` 不存在 | 编译失败 | 改用 `gtk_text_view_set_left/right/top/bottom_margin` |
| 4 | 夹具里的 PNG 缺 chunk length、后补又缺每行 filter byte，缩略图解码报 `Unknown filter method 200` | `thumbs=0/1` + 打印解码错误 | 修正 fixture 的 PNG 生成；`tools/m3-fixture.sh` 现生成真实 400x400 PNG |
| 5 | 缩略图回调保存裸 `DetailPane*`，窗口先销毁时可能 UAF | 代码审查 | 改为 `GWeakRef` 持有详情根窗口；窗口没了就不计数、不碰 `dp` |
| 6 | smoke 在 `show_detail()` 后立刻打印 `thumbs`，异步解码还没回来，误报 0 | 测试输出顺序 | smoke 延迟到异步缩略图完成后打印并退出（当前 2.5 s） |

已知未消除的编译警告：`gdk_texture_new_for_pixbuf` 在 GTK 4.22 标记 deprecated。当前基线 GTK 4.10 仍可用，暂保留，未改用 `gdk_memory_texture_new`（后者需要处理 pixbuf 的 RGB/RGBA 格式差异）。

## 5. 详情页实现要点

- 元数据字段：Platform / Year / Genre / Developer / Publisher / Region / Players / Rating / Status / Format / Multi-disc / Patch / DAT / Directory，值可选中复制。
- 资源组固定顺序：`rom`、`covers`、`screenshots`、`videos`、`marquees`、`miximages`、`intro`、`patcher`，与 `rh_detail_group_kind()` 一致，不重复实现目录规则。
- 图片在 `GTask` 线程池里先查磁盘缓存，再决定解码原图还是读缓存；主线程只做 `GdkTexture` 绑定；UI 不等待磁盘。
- 缩略图缓存目录 `$XDG_CACHE_HOME/rom-hamster/thumbs/`，文件名是 `SHA1(路径 + size + mtime)`，内容是 400×300 PNG；先写 `.thumb.XXXXXX` 再 `rename`，不产生半张图。缓存键不含纳秒（`GStatBuf` 在该构建下没有 `st_mtim` 字段），同一秒内改图且 size 不变的极端情况会命中旧图，属于已知取舍。
- 视频用 `GtkVideo` 内嵌，旁边常驻 “Open externally” 按钮；无媒体后端时用户仍可外部打开，界面有提示。
- 描述是只读 `GtkTextView`，换行按 `WORD_CHAR`，不解释标记、不做富文本。
- 文件缺失在图片页和文件页都标 `(missing)`，不隐藏。

## 6. 仍未闭环

1. 缩略图缓存没有容量上限/过期清理策略；当前只保证原子写与缓存命中，不做 LRU 或按大小回收。缓存损坏时会回退解码原图并覆盖。
2. `GtkVideo` 只验证了控件构建，未在真实带 GStreamer 后端的桌面验证播放/暂停/无后端降级流程。
3. 详情页没有右键菜单、删除/编辑入口、媒体翻页放大；这些不在本步。
4. 参考机型（4 核 / 16 GB / SATA SSD / 1920×1080 / GTK 4.16）上的首屏与 10 s 滚动帧率仍未复测；本机 WSL2 的软件渲染数字只作基线记录。
5. 真实 `GtkFileDialog` 人机点选仍未自动化。

## 7. 10k 条性能基线

夹具与脚本：

- `tools/m4-perf-fixture.sh`：生成 10000 条游戏、11000 条 ROM（每 10 条一个多碟）、1501 条媒体（每 20 条三张截图，首条带真实 400×400 封面），直接写 SQLite，避免逐条 CLI 导入污染测速。
- `tools/m4-perf.sh`：跑 3 轮启动 / 搜索 / 详情 / 10 s 滚动，原始日志在 `$HOME/.cache/romham-m4/perf/runs/`；`tools/m4-perf-report.py` 汇总为 `docs/m4-perf-baseline.md`。
- GUI 只在 `RH_PERF=1` 时打点：`main()` → `map`、`map` → `after-paint`、搜索、详情、缩略图、`after-paint` 帧间隔、`/proc/self/status` RSS。默认不输出、不影响正常使用。
- 本脚本是人工基线，不接入 CI；ARCHITECTURE §14.1 明确帧率/启动时间在无显示器环境测不准。

本机 3 轮中位数（WSL2 / i3-10100T / 3.7 GB / Xvfb / GTK 4.22.4，非参考机型）：

| 指标 | 阈值 | 实测 | 结论 |
|---|---|---|---|
| 冷启动 | < 1.5 s | 首轮 834.5 ms，后两轮 288.5 / 259.1 ms | 达标 |
| 首屏 | < 300 ms | 711.4 ms | 超阈值；3 行夹具也约 690 ms，主因是 WSL 首次软件渲染，不是 10k 列表 |
| 滚动 10 s | p95 ≤ 33 ms / max ≤ 100 ms | p95 44.3 ms / max 207.3 ms | 超阈值；同样受软件渲染限制，参考机型复测前不判发布阻断 |
| 关键字筛选 | < 200 ms | 总 128.6 ms（查询 28.4 ms） | 达标；防抖已从 200 ms 改为 100 ms |
| 详情元数据 | < 150 ms | 4.8 ms | 达标 |
| 首张缩略图 | < 400 ms | 78.7 ms | 达标 |
| 10 s 滚动 RSS 增量 | < 50 MB | 24.5 MB | 达标 |
| 导入进度 / 取消 | ≤ 200 ms / < 1 s | SKIP | GUI 导入尚未实现，未伪造指标 |

完整记录见 `docs/m4-perf-baseline.md`。

## 8. 导入进度 / 取消（2026-11-12）

### 8.1 实现

- 核心取消不再只在条目之间看一眼：
  - `rh_fs_copy_atomic_cancellable()` 在 256 KB 块循环里检查 `cancel_flag`，取消返回 4，并删除 `.part.XXXXXX` 临时文件；原 `rh_fs_copy_atomic()` 保留为无取消参数版本。
  - `rh_arc_extract_cancellable()` 在每条目头、每个数据块检查取消；取消返回 2；原 `rh_arc_extract()` 保留。
  - `rh_import_run()` 入口清零 `ctx->cancel_flag`；扫描阶段在 `plan_path` 顶部、目录循环、每个普通文件发 `rh_progress(-1, basename)`，扫描完成后再检查一次；压缩包解压走 cancellable 版本。
  - 执行阶段 `exec_rom()` / `exec_media()` 使用 cancellable 复制；返回 2 时主循环 `rh_job_finish(..., "cancelled")` 并返回 `RH_ERR_CANCELLED`。
- GUI：
  - HeaderBar 增加 `Import` 按钮，选目录后把 `rh_import_run()` 放进 `GTask` 工作线程；主线程不碰 SQLite。
  - 导入期间显示 `GtkProgressBar`、状态栏文件名，禁用 `Refresh`、搜索、列表和 `Import`；`Cancel` 按钮在导入开始 100 ms 后可用。
  - 工作线程只调用 `RhUi::progress`；GUI 用 `GWeakRef` 投递到主线程，最多每 100 ms 调度一次 UI 更新。取消通过 `rh_job_cancel()` 置原子标志，不跨线程直接操作控件。
  - 导入期间 `close-request` 返回 `TRUE`，提示先取消，避免窗口销毁后工作线程继续使用 `RhCtx`。
  - 完成后 `refresh()` 重新查询列表，状态栏给出 imported/media/skipped/failed 汇总；取消显示 `Import cancelled`。
- 平台规则：生产环境仍走 `platform_rules_load_standard()`（datadir / sysconf / 用户层）。开发树测试用 `RH_PLATFORMS=data/platforms.conf` 显式加载；规则数为 0 时导入直接报 `平台规则为空`，不再静默把所有文件跳过。

### 8.2 验收

命令：

```bash
bash tools/m4-import-check.sh
```

夹具与结果：

| 用例 | 夹具 | 结果 |
|---|---|---|
| 正常导入进度 | 800 个 32 KB NES 文件，全新库 | imported=800，rows=800 |
| 进度回调最大间隔 | 同上 | 32.7–83.7 ms（多次运行，阈值 ≤200 ms） |
| UI 刷新最大间隔 | 同上 | 109.8–153.4 ms（阈值 ≤200 ms；首次 UI 更新 571–787 ms 是首帧绘制，单独记录，不计入刷新间隔） |
| 取消延迟 | 1500 个 64 KB NES，300 ms 时取消 | 3–4 ms（阈值 <1 s） |
| 取消后状态 | 同上 | `cancelled=1`，155 条完整游戏保留；无 `.part.*`；无 games 无 rom 的半条记录 |
| 取消后再次导入 | 同一库、同一输入 | imported=1500，rows=1500（幂等补齐，无重复） |
| ASAN 取消 | 500 个 64 KB NES，300 ms 取消 | rc=0，取消 4 ms，无 ASAN 报告 |

回归：

- `SKIP_DEB=1 bash tools/m2-check.sh`：PASS 42 / FAIL 0。
- `SKIP_DEB=1 bash tools/m3-check.sh`：M3 CHECK: ALL PASS。
- `bash tools/m4-gui-check.sh`：M4 GUI STEP2: ALL PASS；详情页冷缓存 `cache_hits=0`、热缓存 `cache_hits=1`，缓存文件 1 个。
- `bash tools/m4-import-check.sh`：M4 IMPORT: ALL PASS。
- `bash tools/m4-drop-check.sh`：M4 DROP: ALL PASS（两个目录共 100 个文件，rows=100）。
- 新增单测 `/util/cancel`：复制取消返回 4 且目标不存在；解压取消返回 2 且不落文件；`meson test -C build` 5/5 通过。

### 8.3 取消语义

- 取消时**保留已经完整提交的游戏记录**，不回滚整批；只保证不产生“games 有行但没有 rom”的半条记录。理由：导入以单条游戏事务提交，回滚整批会删掉用户已经看到并可能继续使用的条目；架构 §14.1 只规定取消延迟，未规定整批回滚。
- 正在复制的文件通过 `.part.XXXXXX` + rename 保证原子性；取消时删除临时文件。取消发生在解压包扫描阶段时，临时解压目录在 `rh_import_run` 清理路径删除。
- 取消后再次导入同一目录是幂等的：完整条目跳过/更新，缺失条目补齐。
- 扫描阶段取消不应答 UI 对话框；`opts.non_interactive=TRUE`，二义格式/多平台候选取第一项，不再弹选择。

### 8.4 本步修掉的缺陷

| # | 缺陷 | 发现方式 | 修法 |
|---|---|---|---|
| 1 | `GTask` 的 task data 设成了路径字符串，`import_thread` 把 `data` 当 `Gui*` 用，`g->ctx` 读到路径内容，导入线程启动即段错误 | GUI import smoke rc=139；ASAN 指向 `rh_import_run` 的 `ctx->cancel_flag` | 新增 `ImportArgs{Gui*, path}` 作为 task data，线程函数从 `ImportArgs` 取两者 |
| 2 | `rh_ctx_set_ui()` 保存指向块作用域复合字面量 `&(RhUi){...}` 的指针，函数返回后悬空；进度回调会 UAF | 代码审查（在导入 smoke 前修掉） | 改为文件级 `static RhUi GUI_UI`，每次启动设置 `userdata` |
| 3 | `ui_max_gap` 把首帧绘制时间算进“进度刷新间隔”，800 文件误报 425.6 ms | `m4-import-check.sh` 首次运行 | 首次 UI 更新单独记 `first_ui_ms`，从第二次起统计 `ui_max_gap` |
| 4 | 平台规则为空时导入静默全跳过 | 开发树无 `/usr/local/share/rom-hamster/platforms.conf` | 规则数为 0 直接返回 `RH_ERR_CONFIG`，提示安装 data 包或设 `RH_PLATFORMS` |

### 8.5 仍未闭环

1. 真实 `GtkFileDialog` 人机点选仍未自动化；smoke 用 `RH_SMOKE_IMPORT_DIR` 直接走选完目录后的路径。
2. 真实指针拖拽事件未自动化；smoke 通过 `RH_SMOKE_DROP_PATHS` 构造 `GdkFileList` 并调用同一个 `on_drop`，验证 receiver、路径过滤、多输入导入与进度刷新。导入完成后自动打开详情页、导入历史/失败重试不在本步。
3. 压缩包取消只在单测覆盖“取消返回 2 且不落文件”；GUI 级“解压大包时取消”尚未跑真实大包夹具。
4. 取消后的已提交条目保留策略已写在本节，但尚未写进 `ARCHITECTURE.md` 的验收口径；需要用户确认是否接受“不回滚整批”。

### 8.6 缩略图磁盘缓存与拖拽导入（本轮补做）

缩略图：

- 目录 `$XDG_CACHE_HOME/rom-hamster/thumbs/`；键 `SHA1(400x300|路径|size|mtime)`，输出 400×300 PNG。
- 线程里先读缓存；缓存不存在、损坏或键不匹配才解码原图。解码成功后先写 `.thumb.XXXXXX`，`g_rename` 原子替换。
- `DetailPane` 增加 `thumb_generation`：详情页重新 `show` 时旧任务的结果直接丢弃，避免旧缩略图计数/绘制污染新详情页；`thumbs_*` 计数每次 `show` 清零。
- 测试：同一 `XDG_CACHE_HOME` 连续两次进入 Alpha 详情。冷启动 `cache_hits=0`，`thumb_load_ms=61–63 ms`；热启动 `cache_hits=1`，`thumb_load_ms=25–29 ms`（本机 WSL2 单次值，仅作缓存路径生效的证据，不作为性能阈值）。缓存文件数 1。
- `RH_PERF=1` 时 `perf: first_thumb_ms=... thumb_load_ms=... cache_hit=...` 可用于后续基线；`first_thumb_ms` 含 UI 调度噪声，判缓存是否生效看 `cache_hit`。

拖拽：

- 主窗口安装 `GtkDropTarget`，只接收 `GDK_TYPE_FILE_LIST`；`on_drop` 过滤无本地路径的 URI，把多个 `GFile` 转成 `GPtrArray<gchar*>`，复用 `start_import_paths()` / 现有两阶段导入、进度、取消、关闭保护。
- 文件夹按钮和拖拽走同一个入口；多选时状态栏显示 `Scanning N items`。
- 验收脚本 `tools/m4-drop-check.sh`：两个目录分别 60 + 40 个 NES，合成 `GdkFileList` 调 `on_drop`，`planned=100 imported=100 rows=100`，进度回调最大 19.3–22.9 ms；未使用真实指针拖拽，脚本注释明确标注。

### 8.7 启动游戏（本轮补做）

- 配置：`data/emulators.conf` 出厂只放下注释示例，不猜模拟器路径；优先级 `/usr/share/rom-hamster/emulators.conf` → `/etc/rom-hamster/emulators.conf` → `~/.config/rom-hamster/emulators.conf`，后者覆盖前者。
- 格式 `平台ID: 命令模板`；`%ROM%` / `{file}` 替换为选中 ROM，`%DIR%` / `{dir}` 替换为游戏目录；模板缺 ROM 占位符时明确报错，不静默追加参数。
- 平台没有条目时退回配置键 `emulator_launch`（全局兜底）；两者都空时报 `no emulator command for platform`。
- ROM 选择：多碟优先存在的 `.m3u`，否则第一个存在的 ROM；没有可用 ROM 报错。
- 启动：`g_shell_parse_argv` + `g_spawn_async(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD)`，工作目录为游戏目录；`g_child_watch_add` 回收子进程，避免 GUI 僵尸。
- GUI 详情页 HeaderBar 增加 `Launch` 按钮与结果/错误标签；CLI 新增 `rom-hamster run <game-id>`，成功打印实际 ROM，失败打印核心错误并返回 1。
- 验收 `tools/m4-launch-check.sh`：CLI Alpha 成功、`%ROM%` 落到命令；多碟 Beta 选 `Beta.m3u`；GUI smoke 真实点击 Launch 后状态为 `Launched: Alpha.nes` 且命令收到 ROM；无 `emulators.conf` 时明确失败。结果 **M4 LAUNCH: ALL PASS**。
- 未闭环：没有随包内置具体模拟器命令；用户必须按本机 RetroArch 核心/独立模拟器路径填写 `emulators.conf`。真实模拟器进程能否显示窗口未在 CI 中验证，测试用 `/bin/sh` 替身只验证参数传递与 spawn。
