#!/bin/zsh
# usage: run.sh NAME [STOP_S]: boots xnu-NAME.img when ABL fastboot is there, runs, `oem stop` at STOP_S (default 60) while the kernel is alive, gets the full log
N=$1; [[ -n "$N" && "$N" != */* && "$N" != .* ]] || { echo "usage: $0 NAME (a plain name: the log dir $T/NAME is wiped first)"; exit 2; }; STOP=${2:-60}; T=${LOGDIR:-$HOME/hakios-mayfly-logs}; mkdir -p $T; D=$T/$N; rm -rf $D; mkdir -p $D
cd "$(cd "$(dirname "$0")" && pwd)/../../.."   # the top of hakios-xnu-kernel (this repo sits in devices/mayfly)
until timeout 5 fastboot devices | grep -v xnuloader | grep -q fastboot; do sleep 1; done
echo "boot $(date +%T)"; until fastboot boot xnu-$N.img 2>&1 | grep -q Finished; do sleep 2; done
for i in {1..60}; do timeout 5 fastboot devices | grep -q xnuloader && break; sleep 1; done
timeout 20 fastboot oem run 2>&1 | tail -1; S=$(date +%s); echo "run $(date +%T)"
while (( $(date +%s) - S < STOP )); do
  timeout 6 fastboot oem log >/dev/null 2>&1 && timeout 15 fastboot get_staged $D/live.part >/dev/null 2>&1 && mv $D/live.part $D/live
  sleep 1
done
echo "stop at $(( $(date +%s) - S )) s"; timeout 8 fastboot oem stop 2>&1 | tail -1
for i in {1..40}; do r=$(timeout 5 fastboot getvar result 2>&1 | head -1); case "$r" in *running*) sleep 2;; *) break;; esac; done; echo "$r"
timeout 30 fastboot get_staged $D/final >/dev/null 2>&1; echo "final $(wc -c < $D/final 2>/dev/null) bytes"
