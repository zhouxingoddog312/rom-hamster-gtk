# rom-hamster（C + GTK4）架构与依赖说明

> 版本：v0.3（在 v0.2 基线上增补 §7.5 刮削扩展点契约；只留契约，实现暂不排期）
> 日期：2026-11-11
> 对照基线：bash 版 rom-hamster 1.0.0 —— 15 个模块、198 个函数、100 个平台规则、28 字段 CSV、76 个单元测试、deb 打包
> 本文所有"现状"结论均来自对 1.0.0 源码的逐文件核对，不依赖记忆。

## 0. 决策索引（按议题查章节）

| 议题 | 结论 | 章节 |
|---|---|---|
| 交付形态 | 两二进制（`rom-hamster` CLI + `rom-hamster-gui`）+ 共享 `libromham` | §1.1 |
| 目标平台 | 仅现代 Linux 桌面；CLI 需 glib 2.66/sqlite 3.35；GUI 需 GTK 4.10 | §1.3 |
| 排除项 | 不做格式转换、不打补丁、无 Web/服务端、无 NAS 与容器交付；首个版本不实现刮削 | §1.2 §1.3 |
| 刮削 | 只留扩展点契约（规则文件 + `RhScraper` vtable + 来源字段），**暂不实现、不排期** | §7.5 |
| 元数据存储 | SQLite 单文件，7 表 + FTS5 trigram，类型化三原则 | §3 |
| `tags` | 规范化到 `tags` + `game_tags` | §3.5 |
| 哈希 | `crc32`/`md5`/`sha1` 三列，导入只算 `dat_match_hash` 指定的一种 | §3.5 |
| DAT | 全量入 `dats`/`dat_entries`，不保留扫文件路径 | §3.5 |
| 事务 | 每条目一个事务（记录 + `job_items` 状态同事务） | §3.5 §11 |
| 并发 | 不加锁文件，靠 SQLite 串行化写 + `busy_timeout=5000` | §13 |
| 模块边界 | `classify` 只判定不搬文件、`query` 只读、`store` 唯一持有 SQL | §5 |
| 导入流程 | 两阶段：预扫描（可交互）→ 执行（零交互） | §5 §6.2 |
| 线程 | 主线程跑 GTK 与交互；工作线程只调核心，禁止碰 GTK | §6.2 |
| 前端规格 | 每前端一份 `.conf`，模板优先、vtable 兜底；解析期报错带行号 | §7 |
| gamelist.xml | `xmlDoc` + XPath；单文件过大则报错要求拆分 | §7.2 |
| Pegasus | 记录以 EOF 收尾、描述支持续行、补 `assets.screenshot`/`assets.titlescreen`（注意全小写） | §7.2 |
| 图标 | 沿用 1.0.0 的 SVG 与 png 套件，不重画 | §1.1 |
| 二进制命名 | CLI `rom-hamster`、GUI `rom-hamster-gui`，不做 bash 版兼容 | §9.3 |
| 打包 | 三包 `rom-hamster` / `-gui` / `-data`；conffile 归位 `/etc` | §9 |
| 配置 | 五层合并 + 来源记录 + `config --explain`；平台规则四层（含用户层） | §10 |
| 旧版数据 | **不迁移**：不读 CSV、不读旧 conf、不继承测试 | §12 |
| i18n | 英文 msgid + `zh_CN` `.po` | §11 |
| 首启与拖拽 | GUI 启动即弹目录选择；`GtkDropTarget` 走同一导入管线，进 M4 | §11 |
| 里程碑 | M0–M5，M1 起每阶段出 deb，M3→M4 串行 | §14 |

---


## 1. 目标与边界

### 1.1 交付形态

一款 Linux 桌面应用，同时提供等价的无界面运行能力：

| 产物 | 说明 | 图形依赖 |
|---|---|---|
| `rom-hamster`（CLI） | 全部功能可通过命令与子命令使用；可脚本化、可在纯命令行系统运行 | 无 |
| `rom-hamster-gui` | 同一套核心，GTK4 前端 | GTK4 |
| `libromham` | 核心库，两个前端共享，不含任何 GTK 符号 | 无 |

硬约束（违反即视为架构错误）：

1. `libromham` 的链接依赖中不得出现 `gtk4`/`gdk`/`pango`。
2. `libromham` 不得打印面向用户的文字，不得读 stdin，不得自行弹窗。所有交互经 `rh_ui` 回调（见 §6）。
3. `libromham` 不得持有全局可变状态（替代 bash 版散落在各模块的全局变量），全部经 `RhCtx *` 传递。
4. 除 `store.c` 外，其他源文件不得出现 SQL 语句。

### 1.2 非目标

- 不下载 ROM、不提供 ROM 来源、不接入私有镜像。
- **不自动打补丁**（继承 1.0.0 原则：补丁只做存储、关联、类型标注，绝不执行）。
- 不做 ROM 格式转换（`chd`/`cso`/`wbfs` 仅作为 `rom_format` 的取值被记录；1.0.0 中也无任何 `chdman`/`cso`/`wbfs` 调用，此处保持一致）。
- 不做在线元数据刮削（ScreenScraper / IGDB 等留作 §7 的代码型扩展点，不进入首个版本）。
- 不提供 Web UI / 服务端模式。

### 1.3 目标平台基线（已定）

只支持现代 Linux 桌面发行版，不追求 NAS、容器、老 glibc 环境的兼容。

| 组件 | 依赖下限 | 覆盖的典型系统 |
|---|---|---|
| CLI（`libromham`） | glib 2.66、sqlite 3.35、libarchive 3.4、libxml2 2.9 | Debian 12、Ubuntu 22.04 LTS、Fedora 36+、UOS V25 |
| GUI（`rom-hamster-gui`） | 上述 + **GTK 4.10** | Ubuntu 24.04 LTS、Debian 13、Fedora 38+、UOS V25（GTK 4.16.7） |

直接后果：**Debian 12（GTK 4.8）与 Ubuntu 22.04（GTK 4.6）只能用 CLI**。GUI 包声明 `libgtk-4-1 (>= 4.10)`，`apt` 在这两个系统上会直接拒绝安装，而不是装完后运行时报错。这是明确取舍，不是遗漏。

明确排除：NAS 系统（群晖 DSM / 威联通 QTS 等）、OCI 容器交付、Entware/opkg 路径、glibc < 2.28 的老平台。相关调研数据保留在 §8.3.2–§8.3.3，仅供追溯，不再投入工程资源。

---

## 2. 分层与目录结构

### 2.1 分层

| 层 | 内容 | 依赖方向 |
|---|---|---|
| L0 基础 | 错误码、字符串、路径、哈希、配置、日志 | 只依赖 GLib/zlib |
| L1 存储 | `store.c`（唯一持有 SQL）、schema 迁移 | 依赖 L0 + libsqlite3 |
| L2 领域 | 平台规则、分类、压缩包、DAT、导入、整库、导出、检索、详情、管理 | 依赖 L0+L1 |
| L3 任务 | 导入/导出作业、进度、取消、持久化队列 | 依赖 L2 |
| L4 前端 | CLI（`src/cli`）、GTK4（`src/gui`） | 依赖 L0–L3 |

依赖只能向下，禁止 L2 反向依赖 L3/L4。任务进度经 L3 的回调上报，L2 内部不感知前端。

### 2.2 目录

```
c与gtk/
├─ meson.build
├─ meson_options.txt          # gui=true/false, tests=true/false
├─ README.md
├─ ARCHITECTURE.md            # 本文
├─ data/
│  ├─ platforms.conf          # 101 平台规则，格式原样继承
│  ├─ frontends/
│  │  ├─ es.conf              # 每份"前端规格"同时描述导入与导出：
│  │  ├─ esde.conf            #   [export.*] 排版模板 + [import] 字段映射
│  │  └─ pegasus.conf
│  ├─ rom-hamster.conf.template
│  ├─ io.github.goddog312.RomHamster.desktop
│  ├─ io.github.goddog312.RomHamster.metainfo.xml
│  ├─ rom-hamster.svg  +  icons/{16,32,48,256}/rom-hamster.png
│  └─ rom-hamster.1
├─ src/
│  ├─ core/
│  │  ├─ rh-ctx.h/.c        # RhCtx：配置、存储句柄、ui 回调、日志、缓存目录
│  │  ├─ rh-err.h/.c        # RhErr 错误码 + 可读消息
│  │  ├─ rh-str.h/.c        # 扩展名/复合扩展名、核心名归一、相似度、清洗目录名
│  │  ├─ rh-fs.h/.c         # 递归遍历、原子写、安全复制、路径校验
│  │  ├─ rh-hash.h/.c       # crc32/md5/sha1 单次读盘三算
│  │  ├─ rh-config.h/.c     # 5 级配置加载与持久化
│  │  ├─ platform.h/.c      # 规则解析、平台判定、媒体识别、导出平台名映射
│  │  ├─ classify.h/.c      # 单文件分类分流
│  │  ├─ archive.h/.c       # libarchive：列表/解压/加密探测/打包
│  │  ├─ store.h/.c         # 唯一 SQL 入口
│  │  ├─ store-schema.h     # DDL + 迁移版本表
│  │  ├─ dat.h/.c           # Logiqx DAT 导入与哈希匹配
│  │  ├─ library.h/.c       # gamelist.xml / metadata.pegasus.txt 整库识别
│  │  ├─ import.h/.c        # 导入编排：队列、归属、命名、m3u、补丁类型
│  │  ├─ export.h/.c        # 前端导出
│  │  ├─ query.h/.c         # 检索/排序/筛选
│  │  ├─ detail.h/.c        # 只读详情（CLI 纯文本；GUI 走 window-detail）
│  │  ├─ manage.h/.c        # 编辑/删除/碟片/补丁/统计/完整性
│  │  ├─ job.h/.c           # 作业、进度、取消、持久化队列
│  │  └─ plugin.h/.c        # 扩展点注册表（编译期）
│  ├─ cli/
│  │  ├─ main.c             # GOptionContext 解析，子命令分发
│  │  ├─ ui-cli.c           # rh_ui 的终端实现
│  │  └─ cmd-*.c            # 各子命令
│  └─ gui/
│     ├─ main.c             # GtkApplication，单实例 + 远程激活
│     ├─ ui-gtk.c           # rh_ui 的 GTK 实现
│     ├─ window-main.c      # GtkColumnView 列表 + 筛选栏
│     ├─ window-detail.c    # 详情 + 图片/视频预览
│     ├─ dialog-*.c         # 确认/选平台/选图片类型/元数据表单
│     └─ ui/*.ui            # GtkBuilder 文件，经 GResource 编译
├─ po/                       # gettext
├─ tests/
│  ├─ unit/                  # GLib GTest
│  └─ fixtures/              # 构造的小样本 ROM 目录
├─ debian/
└─ tools/
```

---

## 3. 数据模型

### 3.1 存储选型

