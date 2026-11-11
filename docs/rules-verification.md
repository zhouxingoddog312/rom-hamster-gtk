# 规则核对记录（2026-11-11）

本文记录 `data/platforms.conf` 与 `data/frontends/*.conf` 里每一条结论的来源，
以及**刻意没有采纳**的内容。目的是让后来的修改者知道哪些值是查过的、
哪些是判断过的、哪些仍是存疑的。

## 1. 一手来源

| 用途 | 来源 | 取用方式 |
|---|---|---|
| 平台 ID 与扩展名 | ES-DE 官方仓库 `resources/systems/unix/es_systems.xml`（195 个系统，每个系统的 `<extension>` 清单） | 直接下载 XML，用脚本解析成 `系统名 → 扩展名集合` 再与本项目清单逐项比对 |
| gamelist.xml 字段 | ES-DE `es-app/src/MetaData.cpp` 的 `gameDecls[]` 声明表 | 字段名、类型、默认值逐条抄录 |
| gamelist.xml 解析行为 | ES-DE `es-app/src/GamelistFileParser.cpp` | 通读；确认其中**不存在** `image`/`marquee`/`video`/`thumbnail`/`fanart` 字样 |
| ES-DE 媒体放置规则 | ES-DE `USERGUIDE.md` | 搜 `downloaded_media`，得到 `~/ES-DE/downloaded_media/<system>/<media type>/<Game Name>.<ext>` |
| Pegasus 元数据语法 | Pegasus 官方仓库 `tests/backend/providers/pegasus/data/*`（真实样例） | 抄 `collection:`/`game:`/`file:`/`assets.boxFront:`/多行 `files:` 的实际写法 |
| Pegasus 资产键 | Pegasus `src/backend/model/gaming/Assets.h` 的 `GEN()` 清单 | 逐条抄；注意 `titlescreen` 全小写 |
| Pegasus 字段解析 | Pegasus `src/backend/providers/pegasus_metadata/PegasusMetadata.cpp` | 抄字段名与 `assets.` 前缀匹配规则 |

## 2. platforms.conf 的修正（相对 1.0.0）

| 平台 | 改动 | 依据 |
|---|---|---|
| `pcengine` | 删 `.tg16` | TG16 是主机名（TurboGrafx-16），不是文件扩展名；ES-DE 该平台无此扩展 |
| `videopac` | 删 `.jpg` | 图片不是 ROM。1.0.0 把 `.jpg` 放进 videopac，正是"截图被当成 ROM 导入"那个缺陷的源头 |
| `gamegear` / `mastersystem` | 删 `.zip` | 归档格式统一由 `[archive_extensions]` 识别；写进平台扩展会让压缩包分流与平台判定互相污染 |
| `atari800` | 去重 `.atr`；补 `.car .cas .cdm .com .atx .rom .bin .xfd` | ES-DE 的 atari800 合并了 400/800/XL 家族，这些扩展均为真实映像格式 |
| `bbcmicro` | 去重 `.ssd`；补 `.img` | 同上 |
| `megaduck` | `.md` → `.bin` | `.md` 是 Mega Drive 的扩展，会让 Mega Duck 与 Mega Drive 冲突；Mega Duck 实际是 `.bin` |
| `c16` | `aliases` 去掉 `plus4` | Plus/4 是独立机型，已新增 `plus4` 平台行 |
| `gameandwatch` | 补 `.mgw` | ES-DE 认定格式；`.mg/.mgc` 保留为历史变体 |
| `nes` / `snes` / `gbc` / `gba` 等 | 补 `.unif .swc .bs .bml .st .bsx .gd3 .gd7 .mgd .dx2 .cgb .dmg .agb` 等 | 均为该平台真实存在的 dump 变体 |
| 各光盘平台（psx/ps2/saturn/dreamcast/segacd/pcfx/pcenginecd/ngcd/3do/cdi/amigacd32） | 补 `.chd .mds .toc .ciso .cso .nrg .zso .isz` 等 | 这些格式在模拟器社区被普遍使用 |
| `ps3` / `psvita` / `switch` / `wiiu` | 补 `.ps3 .ps3dir .psvita .nro .nca .wua .wuhb .tmd` | ES-DE 认定格式 |
| `doom` / `zmachine` / `scv` / `vectrex` / `atarilynx` / `atarist` / `amiga` / `c64` 系 / `zxspectrum` / `pc88` / `pc98` / `x68000` / `x1` / `msx` 系 | 补该平台真实变体 | 逐项对照 ES-DE |

