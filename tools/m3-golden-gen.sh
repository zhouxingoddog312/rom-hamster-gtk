#!/bin/bash
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$HOME/.cache/romham-m3/check"
export XDG_CONFIG_HOME="$ROOT/config"
A=nes-c9a7d94a2f3f
B=nes-c0ba3f479def
rm -rf "$ROOT/out"
./build/rom-hamster export "$ROOT/out/es" --frontend ./data/frontends/es.conf \
  --id "$A" --id "$B" --media || exit 1
./build/rom-hamster export "$ROOT/out/esde" --frontend ./data/frontends/es-de.conf \
  --id "$A" --id "$B" --media || exit 1
./build/rom-hamster export "$ROOT/out/peg" --frontend ./data/frontends/pegasus.conf \
  --id "$A" --id "$B" --media || exit 1
echo "===ES"; cat "$ROOT/out/es/nes/gamelist.xml"
echo "===ESDE"; cat "$ROOT/out/esde/gamelists/nes/gamelist.xml"
echo "===PEG"; cat -A "$ROOT/out/peg/nes/metadata.pegasus.txt"