**SQLite3**（`libsqlite3-0`，公共领域授权）。理由记录在案，便于日后复查：

- 进程内库 + 单文件，无守护进程、无端口、无账号体系，与桌面应用语义匹配。
- 单文件即全库：备份 = 复制，迁移 = 换路径，外部工具可读。
- 具备 schema 约束、事务、索引、FTS5 —— 这是 bash 版 CSV 完全缺失的部分。
- 1.0.0 的两个真实性能瓶颈可直接消解：启动时逐行 fork `awk` 解析 CSV（`db.sh` 的 `parse_csv_line` + `db_load_cache`），以及每次单条写入都整表重写（`db_upsert_F` → `db_flush`）。

不采用 PostgreSQL：单机单用户场景下需要用户 `initdb`、起服务、管端口与 `pg_hba`，成本转嫁给用户，收益（多写并发、远端连接）用不上。留作 §13 的未来分叉。

不采用 gdbm/LMDB/BerkeleyDB 作为主库：它们是键值层，与现有 `DB_ALL[id] = 记录串` 属同一抽象层，换成它们等于只换存储介质而查询能力不变 —— 13 个筛选维度仍需全表扫描手写判定，且无 schema、无事务、无排序索引。这类库适合做纯索引（见 §7.4），不适合主库。

连接参数：

```c
"PRAGMA journal_mode=WAL;"
"PRAGMA foreign_keys=ON;"
"PRAGMA synchronous=NORMAL;"
"PRAGMA busy_timeout=5000;"
```

库文件位置：`<ROM_ROOT>/romhamster.db`。**库文件必须位于本地文件系统**，不得放在 SMB/NFS 上（SQLite 的锁依赖 POSIX 语义，见 §13）。

### 3.2 28 字段 → 表结构（逐字段对照）

1.0.0 的 `CSV_FIELDS` 共 28 项：其中 8 项是以分号分隔的多值字段（`rom_files` + 7 个媒体字段）、1 项（`tags`）是逗号分隔列表，共 9 项拆入子表；其余 19 项为标量，落在 `games` 表。

| # | 原字段 | 新位置 | 处理 |
|---|---|---|---|
| 1 | `id` | `games.id` | 原样保留（小写字母数字 8 位），不重新生成 |
| 2 | `rom_files` | `roms.filename` × N | 分号拆分；`disc_no` 从碟名解析（原 `parse_disc_name`） |
| 3 | `game_main_name` | `games.game_main_name` | 命名基准 |
| 4 | `platform` | `games.platform` | 规范平台 ID |
| 5 | `is_multi_disc` | `games.is_multi_disc` | 文本 `true/false` → `1/0` |
| 6 | `is_arcade` | `games.is_arcade` | 同上 |
| 7 | `rom_format` | `games.rom_format` | `raw/zip/7z`，为 `chd/cso/wbfs` 预留取值 |
| 8 | `rom_status` | `games.rom_status` | `good/baddump/unknown`，加 CHECK 约束 |
| 9 | `dat_source` | `games.dat_source` | 匹配来源 DAT 名 |
| 10 | `name` | `games.name` | 前端显示名 |
| 11 | `desc` | `games.description` | 更名（`desc` 易与 SQL 语义混淆） |
| 12 | `developer` | `games.developer` | |
| 13 | `publisher` | `games.publisher` | |
| 14 | `year` | `games.year` | `INTEGER`，空串 → `NULL` |
| 15 | `genre` | `games.genre` | |
| 16 | `players` | `games.players` | `INTEGER`，空串 → `NULL` |
| 17 | `rating` | `games.rating` | `REAL`，空串 → `NULL` |
| 18 | `region` | `games.region` | |
| 19 | `tags` | `tags` + `game_tags` | 逗号拆分并规范化，便于按标签筛选 |
| 20 | `dir_path` | `games.dir_path` | `<platform>/<游戏目录>`，加 UNIQUE |
| 21 | `cover_filename` | `media(kind='covers')` | 分号拆分 |
| 22 | `screenshot_filename` | `media(kind='screenshots')` | 分号拆分 |
| 23 | `video_filename` | `media(kind='videos')` | 分号拆分 |
| 24 | `marquee_filename` | `media(kind='marquees')` | 分号拆分 |
| 25 | `miximage_filename` | `media(kind='miximages')` | 分号拆分 |
| 26 | `intro_filename` | `media(kind='intro')` | 分号拆分 |
| 27 | `patcher_filename` | `media(kind='patcher')` | 分号拆分 |
| 28 | `patch_type` | `games.patch_type` | `''/generic/platform/mixed`，由补丁集合重算 |

类型转换原则：空串一律转 `NULL`（不是 `''`），否则 `WHERE year IS NULL` 这类查询会失真；布尔一律 `1/0`（不是 `'true'`）。

### 3.3 DDL

```sql
-- schema 版本：PRAGMA user_version
CREATE TABLE games (
  id             TEXT PRIMARY KEY,
  platform       TEXT NOT NULL,
  name           TEXT NOT NULL,
  game_main_name TEXT,
  dir_path       TEXT NOT NULL UNIQUE,
  developer      TEXT,
  publisher      TEXT,
  year           INTEGER,
  genre          TEXT,
  players        INTEGER,
  rating         REAL,
  region         TEXT,
  description    TEXT,
  is_multi_disc  INTEGER NOT NULL DEFAULT 0 CHECK (is_multi_disc IN (0,1)),
  is_arcade      INTEGER NOT NULL DEFAULT 0 CHECK (is_arcade IN (0,1)),
  rom_format     TEXT NOT NULL DEFAULT 'raw',
  rom_status     TEXT NOT NULL DEFAULT 'unknown'
                 CHECK (rom_status IN ('good','baddump','unknown')),
  dat_source     TEXT,
  patch_type     TEXT NOT NULL DEFAULT ''
                 CHECK (patch_type IN ('','generic','platform','mixed')),
  created_at     TEXT NOT NULL,
  updated_at     TEXT NOT NULL
);

CREATE TABLE roms (
  game_id  TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  filename TEXT NOT NULL,
  disc_no  INTEGER,
  size     INTEGER,
  crc32    TEXT,
  md5      TEXT,
  sha1     TEXT,
  sort     INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (game_id, filename)
);

CREATE TABLE media (
  game_id  TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  kind     TEXT NOT NULL CHECK (kind IN
           ('covers','screenshots','videos','marquees','miximages','intro','patcher')),
  filename TEXT NOT NULL,
  size     INTEGER,                -- 字节；导入时 stat 一次，stats 用 SUM(size)
  sort     INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (game_id, kind, filename)
);

CREATE TABLE tags (
  id   INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE
);
CREATE TABLE game_tags (
  game_id TEXT NOT NULL REFERENCES games(id) ON DELETE CASCADE,
  tag_id  INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (game_id, tag_id)
);

-- DAT 索引：导入 DAT 时一次性写入，匹配时按哈希查
CREATE TABLE dats (
  id           INTEGER PRIMARY KEY,
  platform     TEXT NOT NULL,
  path         TEXT NOT NULL,
  name         TEXT,
  version      TEXT,
  imported_at  TEXT NOT NULL,
  entry_count  INTEGER,
  UNIQUE (platform, path)
);
CREATE TABLE dat_entries (
  dat_id    INTEGER NOT NULL REFERENCES dats(id) ON DELETE CASCADE,
  game_name TEXT,
  rom_name  TEXT NOT NULL,
  size      INTEGER,
  crc32     TEXT,
  md5       TEXT,
  sha1      TEXT,
  PRIMARY KEY (dat_id, rom_name)
);

-- 导入作业：崩溃后可续
CREATE TABLE jobs (
  id          INTEGER PRIMARY KEY,
  kind        TEXT NOT NULL,             -- import / export / dat-import
  state       TEXT NOT NULL,             -- queued/running/paused/done/failed/cancelled
  total       INTEGER NOT NULL DEFAULT 0,
  done        INTEGER NOT NULL DEFAULT 0,
  created_at  TEXT NOT NULL,
  finished_at TEXT,
  error       TEXT
);
CREATE TABLE job_items (
  job_id     INTEGER NOT NULL REFERENCES jobs(id) ON DELETE CASCADE,
  seq        INTEGER NOT NULL,
  src        TEXT NOT NULL,
  action     TEXT,                       -- rom / media / patcher / skip
  platform   TEXT,
  target_id  TEXT,
  state      TEXT NOT NULL DEFAULT 'pending',
  message    TEXT,
  PRIMARY KEY (job_id, seq)
);
```

索引：

```sql
CREATE INDEX idx_games_platform  ON games(platform);
CREATE INDEX idx_games_name      ON games(name);
CREATE INDEX idx_games_year      ON games(year);
CREATE INDEX idx_games_rating    ON games(rating);
CREATE INDEX idx_games_status    ON games(rom_status);
CREATE INDEX idx_roms_disc       ON roms(game_id, disc_no);
CREATE INDEX idx_dat_crc32       ON dat_entries(crc32);
CREATE INDEX idx_dat_md5         ON dat_entries(md5);
CREATE INDEX idx_dat_sha1        ON dat_entries(sha1);
```

全文检索（替代 1.0.0 的 `--kw` 子串匹配）：

```sql
CREATE VIRTUAL TABLE games_fts USING fts5(
  name, game_main_name, description,
  content='games', content_rowid='rowid',
  tokenize="trigram"
);
-- 触发器：games 的 INSERT/UPDATE/DELETE 同步到 games_fts
```

`trigram` 分词器（SQLite ≥ 3.34）对中日韩与含数字的 ROM 文件名都能命中子串，优于 `unicode61` 对中文会整串丢失。

### 3.4 schema 版本与升级

`PRAGMA user_version` 记录 schema 版本，`store.c` 内维护 `migration[]` 表：`{version, sql}` 顺序执行，每次升级在单事务内完成。首个版本为 v1。以后加字段只允许"加表/加列/加索引"，不重命名已有列，避免用户数据升级损坏。（这是**结构**升级，与 §12 的数据不迁移是两件事。）

### 3.5 已定决策（不再讨论）

| 项 | 决定 | 理由 |
|---|---|---|
| `tags` | 规范化到 `tags` + `game_tags` | 支持按标签筛选与计数；存逗号串只能 `LIKE`，`rpg` 会误命中 `jrpg` |
| `roms` 哈希 | 三列都建；导入时只算 `dat_match_hash` 指定的那一种（默认 `crc32`），其余留 `NULL`，由 `verify --deep` 按需补算回填 | 反正 DAT 匹配要算同一种，顺手即可；避免导入时三次全量读盘 |
| DAT | 全量入 `dats` / `dat_entries`，**不保留"直接扫 DAT 文件"的路径** | 两条路径必然行为分叉；入表后一次导入、多次索引查询 |
| 事务 | **每个条目一个事务**（记录写入 + `job_items` 状态同事务）；"每 N 条"只作为进度上报频率 | 崩溃最多丢当前条目，且库与队列状态始终自洽；不做跨条目大事务 |
| 全文检索 | FTS5 `trigram`（需 SQLite ≥ 3.34）；启动探测不到则降级 `LIKE` 子串，不报错 | Debian 12 = 3.40、Ubuntu 22.04 = 3.37 均满足；降级只损失子串命中精度 |

