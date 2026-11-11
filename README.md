# rom-hamster（C + GTK4）

复古游戏 ROM 管理系统的 C 重写版。两个二进制共享一个不含 GTK 的核心库：

| 产物 | 用途 | 依赖 |
|---|---|---|
| `libromham-1.0.so` | 核心库：存储、分类、归档、DAT、导入、检索、导出 | glib / sqlite3 / libarchive / libxml2 / zlib |
| `rom-hamster` | CLI 前端，全功能 | 同上 |
| `rom-hamster-gui` | GTK4 图形前端 | 同上 + GTK 4.10 |

## 当前状态

**M1 存储与规则已完成**（2026-11-11）：`store_*` 全量 API、`platforms.conf` 五段规则
编译、三包 deb 布局与首个可安装 deb 均已落地，验收 14/14 通过。
验收明细见 `docs/m1-verification.md`，一键复跑 `tools/m1-check.sh`。

里程碑进度见 `ARCHITECTURE.md` §14：M1 存储与规则（含首个可安装 deb）→ M2 导入 →
M3 检索/管理/导出 → M4 GUI → M5 打包与发布。

## 文档

| 文件 | 内容 |
|---|---|
| `ARCHITECTURE.md` | 架构与依赖说明（v0.2，逐板块过审后的基线）。查议题请先看文首 §0 决策索引 |
| `docs/rules-verification.md` | `platforms.conf` 与 `frontends/*.conf` 的一手来源、修正清单、刻意不采纳项、存疑项 |
| `docs/m0-verification.md` | M0 验收记录：环境、结果、与文档的偏差 |
| `docs/m1-verification.md` | M1 验收记录：14 项证据、与 `ARCHITECTURE.md` 的 6 处偏差及理由、修掉的 8 个真实缺陷 |
| `data/platforms.conf` | 101 个平台规则（5 段：扩展名/街机后缀/媒体目录/媒体后缀/归档后缀） |
| `data/frontends/es.conf` | EmulationStation（RetroPie）导入 + 导出规格 |
| `data/frontends/es-de.conf` | ES-DE 导入 + 导出规格 |
| `data/frontends/pegasus.conf` | Pegasus 导入 + 导出规格 |

## 平台基线

- CLI：glib ≥ 2.66、sqlite3 ≥ 3.35（FTS5 trigram）、libarchive ≥ 3.4、libxml2 ≥ 2.9
- GUI：GTK ≥ 4.10
- 仅面向现代 Linux 桌面发行版；不支持 NAS / 容器 / Entware 交付，不做格式转换、不做刮削

## 从 1.0.0（bash 版）继承什么

**不迁移任何旧文件，也不继承旧测试**。详见 `ARCHITECTURE.md` §12。
不提供 `import-csv`，不读取 `romhamsterdb.csv` 与 `~/.rom-hamster.conf`。
1.0.0 用户升级等于从零建库；旧目录与媒体文件仍在磁盘上，但不会被识别为已有记录。

## 构建

```sh
# 完整构建（需要 GTK >= 4.10）
meson setup build -Dgui=true
ninja -C build
meson test -C build

# 纯 CLI（不含 GTK 依赖的机器）
meson setup build-nogui -Dgui=false
ninja -C build-nogui

# M1 全部验收项（含 deb 构建与安装，需 root 才能跑安装段）
tools/m1-check.sh

# 只跑源码与单测，不构建 deb
SKIP_DEB=1 tools/m1-check.sh
```

打包（产出三个 deb，落在 `dist/`）：

```sh
dpkg-buildpackage -us -uc -b
# rom-hamster_0.1.0_amd64.deb / rom-hamster-gui_0.1.0_amd64.deb / rom-hamster-data_0.1.0_all.deb
sudo dpkg -i dist/*.deb
```

`debian/rules` 是 Debian 强制要求的 make 入口，内部只调 `dh --buildsystem=meson`；
工程本身仍是 Meson + Ninja，没有自维护 Makefile。

依赖安装（Debian/Ubuntu）：

```sh
sudo apt install build-essential meson ninja-build pkg-config \
  libglib2.0-dev libsqlite3-dev libarchive-dev libxml2-dev zlib1g-dev \
  libgtk-4-dev
```

## 许可

GPL-3.0-or-later。维护者：Zhou Xin <zhouxin312@qq.com>。
