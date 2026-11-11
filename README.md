# rom-hamster（C + GTK4）

复古游戏 ROM 管理系统的 C 重写版。两个二进制共享一个不含 GTK 的核心库：

| 产物 | 用途 | 依赖 |
|---|---|---|
| `libromham-1.0.so` | 核心库：存储、分类、归档、DAT、导入、检索、导出 | glib / sqlite3 / libarchive / libxml2 / zlib |
| `rom-hamster` | CLI 前端，全功能 | 同上 |
| `rom-hamster-gui` | GTK4 图形前端 | 同上 + GTK 4.10 |

## 当前状态

**设计阶段**。本仓库目前只有架构文档、平台规则与前端规格三类文件，**尚无代码**；
M0（骨架构建）之后的里程碑见 `ARCHITECTURE.md` §14。

## 文档

| 文件 | 内容 |
|---|---|
| `ARCHITECTURE.md` | 架构与依赖说明（v0.2，逐板块过审后的基线）。查议题请先看文首 §0 决策索引 |
| `docs/rules-verification.md` | `platforms.conf` 与 `frontends/*.conf` 的一手来源、修正清单、刻意不采纳项、存疑项 |
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

## 构建（M0 之后可用）

```sh
meson setup build -Dgui=true
ninja -C build
meson test -C build
```

## 许可

待定（1.0.0 的许可与维护者信息以最终发布为准）。