---

## 4. `store_*` API

约定：

- 全部返回 `RhErr`（`RH_OK` / 错误码 + `char msg[]`）。不使用 `assert` 处理外部输入（磁盘、用户、DAT 文件都可能给出脏数据）。
- 不带 `_F`、`_F` 这类"操作当前记录全局数组"的隐式语义 —— 1.0.0 的 `fget/fset` 模式在 C 里对应显式结构体 `RhGame`。
- 批量写用事务：`store_tx_begin()` / `store_tx_commit()` / `store_tx_rollback()`。
- 只读查询返回 `GPtrArray<RhGame*>` 或游标，所有权明确写入注释。
- **出参所有权（M1 起统一）**：所有 `**out` 出参由函数分配、调用方释放 —— 结构体用 `rh_*_free`，数组用 `g_ptr_array_unref`（数组自带元素的 `free_func`）。不再要求调用方预置结构体；这一处与 1.0.0 的 `_F` 取址风格有意不同，理由见 `docs/m1-verification.md`。

```c
/* 生命周期 */
RhErr store_open(RhCtx *ctx, const char *db_path, int flags);
void  store_close(RhCtx *ctx);
RhErr store_migrate(RhCtx *ctx);                   /* 依 user_version 逐级升级 */

/* 事务 */
RhErr store_tx_begin(RhCtx *ctx);                  /* BEGIN IMMEDIATE */
RhErr store_tx_commit(RhCtx *ctx);
RhErr store_tx_rollback(RhCtx *ctx);

/* 游戏记录 */
RhErr store_game_get(RhCtx *ctx, const char *id, RhGame **out);
RhErr store_game_upsert(RhCtx *ctx, const RhGame *g);
RhErr store_game_delete(RhCtx *ctx, const char *id);
RhErr store_game_list(RhCtx *ctx, const RhFilter *f, GPtrArray **out);
RhErr store_game_count(RhCtx *ctx, gint64 *out);

/* 子表 */
RhErr store_rom_add(RhCtx *ctx, const char *game_id, const RhRomFile *r);
RhErr store_rom_remove(RhCtx *ctx, const char *game_id, const char *filename);
RhErr store_rom_list(RhCtx *ctx, const char *game_id, GPtrArray **out);
RhErr store_media_add(RhCtx *ctx, const char *game_id, const char *kind,
                      const char *filename);
RhErr store_media_add_sized(RhCtx *ctx, const char *game_id, const char *kind,
                            const char *filename, gint64 size, int seq);
RhErr store_media_remove(RhCtx *ctx, const char *game_id, const char *kind,
                         const char *filename);
RhErr store_media_list(RhCtx *ctx, const char *game_id, const char *kind,
                       GPtrArray **out);
RhErr store_media_total_size(RhCtx *ctx, const char *game_id, gint64 *out);

/* 标签 */
RhErr store_tags_set(RhCtx *ctx, const char *game_id, const char **tags, int n);
RhErr store_tag_list(RhCtx *ctx, GPtrArray **out);    /* 供筛选下拉用 */
RhErr store_game_tags(RhCtx *ctx, const char *game_id, GPtrArray **out);

/* DAT：XML 解析归 dat.c（§5.1 边界表），store 只负责落库 */
RhErr store_dat_begin(RhCtx *ctx, const char *platform, const char *path,
                      const char *name, const char *version, gint64 *dat_id);
RhErr store_dat_entries_add(RhCtx *ctx, gint64 dat_id, GPtrArray *entries);
RhErr store_dat_finish(RhCtx *ctx, gint64 dat_id, gint64 entry_count);
RhErr store_dat_count(RhCtx *ctx, gint64 *out);
RhErr store_dat_find_by_hash(RhCtx *ctx, const char *algo,
                             const char *hash, GPtrArray **out);

/* 作业 */
RhErr store_job_create(RhCtx *ctx, const char *kind, int total, gint64 *job_id);
RhErr store_job_update(RhCtx *ctx, gint64 job_id, const char *state, int done,
                       const char *err);
RhErr store_job_get(RhCtx *ctx, gint64 job_id, RhJob **out);
RhErr store_job_list_unfinished(RhCtx *ctx, GPtrArray **out);
RhErr store_job_item_add(RhCtx *ctx, RhJobItem *it);
RhErr store_job_item_update(RhCtx *ctx, gint64 job_id, int seq,
                            const char *state, const char *err);
RhErr store_job_items_list(RhCtx *ctx, gint64 job_id, GPtrArray **out);

/* 检索 */
RhErr store_search_fts(RhCtx *ctx, const char *kw, GPtrArray **out);
```

`RhFilter` 与 1.0.0 的 `cmd_list` 参数一一对应：

```c
typedef struct {
  const char *platform, *genre, *developer, *publisher, *region, *patch_type;
  int   year_from, year_to;      /* 0 = 不限 */
  int   multi;                   /* -1 不限 / 0 / 1 */
  int   has_patch;               /* -1 不限 / 0 / 1 */
  const char *status;            /* good/baddump/unknown/"" */
  const char *format;            /* raw/zip/7z/"" */
  const char **tags; int n_tags;
  const char *kw;                /* 走 FTS5 */
  const char *sort;              /* name/rating/year/discs */
  int   desc;
} RhFilter;
```

---

## 5. 领域层模块职责（对照 1.0.0 的 15 模块）

| 1.0.0 模块 | 函数数 | C 对应 | 变化 |
|---|---|---|---|
| `util.sh` | 28 | `rh-str` / `rh-fs` / `rh-hash` | 哈希三算合一；`sed_inplace`、`print_config` 类 shell 专属函数消失 |
| `deps.sh` | 7 | `rh-config`（能力探测） | 由"命令存在性探测 + 5 种包管理器装包"降级为"可选能力开关"（见 §8.3） |
| `db.sh` | 27 | `store.c` | 整表重写 → 事务；base64/US 编码层整体删除 |
| `config.sh` | 11 | `rh-config.c` | 5 级优先级语义保留；`GKeyFile` 解析 |
| `platform.sh` | 23 | `platform.c` | 规则解析重写为一次性预编译结构体，`ext_platforms` 由 O(n) 字符串拼接变哈希查表 |
| `ui.sh` | 12 | `rh_ui` 接口（§6） | 从函数集合变成可注入的 vtable |
| `classify.sh` | 1 | `classify.c` | 单文件分类的优先级链原样保留（含 `dual_role_kind` 重名扩展名判别） |
| `archive.sh` | 10 | `archive.c` | `unzip`/`7z`/`unrar` 子进程 → libarchive；六级分流逻辑保留 |
| `import.sh` | 21 | `import.c` | 队列从 `$WORK_TMP` 文本文件 → `job_items` 表 |
| `dat.sh` | 7 | `dat.c` | awk 逐个扫描 DAT → 一次性建 `dat_entries` 索引 |
| `library.sh` | 10 | `library.c` | gamelist/Pegasus 解析走 libxml2；整库识别逻辑保留 |
| `export.sh` | 10 | `export.c` | `zip`/`7z` 子进程 → libarchive；三套清单格式保留 |
| `query.sh` | 12 | `query.c` | 13 维筛选 + 4 键排序 → SQL；终端表格打印留在 CLI 层 |
| `detail.sh` | 6 | `detail.c` | 只做**只读视图模型组装**（`RhGameDetail`：元数据 + 8 类资源清单含 `size`/`exists` + 派生判断），不格式化文字、不写库、不删文件；`render_detail.py` 整体删除，文本排版给 CLI、图文面板给 GUI |
| `manage.sh` | 13 | `manage.c` | 菜单逻辑移到前端；业务动作（删碟/补丁/统计/完整性）留在核心 |
| **合计** | **198** | — | 与 1.0.0 实测总数一致（行首定义去重后） |

需要原样继承、不得"顺手优化"的判定规则：

- **分类优先级链**（classify）：压缩包 → 通用补丁（`.ips/.bps/.ups/.ppf/.xdelta/.xdelta3/.patch/.ips32`）→ 平台二义后缀 → 平台专属补丁 → ROM 本体（且当扩展名同时是媒体类型时，用 `dual_role_kind` 判真实内容）→ 媒体/说明。1.0.0 曾有"ROM 判定在媒体分支之前"导致 `.jpg` 被当 videopac ROM、`.md` 被当 megadrive ROM 的真实 bug，新实现必须保持修正后的顺序。
- **压缩包六级分流**：PSV 整库（按 `sce_sys/`、`PCSH*` 路径特征识别，整体入库不解压）→ 纯补丁包 → 街机 ZIP（按 `[arcade_rom_suffixes]` 判定多文件）→ 二义性包 → 单 ROM 包 → 混合包。
- **平台判定**：扩展名唯一命中 → 直接用；多平台命中 → 走 `ui->choose`；目录名反查走 `aliases`（`resolve_platform_dir`）。
- **媒体归属**：孤立媒体按核心名归一后做相似度匹配，阈值取配置 `duplicate_check_threshold`（默认 70）。
- **目录命名**：`sanitize_dirname` 规则沿用，保证与已有库的文件布局兼容。

### 5.1 职责边界（越界即视为架构错误）

| 模块 | 只做 | 不做 |
|---|---|---|
| `platform` | 解析规则、扩展名→平台判定、媒体类型识别 | 不读文件内容、不碰数据库、不搬文件 |
| `classify` | 回答"这个文件归谁"（返回类型 + 目标平台） | **不搬文件、不入库、不弹窗**（1.0.0 的 `enqueue_*` 就混在这里） |
| `archive` | 压缩包列表/解压/加密探测/打包 | 不知道"ROM 是什么"，不判定平台 |
| `dat` | Logiqx 解析、建 `dat_entries`、按哈希查 | 不改游戏记录，只返回匹配结果 |
| `import` | 唯一编排者：队列、归属、命名、m3u、落库 | 不做判定（调 `classify`/`platform`）、不做解压（调 `archive`） |
| `query` | 只读检索、排序、筛选 | **绝不写库** |
| `detail` | 组装只读 `RhGameDetail` | 不格式化文字、不写库、不删文件 |
| `manage` | 编辑/删除/碟片/补丁/统计/完整性 | 不做检索展示 |
| `export` | 遍历输出、模板渲染、媒体复制/压缩 | 不改库 |
| `job` | 作业生命周期、进度、取消、队列持久化 | 不懂业务 |
| `store` | 唯一持有 SQL | 不做业务判断 |

外加一条硬规则：除 `store.c` 外任何源文件出现 SQL 语句即为错误（可用 `grep` 进 CI 检查）。

---