修正后平台数为 **101**（新增 `plus4`）。

## 3. 刻意不采纳的 ES-DE 内容

ES-DE 的扩展名清单是**为容错而做的超集**，它自己负责启动模拟器，多认一个扩展
只是多一次尝试。本项目的判定链需要区分度，照搬会直接破坏功能：

| 不采纳 | 原因 |
|---|---|
| 把 `.zip .7z` 写进每个平台 | 归档识别是独立阶段（压缩包六级分流）。写进平台扩展后，一个 `.zip` 既像 ROM 又像压缩包，二义判定会被淹没 |
| `.m3u` 当作平台扩展名 | m3u 是本项目**生成**的多碟播放列表，不是输入格式 |
| 跨平台容错扩展（如给 `gb` 列 `.sfc/.smc`、给 `gamegear` 列 `.md/.gen`） | 会让几乎每个 `.bin/.rom` 命中十几个平台，导入时弹窗淹没用户；平台判定应宁严勿宽，认不出时按目录名 `aliases` 反查 |
| `.desktop` / `.sh`（ES-DE 用于原生 Linux 游戏） | 不是 ROM；本项目不做"启动任意可执行文件" |

代价：ES-DE 能"凑合认"的错名 ROM，本项目可能不认，需要用户改名或依赖目录名反查。
这是有意选择的严格性。

## 4. 前端规格的核对结论

| 结论 | 影响 |
|---|---|
| **ES-DE 不读 gamelist.xml 里的媒体标签** | 原设计（ARCHITECTURE §7.2 早期版本）在 ES-DE 模板里写 `<image>` 是无效的。已改为只输出元数据，媒体另按 `downloaded_media/<platform>/<type>/<游戏名>.<ext>` 落位 |
| **原版 ES 读媒体标签** | ES 与 ES-DE 的媒体机制不同，**不能共用一份模板**，必须两份 `frontends/*.conf` |
| `<releasedate>` 为 `YYYYMMDDT000000` | 需要代码先算好 `releasedate_es`；1.0.0 的"1999"字符串不能直出 |
| `<rating>` 与 Pegasus `rating` 均为 0.0–1.0 | 本库内部 10 分制，需 `rating_norm` |
| `<path>` 在 ES-DE 必须带 `./` | 两种模板统一写 `./`，ES-DE 与 ES 都能读 |
| Pegasus 资产键是 `assets.titlescreen`（全小写） | 旧文档写的 `assets.titleScreen` 是错的，已改 |
| Pegasus 记录以 `game:` 起始、空行分隔、**文件结尾即记录结尾** | 1.0.0 解析器要求空行收尾，导致最后一条静默丢失；本版按 EOF 收尾 |
| Pegasus 多行值靠缩进续行 | 本版把描述压成单行（`description_line`）输出，避免缩进推断错误；多行留待后续 |

## 5. 仍然存疑 / 待验证

1. **ES-DE 旧版本行为**：以上结论基于 master 分支源码（ES-DE 3.x）。若用户使用
   1.x/2.x，媒体标签是否被读取未验证。导出时以 3.x 为准。
2. **Pegasus 多行描述**：当前单行输出会丢失换行，属已知降级（记入 M3 验收关注项）。
3. **`psvita`**：ES-DE 用 `.psvita` 标记文件而非 `.vpk`；本清单两者都收，导入时
   若遇到 `.vpk` 需确认是否被前端识别。
4. **`.bin` 的二义**：`gamegear`/`mastersystem`/`sg-1000`/`sega32x` 等共享 `.bin`，
   已写入 `ambiguous_formats=.bin`，导入时会走确认流程。这是无法从文件名消解的
   固有二义，只能靠目录名或用户选择。
5. **`arcade` 平台的 `.7z`**：本版给 fbneo/mame/neogeo/cps* 加了 `.7z`，但 1.0.0
   只认 `.zip`。是否接受 `.7z` 直存需要在实际导入测试里确认（MAME 支持 7z 读取）。
