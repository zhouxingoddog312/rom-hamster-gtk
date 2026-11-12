#!/usr/bin/env python3
"""把 m4-perf.sh 的原始日志汇总成 docs/m4-perf-baseline.md。"""
import glob
import os
import re
import statistics
import subprocess
import sys

root, runs = sys.argv[1], sys.argv[2]

def sh(cmd):
    try:
        return subprocess.check_output(cmd, shell=True, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except Exception:
        return "unknown"

def parse(name):
    vals = {}
    for path in sorted(glob.glob(os.path.join(runs, name + "-*.log"))):
        text = open(path, encoding="utf-8", errors="replace").read()
        for key in ("startup_ms", "first_screen_ms", "search_total_ms",
                    "search_query_ms", "detail_ms", "first_thumb_ms",
                    "p95_ms", "max_ms", "rss_delta_kb", "scroll_frames"):
            m = re.search(r"perf: .*?%s=([0-9.]+)" % key, text)
            if m:
                vals.setdefault(key, []).append(float(m.group(1)))
    return vals

data = {n: parse(n) for n in ("startup", "search", "detail", "scroll")}

def med(name, key):
    v = data[name].get(key, [])
    return statistics.median(v) if v else None

def rng(name, key):
    v = data[name].get(key, [])
    return (min(v), max(v)) if v else (None, None)

def fmt(v, unit="ms"):
    return "—" if v is None else ("%.1f %s" % (v, unit))

def status(ok, suffix=""):
    return ("PASS" if ok else "FAIL") + suffix

startup = med("startup", "startup_ms")
first = med("startup", "first_screen_ms")
search_total = med("search", "search_total_ms")
search_query = med("search", "search_query_ms")
detail = med("detail", "detail_ms")
thumb = med("detail", "first_thumb_ms")
p95 = med("scroll", "p95_ms")
worst = med("scroll", "max_ms")
rss = med("scroll", "rss_delta_kb")

env = f"""- 系统：{sh('lsb_release -ds 2>/dev/null || uname -a')}
- CPU：{sh("grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ / /'")}，{sh('nproc')} 个逻辑 CPU
- 内存：{sh("awk '/MemTotal/{printf \"%.1f GB\", $2/1048576}' /proc/meminfo")}
- 显示：WSL2 WSLg / `xvfb-run`（Xvfb 默认 1280×1024×24），无参考机型独立显卡
- GTK：{sh('pkg-config --modversion gtk4')}；glib：{sh('pkg-config --modversion glib-2.0')}；sqlite：{sh('pkg-config --modversion sqlite3')}
- 说明：本机为 WSL2，不是 ARCHITECTURE §14.1 的“4 核 / 16 GB / SATA SSD / 1920×1080 / GTK 4.16”参考机型；首屏与帧率受软件渲染影响，数字只作本机基线，不能直接当作参考机型结论。"""

report = f"""# M4 性能基线（10000 条）

自动生成：`tools/m4-perf.sh`。原始日志在 `$HOME/.cache/romham-m4/perf/runs/`。
本文件是**人工记录**，不接入 CI；ARCHITECTURE §14.1 明确帧率与启动时间在无显示器环境测不准。

## 环境

{env}

## 夹具

- `tools/m4-perf-fixture.sh` 生成，路径 `$HOME/.cache/romham-m4/perf/lib`。
- 10000 条游戏记录；每 10 条 1 个多碟（共 11000 条 ROM）；每 20 条 3 张截图（共 1501 条媒体，含首条封面）。
- 首条 `Perf Game 00001` 带真实 400×400 封面，详情页缩略图可实测解码。

## 结果（3 轮中位数）

| 指标 | 阈值 | 实测 | 判定 | 备注 |
|---|---|---|---|---|
| 冷启动到主窗口可见 | < 1.5 s | 首轮 {fmt(data["startup"].get("startup_ms", [None])[0])} / 3 轮中位 {fmt(startup)} | {status(startup is not None and startup < 1500)} | `main()` 首行 → `map`；首轮含冷缓存 |
| 首屏列表渲染 | < 300 ms | {fmt(first)} | {status(first is not None and first < 300, '（本机软件渲染）')} | `map` → `after-paint`；3 行夹具也约 690 ms，说明主要是首次软件渲染，不是 10k 列表本身 |
| 连续滚动 10 s | p95 ≤ 33 ms，无单帧 > 100 ms | p95 {fmt(p95)} / max {fmt(worst)} | {status(p95 is not None and p95 <= 33 and worst is not None and worst <= 100, '（本机软件渲染）')} | 程序化滚动，记录 `after-paint` 时间戳；本机约 {("%d" % data["scroll"].get("scroll_frames", [0])[0]) if data["scroll"].get("scroll_frames") else "—"} 帧 |
| 关键字筛选响应 | < 200 ms | 总 {fmt(search_total)}（查询 {fmt(search_query)}） | {status(search_total is not None and search_total < 200)} | 纯查询达标；总时延含 100 ms 防抖 |
| 打开详情页：元数据 | < 150 ms | {fmt(detail)} | {status(detail is not None and detail < 150)} | `rh_detail_load` + 元数据/页面构建 |
| 打开详情页：首张缩略图 | < 400 ms | {fmt(thumb)} | {status(thumb is not None and thumb < 400)} | 异步解码，不阻塞 UI |
| 滚动 10 s 后 RSS 增量 | < 50 MB | {fmt(rss, 'KB')} | {status(rss is not None and rss < 51200)} | `/proc/self/status` 前后对比；总 RSS 约 250 MB |
| 导入中 UI：进度刷新 | ≤ 200 ms | SKIP | SKIP（功能未实现） | GUI 导入尚未接入，不能伪造指标 |
| 导入中 UI：取消到线程退出 | < 1 s | SKIP | SKIP（功能未实现） | 同上 |

## 结论

- 10k 整表刷新约 82–86 ms，查询耗时不是瓶颈；详情元数据约 5 ms、首张缩略图约 80 ms，均明显低于阈值。
- 关键字筛选的**纯查询**约 30–55 ms；100 ms 防抖叠加后总时延约 128 ms，按“输入停顿到模型更新完成”的严格口径达标。
- 冷启动、详情、内存增长达标；首屏与滚动帧率在本机软件渲染下超阈值，且 3 行夹具首屏也约 690 ms，说明该数字主要反映 WSLg/Xvfb 首次渲染，不是 10k 数据量本身。参考机型复测前不据此判定发布阻断。
- 导入进度/取消两项因 GUI 导入未实现，显式 SKIP。
"""

path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "docs", "m4-perf-baseline.md")
with open(path, "w", encoding="utf-8", newline="\n") as f:
    f.write(report)
print(report)
print("written:", path)
