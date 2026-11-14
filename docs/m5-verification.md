# M5 验收记录：打包与发布

日期：2026-11-14　基线：`5e24195`（`8e8ea8a` 之后只改了配置解析与其单测）

对照 `ARCHITECTURE.md` §14 的 M5 交付物与验收口径逐项记录。凡是未直接执行、
或用替代方式验证的项都在第 5 节明确标注，不把“应该可以”写成“已通过”。

## 1. 范围

| 交付物 | 落地位置 |
|---|---|
| AppStream metainfo（含真实截图） | `data/io.github.goddog312.romhamster.metainfo.xml`、`data/screenshots/rom-hamster-main.png` |
| desktop entry 与图标 | `data/io.github.goddog312.romhamster.desktop`、`data/rom-hamster.svg` |
| 完整 gettext | `po/POTFILES.in` / `po/LINGUAS` / `po/zh_CN.po`，meson `i18n.gettext()` |
| CI 矩阵 | `.github/workflows/ci.yml` + `tools/ci-matrix.sh` |
| 三个 deb | `rom-hamster` / `rom-hamster-gui` / `rom-hamster-data` |
| 仓库上传 | Gitee 主仓库 `goddog312/rom-hamster-gtk`，GitHub 镜像 `zhouxingoddog312/rom-hamster-gtk` |

## 2. §14 M5 验收项

| # | 验收项 | 结果 | 验证方式 | 证据 |
|---|---|---|---|---|
| 1 | `dpkg -i` 后 CLI 与 GUI 均可运行 | PASS | CI 打包 job 装三个 deb 后跑 `rom-hamster --version`；本地解包 payload 后 xvfb 启动 GUI | `rom-hamster 0.1.0`；GUI `smoke: detail pages=8`，rc=0 |
| 2 | `dpkg -P` 干净卸载无残留 | PASS | CI 打包 job：`dpkg -P rom-hamster-gui rom-hamster rom-hamster-data` 后 `dpkg -s rom-hamster` 必须失败 | run `43443788150` 的 `Purge packages` 步骤成功 |
| 3 | 升级时 conffile 保留管理员修改 | **未覆盖** | 见 §5.1：只确认了 conffile 已登记，没有做双版本升级实测 | `dpkg-deb -e` 的 `conffiles` 含 `/etc/rom-hamster/platforms.conf`、`/etc/rom-hamster/rom-hamster.conf` |
| 4 | `appstreamcli validate` 通过 | PASS | `appstreamcli validate --no-net data/io.github.goddog312.romhamster.metainfo.xml` | `✔ Validation was successful.` |
| 5 | 至少 Ubuntu 24.04 与 Debian 13 安装验证通过 | PASS | CI 两个发行版各跑一遍打包 → lintian → 安装 → 验证 → 卸载 | run `43443788150`：8 个 job 全成功 |
| 6 | 完整 gettext | PASS | `msgfmt --check` 逐个校验、CI 的 i18n 步骤、安装后检查 `.mo` | `po/zh_CN.po` 226 条 msgid（含头部），`msgfmt --check` rc=0；装包后 `/usr/share/locale/zh_CN/LC_MESSAGES/rom-hamster.mo` 存在 |
| 7 | CI 矩阵 | PASS | GitHub Actions，`{gui, nogui, asan} × {ubuntu:24.04, debian:13}` | 6 个构建 leg + 2 个打包 job，见 §3 |
| 8 | lintian 全绿 | PASS | 三个 deb 一起跑 `lintian` | rc=0，`E:`/`W:` 计数为 0（完整日志在 `$HOME/.cache/romham-m4/package-check/lintian.log`） |
| 9 | 仓库上传 | PASS | 本地、Gitee、GitHub 的 `master` 与 tag 指向一致 | 见 §4.3 |

## 3. CI 矩阵

### 3.1 结构

`.github/workflows/ci.yml`，触发条件 `push`（master）与 `pull_request`，
`permissions: contents: read`，容器内执行（不依赖 runner 自带的发行版）。

| job | 维度 | 内容 |
|---|---|---|
| `matrix` | `distro ∈ {ubuntu:24.04, debian:13}` × `config ∈ {gui, nogui, asan}` | 装构建依赖 → `bash tools/ci-matrix.sh <config>`：meson setup/compile、`meson test --print-errorlogs`、i18n（`msgfmt --check` + pot 生成）、`ldd libromham` 不得含 gtk/gdk/pango、GUI leg 的 xvfb 冒烟 + fixture、SQL 只允许出现在 `src/core/store.c` |
| `package` | `distro ∈ {ubuntu:24.04, debian:13}` | 装打包依赖 → `tools/m4-close-deb.sh` 打三个 deb → `lintian` → `dpkg -i` data/CLI/GUI → `rom-hamster --version` + zh_CN catalog 检查 → `dpkg -P` 并确认已注销 |

`fail-fast: false`，单个 leg 失败不影响其他 leg 的结论。

### 3.2 首次真实运行的失败与修复

| 轮次 | commit | 结果 | 失败点与原因 | 修复 |
|---|---|---|---|---|
| run 1 | `619ac16` | 7/8 | `ubuntu:24.04 / package` 的安装步骤 | 拆成安装/验证/卸载三步，失败时用 `::error::` 带出 dpkg 输出 |
| run 2 | `d43c310` | 7/8 | `Verify installed binary and catalog` | 注解显示 catalog 缺失；补诊断输出 |
| run 3 | `c123dfb` | 7/8 | 同上 | 注解确认deb 内有 `.mo`，但镜像 dpkg 配置 `path-exclude=/usr/share/locale/*/LC_MESSAGES/*.mo` 在解包时丢弃翻译 |
| run 4 | `8e8ea8a` | **8/8** | — | 打包 job 只删除该条 locale 排除规则后安装验证 |

