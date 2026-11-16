#!/bin/bash
# v030.sh: v0.3.0 checks on the Steam Deck (docs/phase1/deck-v0.3.0.md): F10 menu Display/Controller pages with the
# Deck's own controller in the port list, frame rate switched live 30 -> 45 -> 60 -> auto with display pacing,
# Defaults on Display and Controller, button capture through a virtual SDL pad under Proton.
# Start: ~/predator-test/tools/start.sh v030 v030.sh APP_DIR=$HOME/predator-test/app-v030 [PARTS="first fps menu cap"]
# Runs land in ~/predator-test/runs/W_<part>; menu dumps in runs/W_<part>/logs, gamescope screenshots in runs/W_<part>/gs.
T=${PREDATOR_TEST:-$HOME/predator-test}
X=$T/tools/run.sh
for kv in "$@"; do export "$kv"; done
A=${APP_DIR:-$T/app-v030}
PARTS=${PARTS:-first fps menu cap}
log() { echo "$(date +%T) $*" >> "$T/runs/v030.txt"; }
B="APP_DIR=$A FCP_USER_ROOT_UNIX=$A FCP_NO_DIALOGS=1 FCP_FPS_STATS=5"
shots() {  # shots <run> <seconds...> (wall seconds after the call) in the background
  local run=$1 last=0; shift; mkdir -p "$T/runs/$run/gs"
  ( for s in "$@"; do sleep $((s - last)); last=$s
      WAYLAND_DISPLAY=gamescope-0 XDG_RUNTIME_DIR=/run/user/1000 timeout 5 gamescopectl screenshot "$T/runs/$run/gs/gs_$s.png" > /dev/null 2>&1
    done ) &
}
keep() { cp "$A/predator.toml" "$T/runs/$1/predator.toml.after" 2>/dev/null; cp "$A/predator_input.toml" "$T/runs/$1/predator_input.toml.after" 2>/dev/null; }
for p in $PARTS; do
  case $p in
  first)  # first start in this folder: game code (reused or built), shader pack, level
    "$X" W_first 20 $B FCP_START_LEVEL=21_dunea FCP_EXIT_AFTER=40 WAIT_LINE='framestat: .*level 21_dunea' MAX_WAIT=1800
    keep W_first
    log "first: $(grep -a -E 'game code: .*(ready in|first-start build in|not used)' "$T/runs/W_first/predator.log" | head -2 | sed 's/^ *[0-9.]* *[0-9]* //' | tr '\n' ' ')";;
  fps)  # live frame rate changes while playing (title clock): 30 at 25 s, 45 at 50 s, 60 at 75 s, auto at 100 s;
        # the menu is opened only at the end (closing F10 leaves the game in its own pause menu)
    shots W_fps 40 122
    "$X" W_fps 140 $B FCP_START_LEVEL=21_dunea FCP_INPUT_SCRIPT_CLOCK=level FCP_INPUT_SCRIPT="8-115:lup,20-21:rright:0.5,50-51:rright:0.5,80-81:rright:0.5" \
      FCP_TEST_MENU="25:set=display.fps=30,50:set=display.fps=45,75:set=display.fps=60,100:set=display.fps=auto,122:open,123:tab=display,124:dump=fps_display_auto.png,126:close" \
      FCP_EXIT_AFTER=130
    wait; keep W_fps; log "fps: done";;
  menu)  # Display and Controller pages, swap A/B by binding, Defaults on both pages (title screen and level)
    shots W_menu 16 20 26 34 42
    "$X" W_menu 90 $B FCP_START_LEVEL=21_dunea \
      FCP_TEST_MENU="14:open,15:tab=display,16:dump=m_display.png,18:set=display.fps=60,19:set=display.frame_queue=3,20:dump=m_display_changed.png,22:defaults=display,23:dump=m_display_defaults.png,25:tab=controller,26:dump=m_ctrl.png,28:padbind=a:b,29:padbind=b:a,31:dump=m_ctrl_swapped.png,32:scroll=1,33:dump=m_ctrl_end.png,34:scroll=0,36:defaults=controller,37:dump=m_ctrl_defaults.png,39:close" \
      FCP_EXIT_AFTER=60
    wait; keep W_menu; log "menu: done";;
  cap)  # button capture: virtual SDL pad next to the Deck pad; capture a <- b, b <- a, then in game, restart, Defaults
    shots W_cap 18 26
    "$X" W_cap 80 $B FCP_START_LEVEL=21_dunea FCP_VIRTUAL_PADS=1 \
      FCP_TEST_MENU="14:open,15:tab=controller,16:dump=c_ports.png,18:padcapture=a,24:padcapture=b,30:dump=c_swapped.png,32:close" \
      FCP_VIRTUAL_PAD_SCRIPT="20:b,26:a,40:a,42:b,44:a,46:b" FCP_EXIT_AFTER=55
    wait; keep W_cap
    "$X" W_cap2 70 $B FCP_START_LEVEL=21_dunea FCP_VIRTUAL_PADS=1 \
      FCP_TEST_MENU="14:open,15:tab=controller,16:dump=c2_persist.png,30:defaults=controller,31:dump=c2_defaults.png,33:close" \
      FCP_VIRTUAL_PAD_SCRIPT="20:a,22:b,40:a,42:b" FCP_EXIT_AFTER=50
    keep W_cap2; log "cap: done";;
  esac
done
log "v030 parts done: $PARTS"