### 6.1 `rh_ui` 接口

对应 1.0.0 的 `ui.sh` 11 个函数，改为 vtable 注入。核心层只认这张表，不认终端也不认 GTK。

```c
typedef struct RhUi {
  void *userdata;

  void (*log)      (void *ud, RhLogLevel lv, const char *msg);

  /* 进度：pct < 0 表示不确定进度（脉冲模式）。工作线程只允许调这一个回调 */
  void (*progress) (void *ud, double pct, const char *label);

  /* 确认：返回 1 = 是，0 = 否 */
  int  (*confirm)  (void *ud, const char *title, const char *msg, int default_yes);

  /* 单选：返回选中索引，-1 = 取消。
     remember_key 非空时，前端可提供"应用到本次所有/全部"选项；
     已记忆的选择由 ctx->batch_memo 查询，核心层不再持有全局状态 */
  int  (*choose)   (void *ud, const char *title, const char **items, int n,
                    const char *remember_key);

  /* 文本输入：返回新分配的字符串（调用方 g_free），NULL = 取消 */
  char *(*enter)   (void *ud, const char *title, const char *label, const char *def);
  char *(*password)(void *ud, const char *title, const char *label);

  /* 文件/目录选择：返回 GPtrArray<gchar*>（调用方 g_ptr_array_unref） */
  GPtrArray *(*pick_files)(void *ud, const char *title, int multiple);
  char      *(*pick_dir)  (void *ud, const char *title);

  /* 只读文本展示（详情、报告） */
  void (*text_view)(void *ud, const char *title, const char *text);

  /* 媒体类型判定：返回 RhMediaKind 枚举，-1 = 取消；
     remember_key 语义同 choose */
  int  (*ask_media_kind)(void *ud, const char *path, const char *remember_key);
} RhUi;
```

**"应用到全部"的正确实现**：`choose` / `ask_media_kind` 的 `remember_key` 让前端把选择记在自己的作用域里（GTK 里是对话框的一个静态字段，CLI 里是一个栈上变量），核心层通过 `ctx->batch_memo` 查询已记忆的选择。这条替代 1.0.0 靠全局变量记忆、在 `$()` 子 shell 里全部失效的四处 bug（`archive_direct_or_extract` / `resolve_ambiguous` / `ask_image_type` / `resolve_platform_for_rom`）。

### 6.2 线程模型

| 线程 | 允许做什么 | 禁止 |
|---|---|---|
| 主线程 | GTK 主循环 / CLI 主流程；创建 `RhCtx`；提交作业 | 长耗时文件操作 |
| 工作线程（`GThreadPool`） | 只调核心 API 与 `ui->progress` | 直接操作任何 GTK 对象、调 `confirm/choose/enter` 等阻塞式回调 |
| SQLite | 每线程一个连接（`store_open_thread()`），WAL 下多连接安全 | 跨线程共用一个 `sqlite3*` |

会阻塞用户的决策（确认、选平台、选图片类型）**不发生在执行阶段**。导入被拆成两阶段：

| 阶段 | 线程 | 交互 | 内容 |
|---|---|---|---|
| ① 预扫描 | 主线程 | 允许（集中提问，一次问完） | 分类到类型/平台、读压缩包索引（不解压）、算重名相似度、判媒体类型、判二义扩展名；产出计划表「将导入 N 游戏 / M 媒体 / K 补丁 / 跳过 J 个（附原因）」；所有需要用户拍板的问题在此一次性处理，答案进 `ctx->batch_memo` |
| ② 执行 | 工作线程 | **零交互** | 纯文件与库操作：复制/改名/m3u/写库。总数已知，进度为确定百分比，可暂停/取消 |

这样做的原因：若允许工作线程中途停下来等弹窗，一个二义扩展名就能卡住整条队列；若采用"遇到决策项就跳过、第二轮再问"，用户要在两轮之间来回切换且第二轮数量不可预期。

兜底：执行阶段仍可能遇到预扫描后环境变化（源文件被移走、目标目录被占用、包内条目与索引不符）。此类情况**不中断整批**，跳过该条目并写入 `job_items.message`；`job_items.state='needs_input'` 仅作为兜底状态保留，作业结束后统一列出待人工处理的条目。

取消：`ctx->cancel_flag`（`atomic_int`），核心层在每个条目边界检查一次；`job_items` 状态确保取消后可从断点继续。

---

## 7. 扩展点

原则：**能声明的用文件，不能声明的才进代码**；先定义扩展点，不做通用插件框架。C 的 ABI 脆弱，过早引入 `.so` 会让内部结构体从此冻结，而当前没有第三方生态。

### 7.1 数据型扩展（无需写代码）

| 扩展点 | 载体 | 说明 |
|---|---|---|
| 平台规则 | `platforms.conf` | 格式原样继承。现格式为"首段后缀表 + `key=value` 键值对"（`nes:.nes;.unf;.fds;compress_supported=true;recommended_format=zip;…`），**新增属性不需要改解析器**（未知键被忽略），因此它已经是数据驱动，无需模板引擎。建议给段头加 `spec_version`，格式变化时明确报错而不是静默读错 |
| 前端规格 | `data/frontends/<id>.conf` | 一份文件同时定义该前端的**导入字段映射**与**导出排版模板**（§7.2）；用户可放 `~/.config/rom-hamster/frontends/` 覆盖，优先级与配置一致 |
| 导出选项 | `export` 配置段 | 目录、是否带媒体、是否压缩、是否生成 m3u |

### 7.2 前端规格文件（导入 + 导出一份）

同一前端的导出字段与导入字段是对称的，所以不为侧各写一套硬编码，而是**每个前端一份规格文件、两面描述**。加新前端 = 加一个 `.conf`，不改代码。

```ini
[frontend]
id           = esde
name         = EmulationStation-DE
spec_version = 1
escape       = xml                  ; 该前端默认转义
layout_rom   = %platform%/%rom_file%      ; ROM 输出相对路径
manifest     = %platform%/gamelist.xml    ; 清单路径
media_dir    = %platform%/media           ; 媒体目录

[export.header]
<?xml version="1.0"?>
<gameList>

[export.game]
  <game>
    <path>./%rom_file:xml%</path>
    <name>%name:xml%</name>
%if:description%    <desc>%description:xml%</desc>
%endif%%if:rating_norm%    <rating>%rating_norm%</rating>
%endif%%if:releasedate_es%    <releasedate>%releasedate_es%</releasedate>
%endif%%if:developer%    <developer>%developer:xml%</developer>
%endif%%if:genre%    <genre>%genre:xml%</genre>
%endif%
  </game>

[export.footer]
</gameList>

[import]
file          = %platform%/gamelist.xml
format        = xml                 ; xml | lines
record        = /gameList/game      ; xml: XPath 1.0 相对表达式
field.name    = ./name
field.description = ./desc
field.cover   = ./image             ; 仅原版 ES 使用；ES-DE 的 gamelist 无此标签
field.marquee = ./marquee
field.video   = ./video
field.rom     = ./path
```

**两处已核对更正**（详见 `docs/rules-verification.md`）：

1. **ES-DE 的 gamelist.xml 不读媒体标签**。它的 `GamelistFileParser.cpp` 里没有 `image`/`marquee`/`video`/`thumbnail`/`fanart`，媒体靠"文件名匹配"从 `~/ES-DE/downloaded_media/<system>/<type>/` 取。所以 ES-DE 模板**不输出** `<image>`，媒体另行落位；而原版 ES（RetroPie）确实读这些标签。两者机制不同，**不能共用一份模板**——这正是"每前端一份 `.conf`"的实际收益。
2. `<releasedate>` 固定为 `YYYYMMDDT000000`、`<rating>` 为 0.0–1.0，故由代码算出 `releasedate_es` 与 `rating_norm` 注入，模板不做算术。

媒体落位由 `[export.media]` 段声明目标路径（代码负责复制、保留源扩展名）：

```ini
[export.media]
covers      = %media_dir%/covers/%game_name%
screenshots = %media_dir%/screenshots/%game_name%
videos      = %media_dir%/videos/%game_name%
```

Pegasus 那份用 `format = lines` + `record = game:` + `field.name = name`（前缀式），字段集相同、语法不同；资产键取自 Pegasus 源码 `Assets.h`，注意是 `assets.titlescreen`（全小写），不是 `titleScreen`。

**模板语法是闭集，只有四类能力**：

| 能力 | 语法 | 说明 |
|---|---|---|
| 变量替换 | `%name%` | 变量名取自下方闭集；**未定义变量在加载期报错**，绝不静默留空（静默空值会让清单字段错位，正是 1.0.0 zenity 表单踩过的坑） |
| 转义修饰 | `%name:xml%` / `:json` / `:text`（默认） | XML 五字符、JSON 引号自动处理，模板作者不必关心转义 |
| 条件 | `%if:cover_rel%…%else%…%endif%` | 仅判真值（非空/非零），不做比较与算术；嵌套 ≤ 3 |
| 循环 | `%for:covers%…%endfor%` | 一个游戏多张图/多补丁；为未来 JSON 前端的数组结构留路 |

可用变量（由代码计算好后注入，模板只排版）：

- 游戏：`id name game_main_name platform platform_export_name description description_line developer publisher year genre players rating rating_norm releasedate_es region tags disc_no is_multi_disc is_arcade rom_status rom_format dat_source patch_type`
- 路径：`rom_file rom_rel rom_size cover_rel screenshot_rel marquee_rel video_rel miximage_rel intro_rel patcher_rel`
- 上下文：`export_dir platform_dir platform_name media_dir game_name platform_extensions emulator_launch`（`emulator_launch` 仅用于前端规格 `launch` 字段；本项目不启动模拟器）

`rating_norm`（10 分制 → 0.0–1.0）、`releasedate_es`（→ `YYYYMMDDT000000`）、`description_line`（压掉换行）都是**代码算好的派生变量**：模板语法里没有算术与字符串函数，这类换算只能在代码里做。

**明确留给代码（模板不做）**：遍历与筛选、相对路径计算、媒体/ROM 的复制与压缩、m3u 生成、多碟隐藏条目规则、路径冲突处理、进度与取消。这些是逻辑，塞进模板就得写表达式引擎。

成本：模板引擎一次性约 600–900 行 C（词法、变量校验、条件/循环、带行号错误报告）。3 个前端时不如硬编码（每个约 150 行）划算，但预期会继续加前端、且用户可自加，从第 4 个起净赚。

### 7.3 代码型扩展（vtable 兜底）

模板表达不了的结构性差异（JSON 嵌套 `game→media[]`、以 SQLite 作为前端库、需联网查询）才走代码后端。

