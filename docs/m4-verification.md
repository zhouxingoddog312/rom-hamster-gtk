# M4 验收记录：首次运行 + 主窗口 + 详情页

日期：2026-11-12　基线：`ae16ec7`（M3）

## 1. 两步的范围

| 步 | 内容 |
|---|---|
| 1 | 首次运行弹 `GtkFileDialog` 选 `ROM_ROOT`；写用户配置、建库、直接进主窗口；`GtkColumnView` 五列；搜索 100 ms 防抖走 `rh_query_list()` |
| 2 | 列表激活进入详情页：`GtkGrid` 元数据、只读描述、`GtkStackSwitcher` + `GtkStack` 的 8 组资源、图片异步缩略图、视频内嵌 `GtkVideo` + 外部打开按钮、返回列表 |
| 2 | 详情页与列表在同一个主窗口 `GtkStack` 内切换，不另开窗口 |

**明确未做**：缩略图磁盘缓存、拖拽导入、导入进度/取消、启动游戏、10k 条性能指标、真实视频后端的播放验证。

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

- AddressSanitizer 构建 `build-gui-asan` 跑详情页 + 返回，无 ASAN 报错。ASAN 下 1.2 s 内缩略图未完成（`thumbs=0/1`），属测试等待时间不足，普通构建为 `1/1`。
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
| 6 | smoke 在 `show_detail()` 后立刻打印 `thumbs`，异步解码还没回来，误报 0 | 测试输出顺序 | smoke 延迟 1.2 s 再打印并退出 |

已知未消除的编译警告：`gdk_texture_new_for_pixbuf` 在 GTK 4.22 标记 deprecated。当前基线 GTK 4.10 仍可用，暂保留，未改用 `gdk_memory_texture_new`（后者需要处理 pixbuf 的 RGB/RGBA 格式差异）。

## 5. 详情页实现要点

- 元数据字段：Platform / Year / Genre / Developer / Publisher / Region / Players / Rating / Status / Format / Multi-disc / Patch / DAT / Directory，值可选中复制。
- 资源组固定顺序：`rom`、`covers`、`screenshots`、`videos`、`marquees`、`miximages`、`intro`、`patcher`，与 `rh_detail_group_kind()` 一致，不重复实现目录规则。
- 图片在 `GTask` 线程池里用 `gdk_pixbuf_new_from_file_at_scale(400,300)` 解码，主线程只做 `GdkTexture` 绑定；UI 不等待磁盘。
- 视频用 `GtkVideo` 内嵌，旁边常驻 “Open externally” 按钮；无媒体后端时用户仍可外部打开，界面有提示。
- 描述是只读 `GtkTextView`，换行按 `WORD_CHAR`，不解释标记、不做富文本。
- 文件缺失在图片页和文件页都标 `(missing)`，不隐藏。

## 6. 仍未闭环

1. 缩略图缓存（`$XDG_CACHE_HOME/rom-hamster/thumbs/`，键 = 路径 + mtime + 尺寸）未做；当前每次进详情页重新解码。
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
