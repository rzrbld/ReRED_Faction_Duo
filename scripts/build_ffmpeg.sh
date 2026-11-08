#!/usr/bin/env bash
# build_ffmpeg.sh - minimal static FFmpeg linked into the game for:
#   Bink movies (win32hle/bink.c): demuxer bink, decoders bink, binkaudio_rdft/dct
#   Miles MP3 music/voice (runtime/miles.c): demuxer mp3, decoder mp3float
# plus libswresample for sample-format conversion. No programs, no network.
# Output: build/ffmpeg/aarch64/{include,lib}. Source: FFmpeg n7.1 (LGPL-2.1+).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD VER=7.1
TARBALL=${FFMPEG_TARBALL:-$ROOT/work/ffmpeg-n$VER.tar.gz}
SHA=7ddad2d992bd250a6c56053c26029f7e728bebf0f37f80cf3f8a0e6ec706431a
[ -f "$TARBALL" ] || curl -sSL --retry 3 -o "$TARBALL" https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/refs/tags/n$VER
echo "$SHA  $TARBALL" | sha256sum -c -
SRC=$ROOT/work/FFmpeg-n$VER
[ -d "$SRC" ] || tar xf "$TARBALL" -C "$ROOT/work"
ARGS="--disable-everything --disable-programs --disable-doc --disable-network \
 --disable-autodetect --disable-avdevice --disable-swscale --disable-postproc --disable-avfilter \
 --enable-swresample --enable-static --disable-shared --enable-pic --enable-small --disable-debug \
 --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
 --enable-protocol=file --enable-demuxer=bink,mp3 --enable-parser=mpegaudio \
 --enable-decoder=bink,binkaudio_rdft,binkaudio_dct,mp3float,mp3"
mkdir -p build/logs
docker run --rm -u $(id -u):$(id -g) -v "$ROOT:/src" -w /src ${FFMPEG_IMAGE:-rered-cross:bookworm} bash -c "
  set -e; rm -rf build/ffmpeg/build-aarch64; mkdir -p build/ffmpeg/build-aarch64; cd build/ffmpeg/build-aarch64
  /src/work/FFmpeg-n$VER/configure --prefix=/src/build/ffmpeg/aarch64 --enable-cross-compile --arch=aarch64 \
    --target-os=linux --cross-prefix=aarch64-linux-gnu- --host-cc=gcc $ARGS
  make -j\$(nproc) && make install" > build/logs/ffmpeg.log 2>&1
ls -l build/ffmpeg/aarch64/lib/*.a