```c
/* 导出后端：仅当模板无法表达时注册 */
typedef struct RhExportBackend {
  const char *id;
  const char *display_name;
  unsigned    api_version;        /* 固定 RH_PLUGIN_API_VERSION */
  int (*prepare)(RhCtx *, const RhExportOpts *, RhErr *);
  int (*emit_game)(RhCtx *, const RhGame *, const char *outdir, RhErr *);
  int (*finish)(RhCtx *, const char *outdir, RhErr *);
} RhExportBackend;

/* 导入处理器：非清单形态的整库（PSV 等）无规格可写，必须代码 */
typedef struct RhImportHandler {
  const char *id;
  unsigned    api_version;
  int  priority;                  /* 小的先试 */
  int  (*detect)(const char *path);                       /* 0 = 命中 */
  int  (*handle)(RhCtx *, const char *path, RhErr *);
} RhImportHandler;
```

注册方式：`plugin.c` 中两张显式数组，链接期确定。将来要动态加载时，在 `plugin.c` 里加 `GModule` 扫描 + 严格比对 `api_version`，调用方不变。

首批注册：

- 导出后端：**空**（`es`/`esde`/`pegasus` 全部由 §7.2 模板驱动，不进 vtable）
- 导入处理器：`psv`（PSV 整库特征识别）、`arcade-zip`（街机 ZIP 多文件判定，若规格表达不顺则走代码）

### 7.4 明确不做为扩展点

- DAT 解析器：只支持 Logiqx 一种格式（1.0.0 也只支持这个）。真需要别的格式再开。
- 单游戏图文 HTML 页：1.0.0 的 `render_detail.py`（13.7KB + python3 依赖）唯一存在理由是 zenity 弹不出图片，只能把 HTML 交给浏览器。GUI 原生详情页已覆盖图文与视频，**不再维护第二套渲染器**。若将来确实要"导出可分享的单游戏页"，按 §7.2 加一份 `detail-html.conf` 规格复用同一套变量，而不是另造渲染器。
- 数据库后端：不做多后端抽象（§13）。
- 缩略图缓存索引：若将来需要百万级缓存键查询，才考虑 LMDB/gdbm 这类 kv 库 —— 不是现在。

### 7.5 刮削扩展点（v0.3 只定契约，不实现）

刮削在本项目里被当作"导入的第三种来源"（前两种是本地压缩包/散文件、前端清单），**不另造一套并行的媒体模型**：抓回来的东西仍然走 `store_media_add_sized` 落 `media` 表、按 `<platform>/<main>/<kind>/` 落盘，因此 GUI、导出、去重全部自动复用。首个版本不实现（§1.2 排除项），**当前也不排期**：本节只固定接口形状，避免以后接进来时改动 schema 或改坏导出/GUI。

**为什么不是现在写**：网络栈会改动 §8 依赖与 deb `Depends`；来源溯源要动 schema（v2）。把契约先写死，M3/M4 的导出与 GUI 就不必为"将来可能有网络媒体"预留分支。

**将来真要做时，方向已定（离线优先）**：先做本地数据包导入（`manifest.tsv` + `media/`，按 crc32→md5→sha1 匹配，用户或社区自备，不随包分发版权素材），再做图片源（Libretro Thumbnails 目录按 ROM 名匹配），最后才考虑在线 API（ScreenScraper 需开发者凭证，境外源在境内网络可达性差异大）。在线源一律默认关闭、可覆盖 endpoint、失败不阻塞导入。

#### 7.5.1 数据型：`data/scrapers/<id>.conf`

与 `data/frontends/<id>.conf` 同构：一份文件描述一个来源，INI + 路径表达式，**不引入 JSON 解析依赖**（当前依赖集没有 json-glib；若将来确需深层嵌套，再单议）。用户放 `~/.config/rom-hamster/scrapers/` 覆盖。

```ini
[scraper]
id          = screenscraper
name        = ScreenScraper.fr
spec_version = 1
api         = https://api.screenscraper.fr/api2/jeuInfos.php
auth        = devid,devpassword,ssid,sspassword   ; 只写键名，值在 secrets.conf
rate_limit  = 1/s             ; 令牌桶，按来源声明
timeout     = 30
cache_ttl   = 30d
max_retry   = 3

[query]
platform = %platform_id%      ; 走 [platform_map] 映射，不硬编码
crc      = %crc32%
md5      = %md5%
sha1     = %sha1%
name     = %game_name%        ; 仅在哈希全无命中时退化使用
language = %scrape_lang%

[platform_map]
nes = 3                       ; 本地平台 id → 来源平台 id

[response]
game        = response.jeu
name        = nom
description = synopsis[%scrape_lang%]
developer   = editeur
publisher   = editeur
year        = dates[release].text
genre       = genres[0].noms[%scrape_lang%]

[media]
covers      = medias[type=box-2D][region=%scrape_region%].url
screenshots = medias[type=screenshot][region=%scrape_region%].url
marquees    = medias[type=wheel][region=%scrape_region%].url
videos      = medias[type=video-normalized][region=%scrape_region%].url
```

路径表达式是**闭集**：`a.b`、`a[0]`、`a[key=value]`、`a[%var%]`，取不到即视为"该来源没有这项"，**不报错、不写空值**。字段名取不到的来源由配置作者负责，加载期校验 `[scraper]` 必填项。

#### 7.5.2 代码型：`RhScraper` vtable

规则文件只能描述"怎么问、怎么取"，**分页、限速、鉴权失败重试、媒体类型推断**仍是代码。结构体进 §7.3 同一套 vtable 范式，`plugin.c` 显式数组注册：

```c
typedef struct RhScrapeMatch {
  gchar *source_id;      /* scraper id */
  gchar *source_url;     /* 命中详情页/API 查询 URL，写 media.source_url */
  gchar *name;           /* 来源给出的规范名 */
  gchar *description;
  gchar *developer;
  gchar *publisher;
  gint   year;
  gchar *genre;
  gchar *media_url[RH_MEDIA_KIND_COUNT];   /* 按 kind 索引，可空 */
} RhScrapeMatch;

typedef struct RhScraper {
  const char *id;
  unsigned    api_version;                       /* RH_PLUGIN_API_VERSION */
  gboolean    requires_network;
  int (*probe)(RhCtx *, RhErr *);                /* 凭证/可达性检查，不联网也可返回可用 */
  int (*match)(RhCtx *, const RhGame *, const RhScrapeOpts *, RhScrapeMatch **, RhErr *);
  int (*fetch)(RhCtx *, const RhScrapeMatch *, const char *kind,
               const char *dest_path, RhErr *);  /* 下载到临时文件，由调用方 copy_atomic 落位 */
} RhScraper;
```

`fetch` 只负责拿到临时文件，**落位与入库仍由 `import.c` 的同一套 `rh_fs_copy_atomic` + 单条事务完成**，保证与本地导入同样的幂等语义（0/2/3/1）。

#### 7.5.3 配置与凭证

新增配置键（builtin 默认，均为可选）：

| 键 | 默认 | 说明 |
|---|---|---|
| `scrape_enabled` | `false` | 总开关；关闭时任何刮削命令直接返回 0 并提示未启用 |
| `scrape_providers` | `""` | 按顺序尝试的 id 列表，空 = 不启用 |
| `scrape_lang` | `""` | 空 = 跟随 `lang` |
| `scrape_region` | `""` | 空 = 来源默认区域 |
| `scrape_overwrite` | `false` | `false` = 只补缺，不覆盖已有媒体 |
| `scrape_timeout` | `30` | 秒 |
| `scrape_rate_per_min` | `30` | 全局令牌桶上限，来源声明更低时取小 |
| `scrape_cache_dir` | `""` | 空 = `$XDG_CACHE_HOME/rom-hamster/scrape` |

凭证**不进五层配置**：单独 `~/.config/rom-hamster/secrets.conf`，权限必须 `0600`，加载时校验，过宽则拒绝读取并报错；`--verbose`/日志/错误信息一律脱敏（只打印 key 名，不打印值）。

#### 7.5.4 查询与写回策略（契约）

1. 查询键优先 **crc32 → md5 → sha1**（与 `dat_match_hash` 解耦，刮削总是三选一按来源支持能力发），全无命中才用 `platform + name`，且名称先按 DAT 规范名归一（`game_main_name`），再退原始目录名。
2. 同一游戏多来源命中时按 `scrape_providers` 顺序取第一个，**不合并字段**（避免来源混搭出无法追溯的记录）。
3. 媒体写回：`scrape_overwrite=false` 时同 `(game_id,kind)` 已有文件即跳过并计数 `skipped`；`true` 时先写临时文件再原子替换。
4. 失败语义：单游戏失败只记 `failed` 并继续下一个，**绝不中断整批**；离线/超时按 `max_retry` 退避后放弃。
5. 缓存：命中响应按 `game_id + source_id` 缓存 `cache_ttl`，缓存命中不发请求；`--refresh` 强制绕过。

#### 7.5.5 来源溯源（schema v2）

`media` 表新增三列（v1 → v2 迁移，默认值兼容旧库）：

| 列 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `source` | TEXT | `'local'` | `local` / `library` / `<scraper id>` |
| `source_url` | TEXT | `NULL` | 抓取时的查询或详情 URL |
| `fetched_at` | INTEGER | `NULL` | Unix 秒；本地导入为 NULL |

这样 GUI 能显示"这张封面来自 ScreenScraper"，导出时也能按来源筛选；不引入侧表，避免每次读媒体都要 join。

#### 7.5.6 依赖影响

- **新增 `libcurl`**（`libcurl4`，最低 7.74）。它只被核心库里的 `scraper*.c` 使用，但动态链接后仍是 `rom-hamster` 包的硬依赖；若坚持"纯离线安装不装 curl"，构建期加 `-Dscraper=false` 并在 deb 里拆出 `rom-hamster-scraper`（二选一，将来实现时拍板）。
- 不引入 json-glib / libsoup；HTTP 只走 libcurl，JSON 用来源自带的最小解析（或后续单议）。
- 离线可用性不变：`scrape_enabled=false`（默认）时进程启动与导入路径**不触碰任何网络代码**。

---

## 8. 依赖清单

### 8.1 构建期

| 依赖 | 最低版本 | 用途 |
|---|---|---|
| C 编译器（gcc / clang） | gcc 9 | C11 + `_GNU_SOURCE` |
| `meson` | 1.0 | 构建 |
| `ninja-build` | — | 后端 |
| `pkg-config` | — | 依赖探测 |
| `gettext` | — | `msgfmt` 生成 `.mo` |
| `glib-2.0`（`-dev`） | 2.66 | 容器、字符串、`GChecksum`、`GTask`/`GThreadPool`、`GKeyFile`、`GTest` |

### 8.2 运行期（核心库）

