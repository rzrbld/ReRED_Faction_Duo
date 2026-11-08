#!/bin/sh
# package.sh - assemble the PortMaster package in dist/ (no game files).
#   dist/rered_faction2.zip: ReRed Faction II.sh, rered_faction2/{rered.aarch64,
#   rered-launcher.aarch64, rered-import.aarch64, rered.gptk, licenses/,
#   README.md, port.json, gameinfo.xml, screenshot.png, source/}
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/dist/stage
rm -rf "$OUT"; mkdir -p "$OUT/rered_faction2/licenses"
cp "$ROOT/packaging/ReRed Faction II.sh" "$OUT/"
P=$OUT/rered_faction2
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -w /src rered-cross:bookworm sh -c '
  aarch64-linux-gnu-strip -o dist/stage/rered_faction2/rered.aarch64 build/aarch64/rered
  aarch64-linux-gnu-gcc -O2 -o dist/stage/rered_faction2/rered-launcher.aarch64 launcher/rered_launcher.c $(pkg-config --cflags --libs sdl2)
  aarch64-linux-gnu-gcc -O2 -o dist/stage/rered_faction2/rered-import.aarch64 tools/import/rered-import.c -Wl,-Bstatic -lz -Wl,-Bdynamic
  aarch64-linux-gnu-strip dist/stage/rered_faction2/rered-launcher.aarch64 dist/stage/rered_faction2/rered-import.aarch64
  cp /usr/share/doc/zlib1g/copyright dist/stage/rered_faction2/licenses/LICENSE-zlib.txt'
cp "$ROOT/packaging/rered.gptk" "$P/"
cp "$ROOT/packaging/licenses/LICENSE-gptokeyb.txt" "$P/licenses/"
cp "$ROOT/LICENSE" "$P/licenses/LICENSE-rered.txt"
cp "$ROOT/third_party/pcrecomp/LICENSE" "$P/licenses/LICENSE-pcrecomp.txt" 2>/dev/null || true
cp "$ROOT/work/FFmpeg-n7.1/COPYING.LGPLv2.1" "$P/licenses/LICENSE-ffmpeg-LGPL-2.1.txt"
printf 'FFmpeg n7.1 (https://ffmpeg.org), LGPL-2.1+, statically linked.\nSource: https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n7.1\nConfiguration: scripts/build_ffmpeg.sh of this port (bink, binkaudio, mp3 decoders only).\n' > "$P/licenses/FFMPEG-SOURCE.txt"
cp "$ROOT/packaging/portmaster/README.md" "$ROOT/packaging/portmaster/port.json" "$ROOT/packaging/portmaster/gameinfo.xml" "$P/"
[ -f "$ROOT/packaging/portmaster/screenshot.png" ] && cp "$ROOT/packaging/portmaster/screenshot.png" "$P/"
mkdir -p "$P/source" && echo "Put your GOG.com installer of Red Faction II here: setup_red_faction2_2.0.0.6.exe (see ../README.md)." > "$P/source/PUT_YOUR_GAME_FILES_HERE.txt"
(cd "$OUT" && rm -f ../rered_faction2.zip && zip -qr ../rered_faction2.zip "ReRed Faction II.sh" rered_faction2)
echo "dist/rered_faction2.zip:"; unzip -l "$ROOT/dist/rered_faction2.zip" | tail -n +4 | head -40