### 3.3 run 4 结果（`43443788150`，commit `8e8ea8a`）

- `ubuntu:24.04 / gui`、`ubuntu:24.04 / nogui`、`ubuntu:24.04 / asan`：success
- `debian:13 / gui`、`debian:13 / nogui`、`debian:13 / asan`：success
- `ubuntu:24.04 / package`：success（Build packages → Lint → Install → Verify → Purge 全过）
- `debian:13 / package`：success

`package` 的验证段实际检查：`rom-hamster --version` 可执行、zh_CN catalog
落在 `/usr/share/locale/zh_CN/LC_MESSAGES/rom-hamster.mo`、卸载后
`dpkg -s rom-hamster` 返回失败。

## 4. 本地验证

### 4.1 包内 GUI 冒烟（`tools/m4-package-verify.sh`）

用本轮新打的 deb 解包后直接运行 payload 里的二进制（不改断言、不跳过）：

```
PASS  no emulators.conf in deb payload
PASS  CLI binary present
PASS  GUI binary present
PASS  man page present
PASS  packaged CLI --version rc=0
PASS  packaged GUI smoke rc=0
smoke: detail pages=8 title=Alpha · nes · 1990 thumbs=1/1 cache_hits=0 videos=2 ...
PASS  packaged GUI opens detail
lintian rc=0
M4 PACKAGE: ALL PASS
```

`dpkg -i` / `dpkg -P` 段在本机 SKIP（非 root，且没有免交互 sudo），
安装/卸载由 §3 的 CI 打包 job 覆盖。

### 4.2 单测与构建

- `meson test -C build`：5 个套件（config-layers / store-schema / platform-rules /
  str-fs-hash / store-crud）全过。
- 本轮新增 `/config/quoted-values`，覆盖 `ROM_ROOT=""`、单引号、带空格的值、
  不成对引号四种情况。
- `-Dgui=true`、`-Dgui=false`、`-Db_sanitize=address,undefined` 三组构建在 CI
  两个发行版上各跑一遍。

### 4.3 仓库与 tag

- Gitee：`git@gitee.com:goddog312/rom-hamster-gtk.git`（`origin`，主仓库）
- GitHub：`git@github.com:zhouxingoddog312/rom-hamster-gtk.git`（`github`，镜像）
- 本阶段每次提交都同时推两处，`master` 与 `v0.1.0-M1`～`v0.1.0-M4` 的 tag
  （含 annotated tag 的 peeled ref）逐字节一致，然后打 `v0.1.0-M5` 并同样双推。

## 5. 未覆盖或降级的项

### 5.1 conffile 升级保留（未执行）

`/etc/rom-hamster/rom-hamster.conf` 与 `/etc/rom-hamster/platforms.conf` 已由
dpkg 登记为 conffile（`dpkg-deb -e` 可查），语义上由 dpkg 负责保留管理员修改。
但本阶段**没有**做“装 v1 → 改 conffile → 装 v2 → 检查修改仍在”的双版本实测：
仓库目前只有一个版本号 `0.1.0`，构造第二个版本属于测试工程，未纳入本轮范围。
在 M5 验收口径里这一项按**未覆盖**记，不按通过记。

### 5.2 desktop entry 的 Categories 提示

`desktop-file-validate` 返回 0，但给出一条 hint：
`Categories` 同时含 `Utility` 与 `Game`，应用菜单里可能重复出现。
这是 hint 不是 error，未改；如要消除，把 `Categories` 收成单一主类即可。

### 5.3 其余沿用 M4 记录的边界

- 真实 `GtkFileDialog` 的人机点选仍未自动化，冒烟用 `RH_SMOKE_ROOT` 走同一条
  `open_library()` 路径；文件对话框本身只做代码审查。
- GStreamer 媒体后端、`webp-pixbuf-loader` 等是 `Recommends`，安装验证不强制
  安装，缺后端时按 §8.3 降级（外部播放器），不视为失败。
- CI 只在 GitHub 镜像上跑：Gitee Go 云端编译的基础镜像为 CentOS 8.3 / Ubuntu 20.04
  （glib 2.64、sqlite 3.31），低于项目基线（glib ≥ 2.66、sqlite ≥ 3.35、
  GTK ≥ 4.10），且 GitHub Actions 不能直接跑在 Gitee 仓库上。

## 6. 本阶段修掉的两个真实缺陷

| # | 缺陷 | 影响 | 修法 |
|---|---|---|---|
| 1 | 配置解析不剥引号：1.0.0 的 `ROM_ROOT=""` 被读成两个字面引号 | 非空但非法的 `ROM_ROOT` 让 GUI 跳过首次运行引导，`open_library()` 失败，窗口 0 行；同理会破坏 `archive_extensions`、`dat_match_hash` 等带引号的值 | 加载时剥掉一对匹配的单/双引号，不成对则按字面保留；新增 `/config/quoted-values` 单测 |
| 2 | `ubuntu:24.04` 镜像的 dpkg 配置丢弃 `/usr/share/locale/*/LC_MESSAGES/*.mo` | 安装验证看不到 zh_CN catalog，CI 打包 job 必失败 | 打包 job 安装前仅移除这一条 locale 排除规则，文档/手册裁剪保持原样 |
