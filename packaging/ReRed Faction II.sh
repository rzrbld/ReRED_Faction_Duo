#!/bin/bash
# ReRed Faction II - Red Faction II (2003) for PortMaster.
# Structure follows the PortMaster "Basic Launchscript" template.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/rered_faction2
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export XDG_DATA_HOME="$CONFDIR"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# 1. Launcher: first run imports the user's own game (the GOG installer) from source/
#    (see README); later runs show settings. 0 = start, 10 = the user left.
export RERED_7Z="$controlfolder/7zzs.${DEVICE_ARCH}"
export RERED_INNO="$controlfolder/innoextract.${DEVICE_ARCH}"
"$GAMEDIR/rered-launcher.${DEVICE_ARCH}" --root "$GAMEDIR"
launcher_rc=$?
if [ "$launcher_rc" != 0 ]; then          # nothing has been changed yet: just leave
  [ "$launcher_rc" != 10 ] && echo "[rered] launcher failed ($launcher_rc)"
  pm_finish
  exit 0
fi

# 2. Game. Saves, game config (registry), screenshots: conf/. data/ is never written.
export RERED_USER_DIR="$CONFDIR"
[ -f "$CONFDIR/rered.cfg" ] && source "$CONFDIR/rered.cfg"
export RERED_FPS=${RERED_FPS:-1} RERED_LIMIT=${RERED_LIMIT:-0}
export RERED_STATS=${RERED_STATS:-0}     # 1: a performance line in log.txt every 5 s
[ -f "$CONFDIR/dev.cfg" ] && source "$CONFDIR/dev.cfg"   # developer switches (e.g. RERED_PROF=1)
GPTK="$GAMEDIR/rered.gptk"
[ -f "$CONFDIR/rered.gptk" ] && GPTK="$CONFDIR/rered.gptk"

# CPU: the game runs on one thread and is CPU-bound. "perf" switches the
# cores to the performance governor at the device's own maximum frequency.
# Exception: the Allwinner H700 (PortMaster's DEVICE_CPU) is rated 1.5 GHz but
# firmwares allow 1.8 GHz, an overclock that can overheat or destabilise it,
# so there the maximum is capped at 1512 MHz. Everything is restored on exit;
# RERED_CPU=system leaves the firmware's settings alone.
CPU_SAVED=""
cpu_restore() {
  local entry pol gov max
  for entry in $CPU_SAVED; do
    pol=${entry%%=*}; gov=${entry#*=}; max=${gov#*,}; gov=${gov%%,*}
    echo "$max" | $ESUDO tee "$pol/scaling_max_freq" >/dev/null 2>&1
    echo "$gov" | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  CPU_SAVED=""
}
cpu_perf() {
  local pol cap=""
  case "${DEVICE_CPU,,}" in h700) cap=1512000 ;; esac
  for pol in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -w "$pol/scaling_governor" ] || [ -n "$ESUDO" ] || continue
    grep -qw performance "$pol/scaling_available_governors" 2>/dev/null || continue
    CPU_SAVED="$CPU_SAVED $pol=$(cat "$pol/scaling_governor"),$(cat "$pol/scaling_max_freq")"
    if [ -n "$cap" ] && [ "$(cat "$pol/scaling_max_freq")" -gt "$cap" ]; then
      echo "$cap" | $ESUDO tee "$pol/scaling_max_freq" >/dev/null 2>&1
    fi
    echo performance | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  echo "[rered] cpu: performance governor${cap:+, capped at $((cap / 1000)) MHz (H700)} (restored on exit)"
}
# what is actually in effect, per cluster (a firmware may refuse the change)
cpu_report() {
  local pol
  for pol in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -r "$pol/scaling_governor" ] || continue
    echo "[rered] cpu ${pol##*/}: $(cat "$pol/related_cpus" 2>/dev/null | tr ' ' ',') $(cat "$pol/scaling_governor"), max $(( $(cat "$pol/scaling_max_freq") / 1000 )) of $(( $(cat "$pol/cpuinfo_max_freq") / 1000 )) MHz"
  done
}
trap cpu_restore EXIT
[ "${RERED_CPU:-perf}" = perf ] && cpu_perf
cpu_report

# Controls (RERED_JOY in conf/rered.cfg, set by the launcher):
#   0 (default) keyboard + mouse through gptokeyb (rered.gptk, remappable in
#     the launcher); the game's profile uses KEYBOARD/MOUSE
#   1 native controller only: the pad is an Xbox 360 DirectInput controller,
#     chosen in the game's Options > Controls (gptokeyb: exit hotkey only)
#   2 both, for switching the game's controller setting in its menus
case "${RERED_JOY:-0}" in
  0) export RERED_JOY=0; $GPTOKEYB "rered.${DEVICE_ARCH}" -c "$GPTK" & ;;
  1) export RERED_JOY=1; $GPTOKEYB "rered.${DEVICE_ARCH}" & ;;
  *) export RERED_JOY=1; $GPTOKEYB "rered.${DEVICE_ARCH}" -c "$GPTK" & ;;
esac
pm_platform_helper "$GAMEDIR/rered.${DEVICE_ARCH}"
if [ -n "$RERED_TIMEOUT" ]; then        # automated tests only
  timeout -k 5 "$RERED_TIMEOUT" ./rered.${DEVICE_ARCH} data
else
  ./rered.${DEVICE_ARCH} data
fi

cpu_restore
pm_finish
