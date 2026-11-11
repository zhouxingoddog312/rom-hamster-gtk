# M4 验收记录（第一步）：首次运行 + 主窗口列表/搜索

日期：2026-11-11　基线：`ae16ec7`（M3）

## 1. 本次落地范围

| 项 | 内容 |
|---|---|
| 首次运行 | 启动建 `RhCtx`；`ROM_ROOT` 为空则弹 `GtkFileDialog` 选目录；选完写用户配置、建库，直接进主窗口，无欢迎页 |
| 配置保存 | 新增 `rh_config_save_user_key()`：原位替换 `ROM_ROOT`，保留其他键与注释，原子写 |
| 主窗口 | `.ui` + `GResource` 编译进二进制；`GtkColumnView` 五列（名称/平台/年份/题材/评分） |
| 列表模型 | `RhGameListModel` 实现 `GListModel`，只复制行展示所需字段，不复制描述 |
| 搜索 | 搜索框 200 ms 防抖，走 `rh_query_list()`（与 CLI 同一条查询，不新增 SQL） |
| 测试旁路 | `RH_SMOKE_ROOT` 走同一条 `open_library()` 路径，供无显示器环境验收；真实文件选择器不做自动化 |

**明确未做**：详情页、8 类媒体分页、缩略图缓存、拖拽导入、导入进度/取消、启动游戏。这些是 M4 后续步骤。

## 2. 验收结果

入口：`bash tools/m4-gui-check.sh`（xvfb）

```
build                  PASS  ninja -C build-gui
核心库                 PASS  ldd libromham 无 gtk/gdk/pango
unit tests             PASS  meson test 6 suites（含 config-save）
fixture                PASS  3 games
首次运行空目录         PASS  窗口 map、0 行、romhamster.db 已建、ROM_ROOT 已写
已有库                 PASS  3 行
搜索 Alpha             PASS  1 行
搜索 zzzz              PASS  0 行
M4 GUI STEP1: ALL PASS
```

deb 回归：`SKIP_DEB=0 bash tools/m2-check.sh` → 43/43，三 deb 构建成功；解包后的 `rom-hamster-gui` 在 xvfb 下加载 3 行（证明 `.ui` 资源已编进包）。M3 回归 `SKIP_DEB=1 bash tools/m3-check.sh` 全绿。

## 3. 本步修掉的真实缺陷

| # | 缺陷 | 发现方式 | 修法 |
|---|---|---|---|
| 1 | `GtkStack` 两个 child 没有名字，`set_visible_child_name()` 报 `Child name 'empty' not found` | 首启 smoke 的 Gtk-WARNING | 建窗口后 `gtk_stack_page_set_name()` 设 `list`/`empty` |
| 2 | `gui_new()` 没保存 `GtkApplication` 指针，`g_application_quit(G_APPLICATION(NULL))` 断言失败 | smoke 退出时 GLib-CRITICAL | 结构体保存 `app` |
| 3 | `open_library()` 先 `rh_config_set("ROM_ROOT")`（释放同键旧值），再把原 `root` 指针传给保存函数 → use-after-free，写出乱码配置并建出乱码目录 | 第二次启动读回 `ROM_ROOT=�� ISd`，项目根出现乱码目录 | 先 `g_strdup`，先落盘再覆盖运行时配置 |
| 4 | `GtkSearchEntry::search-changed` 在程序化 `gtk_editable_set_text()` 时不触发，搜索用例不更新 | smoke 搜索 2 行不变 | 改接 `GtkEditable::changed` + 仍保留 200 ms 防抖 |
| 5 | GUI 链接私有 soname `libromham-1.0.so`，`dpkg-shlibdeps` 找不到 shlibs 信息，deb 构建失败 | `SKIP_DEB=0` 回归 | `debian/rules` 覆盖 `dh_shlibdeps -- --ignore-missing-info`；GUI 已显式 `Depends: rom-hamster (= ${binary:Version})` |

## 4. 已知未闭环

1. 真实 `GtkFileDialog` 的人机点选没有自动化；测试只覆盖其后的 `open_library()` 路径。
2. 列表尚未接排序表头、筛选侧栏；当前固定 `sort=name`。
3. 10k 条性能指标（冷启动、首屏 300 ms、滚动 p95、搜索 200 ms）尚未测，属 M4 性能验收，不能拿 3 条夹具冒充。
4. 详情页与媒体分页未开始。