| 库 | Debian 包 | 最低版本 | 用途 | 替代掉的 1.0.0 外部命令 |
|---|---|---|---|---|
| GLib | `libglib2.0-0` | 2.66 | 容器/字符串/线程/配置/`GChecksum`(md5,sha1)/GIO 打开 URI | `grep` `sed` `awk`(部分) `md5sum` `sha1sum` `xdg-open` |
| SQLite | `libsqlite3-0` | 3.35（FTS5 trigram 需 3.34+） | 元数据库 | `awk` 逐行解析 CSV、`base64` |
| libarchive | `libarchive13` | 3.4 | zip/7z/rar/tar 读写、密码、加密探测 | `unzip` `zip` `7z` `unrar` |
| zlib | `zlib1g` | — | CRC32、deflate | `gzip` 尾部取 CRC 的降级路径 |
| libxml2 | `libxml2` | 2.9 | Logiqx DAT、`gamelist.xml` 流式解析 | `awk` 手写 XML 扫描 |

### 8.3 可选

| 库/程序 | 用途 | 缺失时行为 |
|---|---|---|
| `gtk4`（`libgtk-4-1`，**≥ 4.10**） | GUI 的唯一图形依赖。基线定 4.10 后可直接使用 `GtkFileDialog`（异步文件选择），不再写 `GTK_CHECK_VERSION` 兼容分支 | 仅 CLI 可构建；`meson -Dgui=false` |
| `libadwaita-1-0` | 更贴近 GNOME 的控件 | 退回原生 GTK4 |
| GTK4 GStreamer 媒体后端 | 详情页内嵌播放视频（`GtkVideo`/`GtkMediaFile`）。Debian 系为独立包，**包名需打包时按发行版核对**（常见 `libgtk-4-media-gstreamer`） | 退化为 `g_app_info_launch_default_for_uri` 交外部播放器打开，并提示原因 |
| `gstreamer1.0-plugins-base` / `-good` / `-libav` | 视频编解码覆盖（mp4/webm/mkv/avi） | 部分格式播不了，其他功能不受影响；gui 包用 `Recommends` 而非 `Depends` |
| `webp-pixbuf-loader` | `.webp` 缩略图（`media_suffix_rules` 明确允许 webp） | 该格式无缩略图，其余正常 |
| `libchdr` | CHD 只读（后续若要识别/校验 CHD） | 仅记录格式字符串 |
| `chdman`（外部） | CHD 转换 | 不做转换（与 1.0.0 一致） |
| `unrar`（外部） | RAR5 加密包（libarchive 对部分 RAR5 加密不支持） | 报告该包无法读取，不中断整批导入 |
| `libcurl4`（**将来实现刮削时**） | 刮削的唯一网络栈（§7.5） | M0–M5 不链接；`-Dscraper=false` 时不进依赖 |

对照 1.0.0 `deps.sh`：原本 6 个必装命令（`bash`≥4 / `find` / `coreutils` / `grep` / `sed` / `awk`）+ 7 个建议组件（`zip` `unzip` `7z` `zenity` `python3` `unrar` `xdg-open`），以及 `_dep_pkg` 里按 apt/dnf/pacman/brew/opkg 做的包名映射与 `sudo` 自动装包逻辑 —— 全部消失。运行时依赖变成 5 个 `.so`，由 dpkg 声明。

> **以下三条为归档**：§8.3.1–§8.3.3 是定基线之前的调研记录。按 §1.3 的决定，目标平台已收窄为现代桌面发行版，NAS / 容器 / Entware 均不再实施，数据仅作追溯，不投入工程资源。

### 8.3.1 国产发行版（统信 UOS）的实测数据

| 版本 | 白皮书里的一手数据 | 对本项目的含义 |
|---|---|---|
| **统信桌面操作系统 V25** | 《V25 产品白皮书》表 4「重要技术指标」：`gtk 3.24.41、4.16.7`、glibc 2.38、gcc 12.3、Qt 6.8.0/5.15、内核 6.6、systemd 255.2 | GTK **4.10** 基线成立，且 `GtkFileDialog` 可直接使用；`sqlite3` / `libarchive` / `libxml2` 均远高于下限 |
| **统信桌面操作系统 V20** | 《V20 白皮书》表 3-7「指标参数」：内核 4.19/5.10、DDE 5.4（自研）、Xorg 1.20、**glibc 2.28**、gcc 8.3、**未列 GTK 条目** | glibc 2.28 = Debian 10 世代，GTK4 发布于 2020 年底，**该平台基本不存在 GTK4**，GUI 装不上；`glib` 约 2.58、`sqlite3` 约 3.27，均低于本文件 §8.2 的下限 |

结论：**GUI 无法覆盖 V20，CLI 有希望覆盖**。这正是"两二进制 + 共享核心"设计的实际收益 —— 同一份核心库，V25 上装 `rom-hamster-gui` 全功能，V20 上只装 `rom-hamster` 走纯命令行。若要把 CLI 也覆盖到 V20，需把 `glib` 下限降到 2.56、`sqlite3` 降到 3.27，代价是 SQL 中不能用 `RETURNING`（3.35+），且 FTS5 `trigram` 不可用（需 `unicode61` 或 `LIKE` 降级）。

任何发行版的实际版本不要靠文档推断，在真机上一行定论：

```
pkg-config --modversion gtk4; pkg-config --modversion glib-2.0; sqlite3 --version; apt-cache policy libgtk-4-1
```

### 8.3.2 主流 NAS 系统

| 系统 | glibc | glib | GTK | 依据 |
|---|---|---|---|---|
| Unraid 7.x | **2.40** | glib2 ≈2.80（原文被截断） | 无 | 官方发布说明「Base distro」包清单直接列出 `glibc: version 2.40` |
| TrueNAS SCALE 24.04+ | 2.36（Debian 12） | 2.74 | 无 | TrueNAS 功能文档标注 `Linux Kernel 6.6, Debian 12` |
| OpenMediaVault | 2.36（Debian 12） | 2.74 | 无 | 基于 Debian 12 |
| 群晖 DSM 7.x | 未公布（工具链属 glibc 2.26 一代，**待真机实测**） | 待实测 | 无 | DSM 7 第三方套件开发指南只规定工具链与套件格式，不列库版本 |
| 威联通 QTS 5.x | **2.21** | 待实测 | 无 | QNAP 社区 2026-11 的功能请求原文：「currently 2.21」 |

三条结论：

1. **NAS 上不存在 GTK**。DSM 与 QTS 的界面都是 Web 面板，`rom-hamster-gui` 不进 NAS，这不是取舍而是平台事实。
2. 真正的门槛是 **glibc**：QTS 5.x 至今仍是 2.21（2015 年的版本），任何在现代系统上编译的二进制都会因符号版本不足直接起不来；DSM 7 需要真机确认。
3. 因此 NAS 的正确交付形态**不是 deb，而是 OCI 容器镜像**（群晖 Container Manager、威联通 Container Station 都原生支持），把 `ROM_ROOT` 作为卷挂载后跑 CLI。备选是自带 `.so` + `rpath` 的 tarball。

真机验证：

```
ldd --version | head -1; pkg-config --modversion glib-2.0; ls /usr/lib/libgtk-4.so.1 2>/dev/null || echo "no gtk4"
```

### 8.3.3 Entware 路径的可行性（NAS 仅 CLI）

Entware 的 `x64-k3.2` 包索引里，本项目需要的库全部存在：

| 库 | Entware 包 | 索引中的实际版本 | 本项目下限 |
|---|---|---|---|
| glib | `glib2` | 2.82.0-1 | 2.56 |
| sqlite3 | `libsqlite3` / `sqlite3-cli` | 3.51.2-1 | 3.27 |
| libarchive | `libarchive` | 3.8.1-2 | 3.4 |
| libxml2 | `libxml2` | 2.15.1-1 | 2.9 |
| zlib | `zlib` | 1.3.1-1 | — |
| RAR 兜底 | `unrar` / `libunrar` | 7.0.9-1 | — |

但 **Entware 不自带 libc**：其 wiki 明确"repo 内二进制的依赖库全部来自 `/opt` 内部"，libc 由固件提供；feed 以 **glibc 2.27** 为构建基线（包索引中 `glibc-gconv 2.27-12` 可佐证）。因此固件 glibc 低于 2.27 时，上述现代版本包会因符号版本不足而无法加载（`version 'GLIBC_2.27' not found`），且 `opkg` 安装阶段不会报错。QTS 5.x 的 2.21 正落在此区间，DSM 7 需真机实测。

三条路径的取舍：

| 路径 | 前提 | 代价 |
|---|---|---|
| **OCI 容器**（推荐主交付） | 无 | 无；容器自带 glibc 与全部库，QTS 2.21 亦可运行 |
| Entware + 自建 `.ipk` | 固件 glibc ≥ 2.27；Entware 不收录第三方包，需自行交叉编译 x86_64/aarch64/armv7sf/mipselsf/mipssf 五架构并维护依赖元数据 | 用户需先装 Entware、外接盘承载 `/opt`（权限 ≥600）、再装五个库 |
| 低 glibc 构建的裸二进制 | 在 CentOS 7 / Debian 10 容器内构建 | 只能用老版本 glib/sqlite，与 Entware 新包**互斥**，不可混用 |

结论：NAS 与老 glibc 环境以容器镜像为主交付，Entware 列为可选路径并写死前提，不作为默认支持承诺。

**与 1.0.0 bash 版的对照**：bash 版不链接任何 `.so`，只调外部命令，因此**完全不受 glibc / GTK 约束**；`deps.sh` 的 `detect_pkg_mgr()` 第一行就判 `opkg`（且要求 `/opt/etc` 存在），说明它的设计本就把 Entware 当第一等环境。Entware `x64-k3.2` 索引中的对应包：

| 1.0.0 依赖 | Entware 包 | 索引中的版本 |
|---|---|---|
| `bash`（要求 ≥4） | `bash` | 5.3-2 |
| `find` | `findutils` | 4.10.0-1 |
| `coreutils` | `coreutils` | 9.9-2 |
| `grep` | `grep` | 3.12-1 |
| `sed` | `sed` | 4.9-1 |
| `awk` | `gawk` | 5.3.2-1 |
| `zip` / `unzip` | Info-ZIP `zip` / `unzip` | `unzip` 6.0-9 |
| `7z` | `p7zip` / `7-zip` | 16.02-3 / 25.01-1 |
| `python3`（CRC32 兜底、HTML） | `python3` | 3.13.9-2 |
| `unrar` | `unrar` | 7.0.9-1 |
| `zenity` / `xdg-open` | 不需要 | NAS 无图形环境，`--gui` 本就不适用 |

**对上一条的自我修正**：不能仅凭「构建基线 glibc 2.27 vs 固件 2.21」就断定 Entware 必然失败 —— QNAP 官方 FAQ 本身推荐用 Entware QPKG 装 iperf3，说明它在 QTS 5.x 上实际可运行。判定标准应改为**先跑 `/opt/bin/bash -c 'echo ok'`**：能跑，则同一工具链产出的 `glib2` / `libarchive` 也能跑（它们出自同一 buildroot）。因此 Entware 路径对 bash 版无门槛，对 C 版 CLI 亦大概率可行，但仍需真机确认。

### 8.4 测试/CI 依赖

