# ReRed Faction II

Red Faction II (PC, 2003) for ARM64 handhelds through PortMaster. The game's
executable was statically recompiled to native ARM64 code; Direct3D 8 runs on
OpenGL ES, Miles Sound System and Bink are reimplemented (audio and movies
decoded with a built-in minimal FFmpeg). **No game files are included.**

## What you need (your own copy)
The GOG.com release of Red Faction II (DRM-free). Put its installer,
`setup_red_faction2_2.0.0.6.exe`, in `ports/rered_faction2/source/`.

Start **ReRed Faction II** from the Ports menu. The first run unpacks the
installer with PortMaster's innoextract (about 1.4 GB on the card, a few
minutes) and sets the game's display options to 640x480. `source/` is never
modified and the installer can be deleted afterwards.

## Launcher
Later runs open the launcher (DOS-style terminal):
- FPS counter, frame limit, look speed, CPU mode (MAX: performance governor at
  the device's rated maximum - on the H700 capped at 1.5 GHz - restored on
  exit; SYSTEM: firmware settings);
- **Controls**: `KEYS` (default) - the pad acts as keyboard and mouse through
  gptokeyb; `PAD` - the game's own Xbox-pad support (choose the controller
  under Options > Controls in game); `BOTH`;
- **Button mapping**: assign any pad button to each game action.

Default layout (adapted from the community Xpadder profile, for the game's
default keys): left stick move, right stick look / menu cursor, RT fire,
LT alt fire, A jump, B use, X reload, Y next weapon, RB grenade, R3 crouch,
L3 shift, Start menu, Back objectives.

To start a new single-player game: Single Player, then click the picture on
the right side, then click it again.

## Files
- `conf/`: settings, game config, profiles and saves (`conf/game/`), `log.txt`
  is next to the launcher.
- `data/`: the imported game, never written by the port.

## Licences
See `licenses/`: PCRecomp runtime, gptokeyb, FFmpeg (LGPL-2.1, statically
linked; source: FFmpeg n7.1, built by `scripts/build_ffmpeg.sh` of this port),
zlib.
