#!/usr/bin/env python3
"""M3 导出验收：三套清单 golden 逐字节对比 + 结构/媒体/m3u/转义检查。"""
import os
import subprocess
import sys
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.expanduser("~/.cache/romham-m3/check")
BIN = os.path.join(HERE, "..", "build", "rom-hamster")
ENV = dict(os.environ, XDG_CONFIG_HOME=os.path.join(ROOT, "config"))
OUT = os.path.join(ROOT, "out")
A = "nes-c9a7d94a2f3f"
B = "nes-c0ba3f479def"
G = "nes-f5fc753a8b2f"

passed = failed = 0


def check(label, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"PASS  {label}")
    else:
        failed += 1
        print(f"FAIL  {label}  {detail}")


def run_export(dest, frontend, ids):
    r = subprocess.run([BIN, "export", dest, "--frontend", frontend]
                       + sum([["--id", i] for i in ids], []) + ["--media"],
                       env=ENV, capture_output=True, text=True)
    return r


def main():
    os.chdir(os.path.join(HERE, ".."))
    os.makedirs(OUT, exist_ok=True)

    cases = [
        ("es", "data/frontends/es.conf", os.path.join(OUT, "es"),
         "nes/gamelist.xml", "tests/fixtures/m3/es.gamelist.xml"),
        ("esde", "data/frontends/es-de.conf", os.path.join(OUT, "esde"),
         "gamelists/nes/gamelist.xml", "tests/fixtures/m3/esde.gamelist.xml"),
        ("pegasus", "data/frontends/pegasus.conf", os.path.join(OUT, "peg"),
         "nes/metadata.pegasus.txt", "tests/fixtures/m3/pegasus.metadata.txt"),
    ]

    for name, fe, dest, rel, golden in cases:
        if os.path.isdir(dest):
            subprocess.run(["rm", "-rf", dest], check=True)
        r = run_export(dest, fe, [A, B])
        check(f"{name} export rc=0", r.returncode == 0, r.stderr.strip())
        actual = os.path.join(dest, rel)
        if not os.path.exists(actual):
            check(f"{name} manifest exists", False, actual)
            continue
        with open(actual, "rb") as f:
            got = f.read()
        with open(os.path.join(HERE, "..", golden), "rb") as f:
            want = f.read()
        check(f"{name} golden byte-identical", got == want,
              f"{len(got)} vs {len(want)} bytes")

    # ES 清单结构 + 媒体实际落盘
    es_xml = os.path.join(OUT, "es", "nes", "gamelist.xml")
    tree = ET.parse(es_xml)
    games = tree.findall("game")
    check("ES gamelist parses", len(games) == 2, f"{len(games)} games")
    alpha = next(g for g in games if g.findtext("name") == "Alpha")
    beta = next(g for g in games if g.findtext("name") == "Beta")
    check("ES <image>", alpha.findtext("image") == "./media/Alpha-image.png")
    check("ES <thumbnail>", beta.findtext("thumbnail") == "./media/Beta-thumb.png")
    check("ES image file exists",
          os.path.isfile(os.path.join(OUT, "es", "nes", "media", "Alpha-image.png")))
    check("ES releasedate", alpha.findtext("releasedate") == "19900101T000000")
    check("ES rating", alpha.findtext("rating") == "0.85")
    check("ES multi-disc path uses m3u",
          beta.findtext("path") == "./Beta.m3u" and alpha.findtext("path") == "./Alpha.nes")

    # ES-DE：媒体不得写进 gamelist，但文件必须落 downloaded_media
    esde_xml = os.path.join(OUT, "esde", "gamelists", "nes", "gamelist.xml")
    tree2 = ET.parse(esde_xml)
    tags = {c.tag for g in tree2.findall("game") for c in g}
    check("ES-DE no media tags",
          not (tags & {"image", "thumbnail", "marquee", "video", "fanart"}),
          str(tags))
    check("ES-DE cover file exists",
          os.path.isfile(os.path.join(OUT, "esde", "downloaded_media", "nes",
                                      "covers", "Alpha.png")))

    # Pegasus：records + assets 相对路径真实存在
    peg = os.path.join(OUT, "peg", "nes", "metadata.pegasus.txt")
    text = open(peg, encoding="utf-8").read()
    check("Pegasus 2 records", text.count("game: ") == 2)
    check("Pegasus launch", "launch: retroarch -L core.so" in text)
    check("Pegasus extensions", "extensions: nes" in text)
    for rel in ["media/boxfront/Alpha.png", "media/screenshot/Beta.png"]:
        check(f"Pegasus asset exists {rel}",
              os.path.isfile(os.path.join(OUT, "peg", "nes", rel)))

    # m3u：按导出的碟文件重写
    m3u = os.path.join(OUT, "es", "nes", "Beta.m3u")
    want = "Beta.nes\nBeta (Disc 2).nes\n"
    check("m3u rewritten", os.path.isfile(m3u) and open(m3u, encoding="utf-8").read() == want)

    # XML 转义：Gamma 的描述含 & < > "
    esc_dest = os.path.join(OUT, "escape")
    if os.path.isdir(esc_dest):
        subprocess.run(["rm", "-rf", esc_dest], check=True)
    r = run_export(esc_dest, "data/frontends/es.conf", [G])
    check("escaping export rc=0", r.returncode == 0, r.stderr.strip())
    try:
        t = ET.parse(os.path.join(esc_dest, "nes", "gamelist.xml"))
        desc = t.find(".//desc").text
        check("escaping round-trip", desc == 'Gamma & <tag> "quote"', repr(desc))
    except ET.ParseError as ex:
        check("escaping round-trip", False, repr(ex))

    print(f"\n{passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