`valgrind`、`libglib2.0-dev`（含 GTest）、`ninja`。CI 需至少三组构建：`-Dgui=true`、`-Dgui=false`（验证核心不沾 GTK）、`-Db_sanitize=address,undefined`。

---

## 9. 打包与安装布局

### 9.1 三包拆分

| 包 | 内容 | 依赖 |
|---|---|---|
| `rom-hamster` | `/usr/bin/rom-hamster`（CLI）、`/usr/lib/x86_64-linux-gnu/rom-hamster/libromham-1.0.so`、`/etc/rom-hamster/rom-hamster.conf`、`/etc/rom-hamster/platforms.conf`、man page | `libglib2.0-0`, `libsqlite3-0`, `libarchive13`, `zlib1g`, `libxml2` |
| `rom-hamster-gui` | `/usr/bin/rom-hamster-gui`、`.desktop`、图标、AppStream metainfo | `rom-hamster`（= 同一版本核心）, `libgtk-4-1 (>= 4.10)`；Recommends: GTK4 GStreamer 媒体后端、`gstreamer1.0-plugins-base/-good/-libav`、`webp-pixbuf-loader` |
| `rom-hamster-data` | `/usr/share/rom-hamster/platforms.conf`（只读出厂默认）、`/usr/share/rom-hamster/frontends/*.conf`（三份前端规格）、`rom-hamster.conf.template`、`LICENSE`、`changelog` | — |

### 9.2 对 1.0.0 的一处打包修正

1.0.0 把 `platforms.conf` 放在 `/usr/share/rom-hamster/` 并登记为 **conffile**。Debian Policy 要求 conffile 必须位于 `/etc`，`/usr/share` 下的文件不得作为 conffile（否则 `dpkg` 升级行为不保证，`dpkg -P` 也不清理）。新方案：

- `/usr/share/rom-hamster/platforms.conf` —— 出厂默认，属 `rom-hamster-data`，**任何进程不得写入**；
- `/etc/rom-hamster/platforms.conf` —— 系统管理员覆盖层，conffile；
- 配置优先级（低 → 高）：内置默认（编译进二进制的最小集）→ `/usr/share/rom-hamster/platforms.conf` → `/etc/rom-hamster/platforms.conf` → `/etc/rom-hamster/rom-hamster.conf` → `~/.config/rom-hamster/platforms.conf` → `~/.config/rom-hamster/rom-hamster.conf` → 命令行参数（详见 §10.1–§10.3）。

用户级配置路径为 XDG 规范的 `~/.config/rom-hamster/rom-hamster.conf`。**不读取、也不迁移** 1.0.0 的 `~/.rom-hamster.conf`（见 §10.3）。

### 9.3 命名与命令集

- CLI 为 `rom-hamster`，子命令 `init / import / list / search / show / edit / delete / export / stats / config / help / version`。
- GUI 为 `rom-hamster-gui`，桌面文件 `Exec=rom-hamster-gui`。
- **不与 bash 版做行为兼容**：不提供 `--gui` 转跳、不读取 `~/.rom-hamster.conf`、不保留菜单序号语义、**不提供 CSV 导入**（§12.1）。命令行为不做兼容层。
- **图标直接复用 1.0.0 的资产**：`rom-hamster.svg` + `16/32/48/256` 五套 png，不重画。

---

## 10. 配置系统

沿用 `GKeyFile` 的 `[default]` 段，**键名与 1.0.0 的 `rom-hamster.conf` 保持同名**，以便旧配置直接可用：

| 键 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `ROM_ROOT` | path | 空（首次运行引导） | ROM 收藏根目录 |
| `dat_dir` | path | 空（默认 `<ROM_ROOT>/dats`） | DAT 存放目录 |
| `archive_extensions` | 分号列表 | `.zip;.7z;.rar` | 识别的压缩包后缀 |
| `archive_compress_export_format` | 枚举 | `zip` | 导出压缩格式（`zip`/`7z`） |
| `duplicate_check_threshold` | 0–100 | `70` | 同平台重名相似度阈值，0 关闭 |
| `dat_match_hash` | 枚举 | `crc32` | DAT 匹配哈希（`crc32`/`md5`/`sha1`） |
| `dat_auto_match_multi_disc` | bool | `true` | 匹配到单碟后自动关联其他碟 |
| `auto_generate_m3u` | bool | `true` | 多碟自动生成 m3u |
| `export_hidden_discs` | bool | `true` | 导出时为多碟生成隐藏条目 |
| `disc_label_prefix` | string | `Disc` | 碟片命名前缀 |
| `auto_delete_source` | bool | `false` | 导入成功后删除源文件 |

新增键（本次引入）：`store_path`（默认 `<ROM_ROOT>/romhamster.db`）、`emulator_launch`（string；仅作为前端规格 `launch` 字段的模板变量，本程序不启动模拟器）、`lang`（默认随 `LANG`）。

DAT 查找规则保持 1.0.0 语义：`<dat_dir>/<platform>.dat`（含 `.DAT` 大小写）精确优先，未命中则按 `aliases` 正则回退扫描 `<dat_dir>` 两层的 `*.dat`。

### 10.1 分层与来源记录

五层（低 → 高）：内置默认 → `/usr/share/rom-hamster/` → `/etc/rom-hamster/rom-hamster.conf` → `~/.config/rom-hamster/rom-hamster.conf` → 命令行。每个键在 `RhConfig` 中存 `{ value, layer, source_file, line }`，后者覆盖前者但保留来源。

`rom-hamster config --explain <KEY>` 输出：

```
ROM_ROOT = /home/zhoux/roms
  来自 ~/.config/rom-hamster/rom-hamster.conf:3
  覆盖了 /etc/rom-hamster/rom-hamster.conf:5 的 /srv/roms
```

1.0.0 的 `config` 只打印最终值，用户排查"设置为什么没生效"只能逐个文件翻；来源记录让这个问题一句话就能回答。

### 10.2 平台规则的四层

| 层 | 位置 | 谁改 | 性质 |
|---|---|---|---|
| 1 | 编译进二进制 | 无人 | 最小集，保证任何配置文件缺失都能启动 |
| 2 | `/usr/share/rom-hamster/platforms.conf` | 无人 | `data` 包，只读出厂规则 |
| 3 | `/etc/rom-hamster/platforms.conf` | 系统管理员 | **conffile**（Debian Policy 要求 conffile 位于 `/etc`） |
| 4 | `~/.config/rom-hamster/platforms.conf` | 用户 | 自行增补平台，无需 root |

### 10.3 与 1.0.0 配置的关系：不迁移（已定）

**既不迁移、也不提示。** 新版只读 `~/.config/rom-hamster/`；`~/.rom-hamster.conf` 存在于否都不影响运行，程序不会去读、也不会去拷、更不会提示用户。

理由：

- 键名同名（§10 表格）意味着用户若想沿用旧设置，把文件内容复制到新路径即可，不需要程序代劳。
- 迁移逻辑一旦写进代码，就得长期维护一条只为旧版存在的分支（判断旧文件存在、判断新文件不存在、处理权限失败、处理半拷贝）；这对一个全新重写的程序是不必要的负担。
- 与 §9.3「不复用 `~/.rom-hamster.conf`」一致：运行期不存在旧路径这个概念。

数据层面同样不迁移：`romhamsterdb.csv` 不被读取（§12.1）。

---

## 11. 正式桌面应用的必做项

| 项 | 要求 |
|---|---|
| 应用生命周期 | `GtkApplication`，`APPLICATION_ID=io.github.goddog312.RomHamster`，`G_APPLICATION_HANDLES_OPEN`：第二次启动不新开进程，改为激活已有窗口并接收参数 |
| 界面构建 | `.ui` + `GResource` 编译进二进制；不手写控件树；明暗主题跟随系统 |
| 详情页（媒体同窗） | 元数据 `GtkGrid` + 8 类资源分页（`GtkStackSwitcher`：ROM文件/封面/截图/视频/横幅/合成图/说明/补丁），与游戏目录的 8 个子目录一一对应。图片经 `GdkPixbuf` 异步加载、按控件尺寸缩放、磁盘缩略图缓存（键 = 路径+mtime+尺寸，目录 `$XDG_CACHE_HOME/rom-hamster/thumbs/`）；视频用 `GtkVideo` 内嵌播放，无媒体后端时降级外部打开；说明文本用只读 `GtkTextView` 显示原文（**不解析 Markdown**）。由此 `render_detail.py` 与 `python3` 依赖整体消失 |
| 图标与元数据 | 多尺寸图标（沿用 1.0.0 的 `rom-hamster.svg` 与 png 套件）、`.desktop`（`Exec=rom-hamster-gui`）、AppStream metainfo（含截图与发布说明）、man page |
| 国际化 | gettext；**msgid 用英文**（`xgettext` 直接提取字面量），提供 `zh_CN` 中文 `.po` 与英文原文；AppStream 描述必须英文。代价：1.0.0 的中文提示需先写成英文，属一次性迁移工作量 |
| 首次运行 | CLI：`ROM_ROOT` 未配置时报错并提示 `rom-hamster init --root <dir>`。GUI：启动即弹 `GtkFileDialog` 选目录，选完直接建库入库并进入主窗口，不额外插入欢迎页 |
| 拖拽导入 | `GtkDropTarget` 接收文件与目录列表，落入后**走同一条两阶段导入管线**（不新增代码路径），支持多选与目录递归。进 M4 |
| 参数解析 | `GOptionContext`，`--help` 自动生成；子命令独立 option group |
| 日志 | `g_log_structured`，域 `romham`、`romham/import`、`romham/store`；`--log-file` 可选输出 |
| 崩溃/中断恢复 | 导入队列存 `jobs/job_items`；启动时 `store_job_list_unfinished()` 提示续做；杀掉进程不会丢已完成条目 |
| 写入一致性 | **每个条目一个事务，且把 `job_items` 状态更新与记录写入放进同一事务**：`BEGIN` → 写 `games/roms/media` + 标 `job_items.state='done'` → `COMMIT`。文件侧顺序：复制到临时名 → `rename` 到最终名（先于事务提交）。崩溃发生在提交前：库里无记录、可能留下孤儿文件（由完整性检查发现）；重试靠**幂等复制**（目标已存在且 `size`+`mtime` 一致则跳过，不覆盖）。禁止"先写库后搬文件" |
| 原子写 | 配置/清单落盘一律 tmp + `rename`（沿用 `atomic_write`） |
| UI 不阻塞 | 所有 >50ms 的操作进作业线程；进度条支持暂停/取消；不确定进度用 `pct=-1` 的脉冲模式 |
| 测试 | GLib GTest 单测 + `tests/fixtures` 集成样本 + asan/valgrind job |
| 版本与升级 | `--version` 输出维护者、仓库、bug 渠道（沿用 1.0.0 文案）；升级时自动跑 `store_migrate()` |

---

## 12. 从 bash 版继承什么（已定：不迁移、不继承测试）

### 12.1 不迁移任何旧有文件

