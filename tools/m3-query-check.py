#!/usr/bin/env python3
"""M3 查询验收：用独立 SQL 对照 CLI list 的 14 个筛选维与 4 个排序键。

每个用例都在同一份库上跑两条路径：
  1) ./build/rom-hamster list ...  -> id 序列
  2) sqlite3 直接拼 WHERE/ORDER BY -> id 序列
两者必须逐项相等（顺序也相等），否则 FAIL。
"""
import os
import sqlite3
import subprocess
import sys

ROOT = os.path.expanduser("~/.cache/romham-m3/check")
DB = os.path.join(ROOT, "lib", "romhamster.db")
BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "rom-hamster")
ENV = dict(os.environ, XDG_CONFIG_HOME=os.path.join(ROOT, "config"))

CASES = [
    ("filter platform",   ["--platform", "nes"],       "games.platform = ?", ["nes"]),
    ("filter genre",      ["--genre", "Action"],       "games.genre = ?", ["Action"]),
    ("filter developer",  ["--developer", "DevA"],     "games.developer = ?", ["DevA"]),
    ("filter publisher",  ["--publisher", "PubY"],     "games.publisher = ?", ["PubY"]),
    ("filter region",     ["--region", "JP"],          "games.region = ?", ["JP"]),
    ("filter patch_type", ["--patch-type", "generic"], "games.patch_type = ?", ["generic"]),
    ("filter year_from",  ["--year-from", "1990"],     "games.year >= ?", [1990]),
    ("filter year_to",    ["--year-to", "1990"],       "games.year <= ?", [1990]),
    ("filter multi",      ["--multi"],                 "games.is_multi_disc = 1", []),
    ("filter single",     ["--single"],                "games.is_multi_disc = 0", []),
    ("filter has_patch",  ["--has-patch"],             "games.patch_type <> ''", []),
    ("filter no_patch",   ["--no-patch"],              "games.patch_type = ''", []),
    ("filter status",     ["--status", "good"],        "games.rom_status = ?", ["good"]),
    ("filter format",     ["--format", "zip"],         "games.rom_format = ?", ["zip"]),
    ("filter tag",        ["--tag", "classic"],
     "games.id IN (SELECT gt.game_id FROM game_tags gt JOIN tags t ON t.id = gt.tag_id WHERE t.name = ?)",
     ["classic"]),
    ("filter kw",         ["--kw", "Alpha"],
     "(games.name LIKE ? OR games.game_main_name LIKE ? OR games.description LIKE ?)",
     ["%Alpha%", "%Alpha%", "%Alpha%"]),
]

SORTS = [
    ("sort name asc",    "name",   False, "games.name", "ASC"),
    ("sort name desc",   "name",   True,  "games.name", "DESC"),
    ("sort rating asc",  "rating", False, "games.rating", "ASC"),
    ("sort rating desc", "rating", True,  "games.rating", "DESC"),
    ("sort year asc",    "year",   False, "games.year", "ASC"),
    ("sort year desc",   "year",   True,  "games.year", "DESC"),
    ("sort discs asc",   "discs",  False,
     "(SELECT count(*) FROM roms r WHERE r.game_id = games.id)", "ASC"),
    ("sort discs desc",  "discs",  True,
     "(SELECT count(*) FROM roms r WHERE r.game_id = games.id)", "DESC"),
]

passed = failed = 0


def cli_ids(args):
    out = subprocess.run([BIN, "list"] + args, env=ENV, check=True,
                         capture_output=True, text=True).stdout
    lines = out.splitlines()
    return [ln.split("\t")[0] for ln in lines[1:-1]]


def check(label, got, want):
    global passed, failed
    if got == want:
        passed += 1
        print(f"PASS  {label}  ({len(got)} 行)")
    else:
        failed += 1
        print(f"FAIL  {label}\n  cli={got}\n  sql={want}")


def main():
    global passed, failed
    if not os.path.exists(DB):
        print("fixture missing; run tools/m3-fixture.sh first")
        return 2

    for label, args, where, params in CASES:
        got = cli_ids(args)
        sql = (f"SELECT games.id FROM games WHERE {where} "
               f"ORDER BY games.name ASC, games.id ASC")
        with sqlite3.connect(DB) as db:
            want = [r[0] for r in db.execute(sql, params)]
        check(label, got, want)

    for label, key, desc, col, order in SORTS:
        args = ["--sort", key] + (["--desc"] if desc else [])
        got = cli_ids(args)
        sql = f"SELECT games.id FROM games ORDER BY {col} {order}, games.id ASC"
        with sqlite3.connect(DB) as db:
            want = [r[0] for r in db.execute(sql)]
        check(label, got, want)

    r = subprocess.run([BIN, "list", "--sort", "bogus"], env=ENV,
                       capture_output=True, text=True)
    if r.returncode != 0 and "unknown sort key" in (r.stderr + r.stdout):
        passed += 1
        print("PASS  reject unknown sort key")
    else:
        failed += 1
        print(f"FAIL  reject unknown sort key rc={r.returncode} err={r.stderr!r}")

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
