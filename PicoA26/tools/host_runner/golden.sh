#!/bin/sh
# Golden regression: per-ROM total frame hash + audio hash with a fixed pad script.
# usage: golden.sh <rom_dir> [record]   (ROMs are not in the repo; they come from the SD card)
R="$(dirname "$0")/../../build_host_win/a26run.exe"; G="$(dirname "$0")/golden/hashes.txt"
T="${TMPDIR:-/tmp}/a26golden"; mkdir -p "$T"; out=""
for spec in "pitfall:0-1500:none;200-400:right;500-560:fire;600-900:left" \
            "enduro:60-70:reset;200-1400:fire;300-500:left;600-800:right" \
            "riverraid:60-70:reset;100-1400:up;300-400:left;500-600:right;200-1400:fire" \
            "freeway:60-70:reset;100-1400:up" \
            "spaceinv:60-70:reset;100-1400:fire;300-500:left;600-800:right" \
            "combat:60-70:reset;100-1400:up+fire;300-500:left"; do
  name=${spec%%:*}; script=${spec#*:}
  res=$("$R" "$1/$name.a26" --frames 1500 --input "$script" --out "$T" 2>/dev/null | grep total_hash)
  wav=$(md5sum "$T/audio.wav" | cut -c1-8)
  out="$out$name $res wav=$wav
"
done
if [ "$2" = record ]; then printf "%s" "$out" > "$G"; echo recorded; cat "$G"
else printf "%s" "$out" | diff "$G" - && echo GOLDEN_OK; fi
