#!/bin/bash
# run.sh <name> <seconds> [VAR=value ...]: one run of predator.exe under Proton in the Deck's gamescope session.
# Runs on the Deck, normally inside a systemd user unit (start.sh); everything stays under $T (default
# ~/predator-test): app/ (release folder), pfx/ (own Proton prefix), runs/<name>/ (results).
#   PROTON_NAME   Steam's Proton folder (default "Proton 10.0", as FCI/FCE on the Deck)
#   APPID         number for STEAM_GAME / the gamescope base layer while the run lasts (default 7357)
#   WAIT_LINE     regex in predator.log: <seconds> count from the first match (first start: build, then level)
#   MAX_WAIT      give up waiting for WAIT_LINE after this many seconds (default 1800)
# The game's own FCP_EXIT_AFTER ends a run cleanly; at the limit + 30 s the prefix's wineserver is killed (only our
# prefix). Every window whose name contains "Predator" gets STEAM_GAME (X reuses window ids, so each pass checks all
# of them; an untagged window was the likely cause of a gamescope abort in FCI's runs). The base layer is set to
# APPID while our window is up and put back to what it was before the run.
# Sensors once a second into sensors.csv: GPU clock, busy %, APU power, temperatures, CPU clock.
# Idea and structure after FCI/FCE's run2.sh (MIT, same author).
name=$1; secs=${2:-60}; shift 2
T=${PREDATOR_TEST:-$HOME/predator-test}
APP=$T/app; PFX=$T/pfx; O=$T/runs/$name
PROTON_NAME=${PROTON_NAME:-Proton 10.0}
APPID=${APPID:-7357}
STEAM=$HOME/.local/share/Steam
PROTON="$STEAM/steamapps/common/$PROTON_NAME/proton"
WS="$STEAM/steamapps/common/$PROTON_NAME/files/bin/wineserver"
U=$PFX/pfx/drive_c/users/steamuser/AppData/Local/Far-Cry-Instincts-Predator-Recomp
[ -n "$FCP_USER_ROOT_UNIX" ] && U=$FCP_USER_ROOT_UNIX
export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$STEAM STEAM_COMPAT_DATA_PATH=$PFX
for kv in "$@"; do export "$kv"; done
mkdir -p "$O" "$PFX"
log() { echo "$(date +%T) $*" >> "$O/run.txt"; }
[ -x "$PROTON" ] || { log "no proton at $PROTON"; exit 1; }
cd "$APP" || exit 1
env | grep -E '^(FCP_|REX_|PROTON_|DXVK|VKD3D|WINE)' | sort > "$O/env.txt"
rm -f "$U/predator.log"
base=$(xprop -root GAMESCOPECTRL_BASELAYER_APPID 2>/dev/null | sed -n 's/.*= *//p')
case "$base" in *"$APPID"*) base=$(cat "$T/baselayer.orig" 2>/dev/null);; *) echo "$base" > "$T/baselayer.orig";; esac
gs=$(pgrep -o gamescope-wl || pgrep -o -f "^gamescope ")
log "start $name: $secs s, proton '$PROTON_NAME', baselayer '$base', gamescope $gs"
SECONDS=0
"$PROTON" run ./predator.exe > "$O/proton.out" 2>&1 &
pid=$!
echo "t,sclk_mhz,mclk_mhz,gpu_busy,apu_w,gpu_temp_c,cpu_temp_c,cpu_mhz_avg,fan_rpm" > "$O/sensors.csv"
G=/sys/class/drm/card0/device
HW=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = amdgpu ] && echo $h; done | head -1)
HK=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = k10temp ] && echo $h; done | head -1)
HD=$(for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name)" = steamdeck_hwmon ] && echo $h; done | head -1)
limit=$secs; started=; based=0; ntag=0
[ -n "$WAIT_LINE" ] && limit=${MAX_WAIT:-1800}
while true; do
  sleep 1
  el=$SECONDS
  for w in $(xwininfo -root -children 2>/dev/null | grep -i '"[^"]*predator[^"]*"' | awk '{print $1}'); do
    cur=$(xprop -id "$w" STEAM_GAME 2>/dev/null | sed -n 's/.*= *//p')
    [ "$cur" = "$APPID" ] && continue
    xprop -id "$w" -f STEAM_GAME 32c -set STEAM_GAME "$APPID" 2>/dev/null || continue
    ntag=$((ntag + 1))
    log "window $w ($(xwininfo -id "$w" 2>/dev/null | sed -n 's/.*Window id: [^ ]* //p')) tagged at ${el}s, tag $ntag"
    if [ $based = 0 ]; then
      xprop -root -f GAMESCOPECTRL_BASELAYER_APPID 32co -set GAMESCOPECTRL_BASELAYER_APPID "$APPID"; based=1
      log "baselayer set to $APPID at ${el}s"
    fi
  done
  sclk=$(sed -n 's/^[0-9]*: \([0-9]*\)Mhz \*$/\1/p' $G/pp_dpm_sclk); mclk=$(sed -n 's/^[0-9]*: \([0-9]*\)Mhz \*$/\1/p' $G/pp_dpm_mclk)
  pw=$(cat $HW/power1_average 2>/dev/null || cat $HW/power1_input 2>/dev/null)
  cmhz=$(awk '{s+=$1; n++} END {if (n) printf "%d", s/n/1000}' /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq)
  echo "$el,$sclk,$mclk,$(cat $G/gpu_busy_percent),$(awk -v p="$pw" 'BEGIN{printf "%.2f", p/1e6}'),$(($(cat $HW/temp1_input)/1000)),$(($(cat $HK/temp1_input)/1000)),$cmhz,$(cat $HD/fan1_input 2>/dev/null)" >> "$O/sensors.csv"
  if [ -n "$WAIT_LINE" ] && [ -z "$started" ] && grep -qaE "$WAIT_LINE" "$U/predator.log" 2>/dev/null; then
    started=$el; limit=$((el + secs)); log "wait line at ${el}s, limit now ${limit}s"
  fi
  if ! kill -0 $pid 2>/dev/null; then log "proton exited at ${el}s"; break; fi
  if [ $el -gt $((limit + 30)) ]; then log "limit ${limit}s + 30 s reached: wineserver -k"; break; fi
done
WINEPREFIX=$PFX/pfx timeout 20 "$WS" -w || { log "wineserver -k"; WINEPREFIX=$PFX/pfx "$WS" -k; sleep 2; }
wait $pid; rc=$?
if [ $based = 1 ]; then
  xprop -root -f GAMESCOPECTRL_BASELAYER_APPID 32co -set GAMESCOPECTRL_BASELAYER_APPID "$base"
  log "baselayer restored to $base"
fi
cp -a "$U/predator.log" "$U"/predator.gen*.log "$O/" 2>/dev/null
[ -d "$U/logs" ] && { mkdir -p "$O/logs"; mv "$U"/logs/* "$O/logs/" 2>/dev/null; }
log "end after ${SECONDS}s, proton rc $rc, tags $ntag, baselayer now '$(xprop -root GAMESCOPECTRL_BASELAYER_APPID | sed 's/.*= //')', gamescope $(pgrep -o gamescope-wl || pgrep -o -f "^gamescope ") (was $gs)"