**明确不做**：

- 不提供 `import-csv`，不读取 `romhamsterdb.csv`。
- 不读取、不迁移 `~/.rom-hamster.conf`（见 §10.3）。
- 不从旧库继承任何记录、`id`、目录映射。

新库从空开始建。**接受的代价**：1.0.0 用户库里的 19 个标量元数据（年份、类型、开发商、发行商、地区、评分、简介、标签、状态等）全部丢弃，需重新导入源 ROM 文件重算；磁盘上的 `<platform>/<game>/` 目录与媒体文件仍在（目录布局与 `sanitize_dirname` 规则原样保留，见 §13），但程序不识别它们为"已有记录"。

### 12.2 不继承 bash 版的测试

76 个单元测试与 `smoke.sh` 的 14 项菜单喂入用例**一律不迁移**。新项目的测试从零编写：GTest 单测 + `tests/fixtures` 集成样本，验收标准由 §14 各里程碑自行定义。

理由：旧测试断言的是 shell 函数的行为（`$WORK_TMP` 目录、base64/US 分隔的内存表、zenity 的文本输出），这些结构在新实现里根本不存在，强行平移只会得到一批测不到真实行为的断言。

### 12.3 但 1.0.0 的已知缺陷不得在新实现里重现

这一节不是迁移清单，而是**代码审查项**：以下问题在 1.0.0 里被确证过，新实现若重现即视为缺陷。

| 缺陷 | 正确做法 | 落点 |
|---|---|---|
| `db_upsert_F` 改一个字段重写全库 | 单条 `UPDATE` | §3.5 事务决策 |
| `db_load_cache` 每行 fork 一个 awk | 跨行状态机，零子进程 | §3.1 |
| `file_crc32` 用 `gzip -1 -c \| tail -c 8` 全量压缩取尾 | zlib `crc32()`，三哈希一次读盘 | §8.2 |
| 4 处"应用到全部"在 `$()` 子 shell 内失效 | `ctx->batch_memo`，作用域限当前作业 | §6.1 |
| `year`/`rating` 存字符串导致排序是字典序 | 类型化到 `INTEGER`/`REAL` | §3.3 |
| 多图只导出第一张 | `media` 表一对多 | §3.3 |
| Pegasus 末尾记录丢失、描述不跨行、缺 `assets.screenshot` 映射 | 记录以 EOF 收尾、描述续行、补映射 | §7.2 |
| 分类时先判扩展名再判内容（`.jpg` 与 `.md` 双重身份） | 先判真实内容，再判 ROM | §5 |
| 复合扩展名 `.p8.png` 丢失 `.p8` 语义 | 保留多级扩展名 | §5 |
| `recommended_format` 配置存在但全工程无实现 | 明确不做，不假装支持 | §1.2 |
| 元数据表单空字段错位 | C 结构体天然不存在，但测试须覆盖"中间字段留空" | §14 M3 |

---

## 13. 风险与取舍

| 风险 | 影响 | 对策 |
|---|---|---|
| SQLite 放在 SMB/NFS 上 | 锁不可靠，可能损坏库 | 库文件强制本地；启动时检测 `st_dev` 与 fs 类型，网络盘直接拒绝并说明；将来多机共享才考虑 PostgreSQL |
| C 内存安全 | 崩溃、越界 | asan/valgrind 进 CI；所有权约定写入头文件注释（谁分配、谁释放、是否转移） |
| libarchive 对 RAR5 加密支持有限 | 少数包读不了 | 报"该包无法读取"并继续整批，不中断；可选外挂 `unrar` |
| 大整合包（数百 GB） | 内存/耗时 | 全程流式：libarchive 边读边写，哈希单次读盘，`job_items` 逐条提交 |
| 多实例同时写 | 数据竞争 | `GtkApplication` 保证 GUI 单实例；跨进程**不加锁文件**，靠 SQLite 自身的串行化写（WAL 下同一时刻只允许一个写者）+ `busy_timeout=5000`。超时报 `SQLITE_BUSY`，提示"另一实例正在写入，请稍后重试"。不自制锁文件，避免死锁残留 |
| 重写工作量 | 198 函数 → 约 8–12k 行 C | 里程碑拆分（§14），每阶段有可验证产物，不做"全部写完再联调" |
| 不迁移旧库 | 1.0.0 用户等于重建，19 个描述性字段丢失 | 目录布局与 `sanitize_dirname` 规则保留、媒体文件仍在；README 与升级说明必须写明"升级 = 从零建库"（§12.1），避免用户以为数据丢了是 bug |

明确取舍（写下来避免反复讨论）：

- **不做多数据库后端抽象**。所有 SQL 收敛在 `store.c`，将来换 PostgreSQL 只改一个文件。
- **不做动态插件加载**。先编译期注册，等有第三方需求再开 `GModule`。
- **不做格式转换**。`chd/cso/wbfs` 只作为格式字段取值。
- **不把 CLI 当二等公民**。CLI 是正式前端，拥有全部功能；GUI 只是另一个 `rh_ui` 实现。

---

## 14. 里程碑

三条规则：

1. **M1 起每个里程碑都打 git tag 并产出可安装 deb**（`vX.Y.Z-Mn`）。打包问题（soname、`/usr/lib` 布局、conffile 登记）越晚暴露越贵，不能等到 M5 才发现安装布局要返工。
2. **M3 → M4 串行**。GUI 依赖 M3 定型的 `query`/`detail` 接口；并行会让 GUI 反复追接口变更。
3. 验收项必须**可执行**。形容词不算通过。

| 阶段 | 交付物 | 验收 |
|---|---|---|
| **M0 骨架** | `meson.build`、`RhCtx`/`RhErr`、`rh-config` 五层加载、`store_open`+v1 schema+迁移、`rom-hamster --version`、`rom-hamster-gui` 空窗口 | 三组构建通过（`-Dgui=true` / `-Dgui=false` / asan+ubsan）；`ldd libromham.so` 中不含 `gtk`/`gdk`/`pango`；两个二进制都能启动 |
| **M1 存储与规则** | `store_*` 全量、`RhGame`/`RhFilter`、`platform.c` 解析 101 平台、`debian/` 打包骨架 + **首个可安装 deb** | 101 个平台、5 段规则全部解析成功且媒体类型识别有单测；`store_*` 的 CRUD、`dir_path` UNIQUE、级联删除有测试；`dpkg -i` 后 `rom-hamster --version` 可运行；`lintian` 无 error |
| **M2 导入** | `classify`、`archive`（libarchive）、`job` 作业线程、`dat` 索引、`library` 整库、`import` 编排 | fixtures 的 5 类压缩包（zip/7z/rar/单 ROM/混合）+ PSV 特征包 + 二义扩展名 + 多碟 CD 全部落到正确目录；`kill -9` 后重启能续做且库里无半个条目；该阶段 deb 仍可安装 |
| **M3 检索/管理/导出** | `query`、`detail`（CLI 纯文本）、`manage`、`export` 模板引擎 + 三份前端规格 | 13 维筛选 + 4 排序键结果与直接 SQL 对齐；导出的 ES-DE / Pegasus 清单能被对应前端正常读取；golden file 逐字节 diff 通过；故意写错的模板在**加载期**报出文件名与行号 |
| **M4 GUI** | `ui-gtk`、主窗口 `GtkColumnView`、详情页 8 类媒体分页、导入进度/取消、首次运行引导、拖拽导入 | 见下方 §14.1 量化指标；空配置首启能走完"选目录 → 建库 → 主窗口"；拖入目录触发导入并有可见进度；无媒体后端时视频降级为外部打开并提示 |

| **M5 打包与发布** | AppStream metainfo（含真实截图）、完整 gettext、CI 矩阵、lintian 全绿、仓库上传 | `dpkg -i` 后 CLI 与 GUI 均可运行；`dpkg -P` 干净卸载无残留；升级时 conffile 保留管理员修改；`appstreamcli validate` 通过；至少 Ubuntu 24.04 与 Debian 13 两个发行版安装验证通过 |

### 14.1 M4 性能验收（量化）

测试数据：脚本生成 **10000 条**游戏记录（每 10 条含 1 个多碟、每 20 条含 3 张截图）。

参考机型（绝对数字均在此环境测量，README 记录实测配置）：4 核 x86_64 / 集显 / SATA SSD / 16 GB / 1920×1080 / GTK 4.16。

| 指标 | 阈值 | 测法 |
|---|---|---|
| 冷启动到主窗口可见 | < 1.5 s | `main()` 首行打点 → 窗口 `map` 事件，`g_get_monotonic_time()` 求差 |
| 首屏列表渲染 | < 300 ms | 窗口 `map` → 首帧绘制完成（`GdkFrameClock` 的 `after-paint`） |
| 连续滚动 10 s | p95 帧间隔 ≤ 33 ms，且无单帧 > 100 ms | 记录每帧时间戳，统计 p95 与最大值 |
| 关键字筛选响应 | < 200 ms | 输入停顿到列表模型更新完成 |
| 打开详情页 | 元数据 < 150 ms；首张缩略图 < 400 ms 且不阻塞 UI | 详情页构造打点；缩略图异步加载 |
| 内存增长 | 滚动 10 s 后 RSS 增量 < 50 MB | `/proc/self/statm` 前后对比 |
| 导入中 UI | 进度刷新间隔 ≤ 200 ms；取消到作业线程退出 < 1 s | 作业回调打点 |

CI 能自动化的只有「首屏渲染」与「内存增长」两项（xvfb 下运行）。帧率与启动时间在无显示器环境测不准，**不写进 CI 假装有指标**：改由人工在参考机型复测并记录，与上次基线比较，劣化超过 20% 即阻断发布。

---

## 附录 A：1.0.0 现状速查（供对照）

- 元数据库：`ROM_ROOT/romhamsterdb.csv`，28 字段，RFC4180；内存整表 `DB_ALL[id]=base64(US分隔)`；`db_flush` 原子重写全表
- 游戏目录：`<平台>/<目录>/{rom,covers,screenshots,videos,marquees,miximages,intro,patcher}`
- 平台规则：`platforms.conf` 101 平台 / 5 段（`platform_extensions`、`arcade_rom_suffixes`、`media_type_dirs`、`media_suffix_rules`、`archive_extensions`）
- 命令：`menu/init/import/list/search/show/edit/delete/export/stats/config/help/version`，`--gui` 走 zenity
- 筛选：13 个维度（platform/genre/developer/publisher/year/region/tag/multi/status/has-patch/patch-type/format/kw）+ 4 个排序键（name/rating/year/discs）+ `--dir asc|desc`
- 导出前端：`es` / `esde` / `pegasus`
- 统计 11 项：总数、平台数、类型数、ROM 数、多碟数、坏 dump 数、补丁数、压缩数、占用空间、存储位置、配置与 DAT 目录
- 完整性：三向核对（DB→文件缺失、文件→DB 未登记、多余文件）
