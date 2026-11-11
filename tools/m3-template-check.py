#!/usr/bin/env python3
"""M3 模板加载期报错验收：故意写错的模板必须报 "文件:行"，且不得落盘任何文件。"""
import os
import subprocess
import sys
import tempfile

BIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "rom-hamster")
BASE = """[frontend]
id = bad
name = Bad
layout_rom = %rom_file%
manifest = %platform%/gamelist.xml

[export.header]
<?xml version="1.0" encoding="UTF-8"?>
<gameList>

[export.game]
{body}
  </game>

[export.footer]
</gameList>
"""

cases = [
    ("undefined variable", "%nosuch%"),
    ("unknown escape", "%name:html%"),
    ("unclosed if", "%if:name%yes"),
    ("unknown loop", "%for:bogus%x%endfor%"),
    ("empty directive", "%%"),
    ("unclosed loop", "%for:covers%x"),
]

passed = failed = 0
for label, body in cases:
    with tempfile.TemporaryDirectory() as td:
        conf = os.path.join(td, "bad.conf")
        text = BASE.format(body=body)
        with open(conf, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        # 期望行号 = body 在文件中的实际行号
        want_line = text[:text.index(body)].count("\n") + 1
        dest = os.path.join(td, "out")
        r = subprocess.run([BIN, "export", dest, "--frontend", conf],
                           capture_output=True, text=True)
        msg = r.stderr + r.stdout
        ok = (r.returncode != 0 and f"{conf}:{want_line}" in msg
              and not os.path.exists(dest))
        if ok:
            passed += 1
            print(f"PASS  {label}: {conf}:{want_line}")
        else:
            failed += 1
            print(f"FAIL  {label}: rc={r.returncode} want={conf}:{want_line}\n  {msg.strip()}")

# 合法模板必须加载成功（用 es.conf 走一次真实导出）
root = os.path.expanduser("~/.cache/romham-m3/check")
env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(root, "config"))
dest = os.path.join(root, "out", "sanity")
r = subprocess.run([BIN, "export", dest, "--frontend",
                    os.path.join(os.path.dirname(BIN), "..", "data", "frontends", "es.conf"),
                    "--id", "nes-c9a7d94a2f3f"], env=env, capture_output=True, text=True)
if r.returncode == 0 and os.path.isdir(dest):
    passed += 1
    print("PASS  valid template loads")
else:
    failed += 1
    print(f"FAIL  valid template loads rc={r.returncode}\n  {r.stderr.strip()}")

print(f"\n{passed} passed, {failed} failed")
sys.exit(0 if failed == 0 else 1)
