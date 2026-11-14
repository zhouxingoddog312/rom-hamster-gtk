# rom-hamster v0.1.0 发行说明

- 发行日期：2026-11-14
- tag：`v0.1.0`（annotated）
- 性质：首个正式发行版（此前的 `v0.1.0-M5` 是里程碑预发布）

## 概览

rom-hamster 是一个本地优先的复古游戏 ROM 库管理器。核心库
`libromham-1.0.so` 不依赖 GTK，命令行前端 `rom-hamster` 与 GTK4 前端
`rom-hamster-gui` 共享同一套存储、分类、导入、检索与导出实现。

这一版覆盖了 M1–M5 的全部范围：平台规则、导入、检索与管理、导出、GUI、
国际化与打包发布。

## 功能

### 平台与库

- 101 个平台规则，分 5 段：文件扩展名、街机后缀、媒体目录、媒体后缀、
  归档扩展名。
- 规则按“出厂 → 系统 → 用户”三层覆盖；`rom-hamster config --explain KEY`
  能看到最终取值来自哪个文件的第几行。
- 库是单个 SQLite 文件（schema v1），写入走事务，级联删除不留孤儿记录。
- 关键字检索使用 FTS5 trigram；构建环境没有该分词器时自动回落到 LIKE。

### 导入

- 单 ROM、zip / 7z / rar / tar 系列压缩包、旁路媒体文件与 Logiqx XML DAT。
- 两阶段导入：先扫描计划、再落盘；支持取消，进程被 `kill -9` 后可续做。
- 二义扩展名给出候选集让用户确认，不静默归类。
- 多碟游戏按文件名规则合组，重复文件按内容哈希检测。
- 媒体按平台规则落到对应目录；视频优先使用可内嵌播放的格式。

### 检索与管理

- 13 个筛选维度（平台、类型、厂商、发行、地区、年份、标签、多碟、补丁、
  格式、关键字等）与 4 种排序。
- 详情页展示元数据与 8 组媒体，缩略图落盘缓存，冷/热启动都可复用。
- 编辑、删除、导出等写操作都经过同一套 CLI 命令，GUI 与 CLI 行为一致。

### 导出

- EmulationStation、ES-DE、Pegasus 三套前端清单，模板随 `rom-hamster-data`
  分发；支持只导出指定平台与可选媒体字段。

### 命令行

`init`、`import`、`dat`、`list`、`search`、`show`、`edit`、`delete`、
`export`、`stats`、`config`、`help`、`version` 共 13 个命令；
不带参数时给出一行提示，未知命令以状态码 2 退出。

### 图形界面

- GTK4 列表与详情两栏，列表按平台/年份/碟数展示。
- 支持从文件管理器拖入文件或目录导入，导入过程有进度与取消。
- 详情页视频通过媒体后端内嵌播放；后端不可用时显式降级为外部打开，
  不崩溃、不静默失败。

### 国际化与文档

- msgid 使用英文，`zh_CN` 目录完整；GTK `.ui` 字符串也在翻译范围内。
- 两个前端各有一份 man page；AppStream metainfo 含真实截图。

### 打包

- 三个二进制包：`rom-hamster`（CLI + 私有库）、`rom-hamster-gui`（GTK 前端）、
  `rom-hamster-data`（平台规则、前端模板与文档）。
- `/etc/rom-hamster` 下的配置登记为 conffile，升级保留管理员修改。
- 私有库目录 `/usr/lib/<triplet>/rom-hamster`，两个前端都带 rpath，
  不污染发行版的公共库命名空间。
- lintian 无 error / warning。

## 发行产物

Linux 发行版的 libxml2 包在 Ubuntu 26.04 上升级为 `libxml2-16`（soname 16），
而 Ubuntu 24.04 与 Debian 13 仍是 `libxml2`（soname 2），同一个 deb 无法同时
满足两端，因此本版按发行版提供三套：

| 适用系统 | 附件 | 关键依赖 | 验证来源 |
|---|---|---|---|
| Ubuntu 24.04 LTS | `rom-hamster-0.1.0-ubuntu-24.04-debs.tar.xz` | `libxml2`（2.9.14，soname 2） | CI 内真实 `dpkg -i` → CLI/zh_CN → conffile 升级 → `dpkg -P` |
| Ubuntu 26.04 | `rom-hamster-0.1.0-ubuntu-26.04-debs.tar.xz` | `libxml2-16`（2.15.2，soname 16） | 本机构建 + `dpkg --root` 安装 + 包内 CLI/GUI 冒烟 |
| Debian 13 | `rom-hamster-0.1.0-debian-13-debs.tar.xz` | `libxml2`（2.12.7+dfsg+really2.9.14，soname 2） | CI 内真实 `dpkg -i` → CLI/zh_CN → conffile 升级 → `dpkg -P` |

三套都包含 `rom-hamster`、`rom-hamster-gui`、`rom-hamster-data` 三个包；
压缩包内的 `README.txt` 写明适用系统与安装顺序。源码包为
`rom-hamster-0.1.0.tar.xz`，`SHA256SUMS` 覆盖全部附件。

## 安装

```sh
tar xf rom-hamster-0.1.0-<发行版>-debs.tar.xz
cd rom-hamster-0.1.0-<发行版>-debs
sudo apt install ./rom-hamster-data_0.1.0_all.deb \
                 ./rom-hamster_0.1.0_amd64.deb \
                 ./rom-hamster-gui_0.1.0_amd64.deb
```

只装命令行前端可以跳过 `rom-hamster-gui`。卸载：

```sh
sudo dpkg -P rom-hamster-gui rom-hamster rom-hamster-data
```

## 验证

- 单元测试 5 套件；M1 / M2 / M3 验收脚本；GUI 列表/搜索/详情、导入进度与
  取消、真实指针拖拽、合成拖放全部通过。
- ASan/UBSan 构建通过；CI 矩阵 `{gui, nogui, asan} × {ubuntu:24.04, debian:13}`
  共 8 个 job 全部通过。
- 两个发行版的打包 job 均执行 lintian、安装、`--version` 与 zh_CN catalog
  校验、conffile 升级保留、purge 无残留。
- `appstreamcli validate`、`msgfmt --check`、`desktop-file-validate` 通过。

## 已知限制

- 不启动游戏、不做刮削、不做格式转换；这些交给前端与模拟器。
- 性能基线（冷启动、首屏、缩略图、内存）取自 M4 的参考机记录；
  CI 只自动检查首屏与内存增长，不作为绝对性能承诺。
- desktop entry 的 `Categories=Utility;Game;` 会让菜单校验给出一条 hint，
  不是 error。
- Ubuntu 26.04 的包在本机构建与隔离 root 安装验证，未做系统级 `dpkg -i`
  （构建机没有 root）；Ubuntu 24.04 与 Debian 13 由 CI 覆盖真实安装。

## 许可

GPL-3.0-or-later。维护者：Zhou Xin <zhouxin312@qq.com>。
