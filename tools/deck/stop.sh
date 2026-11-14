#!/bin/bash
# stop.sh: ends everything of ours on the Deck and nothing else: the predtest-* user units (their cgroups), then the
# wineserver of our own prefix ~/predator-test/pfx, and puts the gamescope base layer back to the value saved before
# the first run (runs/../baselayer.orig) if it still names our APPID. Prints what is left.
T=${PREDATOR_TEST:-$HOME/predator-test}
APPID=${APPID:-7357}
PROTON_NAME=${PROTON_NAME:-Proton 10.0}
WS="$HOME/.local/share/Steam/steamapps/common/$PROTON_NAME/files/bin/wineserver"
export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/1000
for u in $(systemctl --user list-units --all --plain --no-legend 'predtest-*' | awk '{print $1}'); do
  echo "stop $u"; systemctl --user stop "$u"
done
[ -d "$T/pfx/pfx" ] && WINEPREFIX=$T/pfx/pfx "$WS" -k 2>/dev/null
base=$(xprop -root GAMESCOPECTRL_BASELAYER_APPID 2>/dev/null | sed -n 's/.*= *//p')
case "$base" in *"$APPID"*)
  orig=$(cat "$T/baselayer.orig" 2>/dev/null)
  [ -n "$orig" ] && xprop -root -f GAMESCOPECTRL_BASELAYER_APPID 32co -set GAMESCOPECTRL_BASELAYER_APPID "$orig" && echo "baselayer $base -> $orig";;
esac
echo "baselayer: $(xprop -root GAMESCOPECTRL_BASELAYER_APPID 2>/dev/null | sed 's/.*= //')"
# refresh cycle a run changed (run.sh GS_CYCLE) and could not put back
if [ -f "$T/cycle.orig" ]; then
  c=$(cat "$T/cycle.orig")
  [ -n "$c" ] && WAYLAND_DISPLAY=gamescope-0 timeout 5 gamescopectl debug_set_fps_limit "$c" > /dev/null 2>&1 && echo "refresh cycle -> $c"
  rm -f "$T/cycle.orig"
fi
for a in GAMESCOPE_DYNAMIC_REFRESH GAMESCOPE_FPS_LIMIT GAMESCOPE_DISPLAY_REFRESH_RATE_FEEDBACK; do
  echo "$a: $(xprop -root $a 2>/dev/null | sed 's/.*= //')"
done
echo "left (processes with our folder in their arguments or working folder):"
for p in $(pgrep -u "$(id -u)" .); do
  c=$(readlink /proc/$p/cwd 2>/dev/null); a=$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)
  case "$c $a" in *predator-test*) echo "  $p $c :: ${a:0:120}";; esac
done
