#!/bin/bash
# start.sh <tag> <script> [args...]: runs <script> (relative to ~/predator-test/tools) as the systemd user unit
# predtest-<tag>, so the run survives the end of the SSH session (a process started with nohup/setsid died with it in
# FCI's Deck runs) and everything it starts is in one cgroup. Output: ~/predator-test/runs/<tag>.unit.txt.
T=${PREDATOR_TEST:-$HOME/predator-test}
tag=$1; script=$2; shift 2
mkdir -p "$T/runs"
if systemctl --user is-active --quiet "predtest-$tag"; then echo "predtest-$tag is already running"; exit 1; fi
systemd-run --user --collect --unit="predtest-$tag" -p WorkingDirectory="$T" \
  --setenv=PREDATOR_TEST="$T" ${PROTON_NAME:+--setenv=PROTON_NAME="$PROTON_NAME"} \
  /bin/bash -c "exec /bin/bash \"$T/tools/$script\" \"\$@\" > \"$T/runs/$tag.unit.txt\" 2>&1" _ "$@"
