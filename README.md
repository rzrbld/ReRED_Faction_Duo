# ReRed Faction II

**Beta.** Red Faction II (PC, 2003) running natively on ARM64 Linux handhelds
through PortMaster. The game's executable is statically recompiled to ARM64;
Direct3D 8 runs on OpenGL ES 3.2, Miles Sound System and Bink are
reimplemented (audio and movies via a small built-in FFmpeg).

Tested on the Anbernic RG40XX H (muOS). Other PortMaster devices with
OpenGL ES 3.2 may work.

**This repository contains no game files.** You need your own copy of
Red Faction II - you can buy it DRM-free on GOG.com:
<https://www.gog.com/en/game/red_faction_2>

## Install (prebuilt)

1. Download `rered_faction2.zip` from
   [Releases](https://github.com/rzrbld/ReRED_Faction_Duo/releases) and unpack it:
   `ReRed Faction II.sh` goes into your Ports launcher folder (on muOS
   `ROMS/Ports`), the `rered_faction2` folder into your ports folder (on muOS
   `ports` on the SD card).
2. Put the game's GOG.com installer, `setup_red_faction2_2.0.0.6.exe`, into
   `rered_faction2/source/`. You need your own copy of Red Faction II from
   [GOG.com](https://www.gog.com/en/game/red_faction_2) (DRM-free); the port is built for that
   release (`rf2.exe` SHA-256
   `7f0d187a246c9c8bcc1693b6c0dce3e87dd1585c9bb6fa691b85cee2b2159bef`).
3. Start **ReRed Faction II** from the Ports menu. The first run unpacks the
   installer with PortMaster's innoextract (about 1.4 GB, a few minutes); the
   installer can be deleted afterwards. Later runs open the launcher: FPS
   counter, frame limit, look speed, controls mode and button mapping.

Controls default to the pad acting as keyboard and mouse through gptokeyb
(left stick move, right stick look, RT fire, LT alt fire, A jump, B use,
X reload, Y next weapon, RB grenade, Start menu). The game's own Xbox-pad
support can be chosen in the launcher instead.

Saves and settings live in `rered_faction2/conf/`.

## Build

Requirements: Linux x86-64, Docker, Python 3 with `capstone` and `pefile`,
`innoextract`, a checkout of [PCRecomp](https://github.com/sp00nznet/pcrecomp),
and your GOG installer.

```sh
# toolchain image (Debian 12 + aarch64 cross compiler)
docker build -t rered-cross:bookworm -f docker/Dockerfile.cross docker

# minimal static FFmpeg (Bink + MP3 decoders)
scripts/build_ffmpeg.sh

# unpack the game from your GOG installer
innoextract -I app -d work/gog setup_red_faction2_2.0.0.6.exe

# translate rf2.exe to C (build/gen/)
PCRECOMP=/path/to/pcrecomp python3 tools/translate.py work/gog/app

# compile
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src rered-cross:bookworm \
    make -j$(nproc) ARCH=aarch64 OPT=-O2

# PortMaster package: dist/rered_faction2.zip
tools/package.sh
```

The generated code is derived from your copy of the game and is not part of
this repository.

## Credits and licences

- This project: MIT (see `LICENSE`).
- PCRecomp runtime (`third_party/pcrecomp`, MIT) - changes listed in
  `third_party/pcrecomp/LOCAL_CHANGES.md`.
- FFmpeg (LGPL-2.1+, statically linked, built by `scripts/build_ffmpeg.sh`),
  zlib, gptokeyb - licence texts are shipped in the package's `licenses/`.
- Default button layout adapted from the community "RF2 Controller Support
  (Xpadder)" profile.

Red Faction II is a trademark of its respective owners. This project is not
affiliated with or endorsed by them.
