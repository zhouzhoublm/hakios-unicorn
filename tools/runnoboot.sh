#!/bin/zsh
# usage: runnoboot.sh NAME  (the image already booted by hand: waits for xnuloader, 120 s run, collects log into $T/NAME)
N=$1; [[ -n "$N" && "$N" != */* && "$N" != .* ]] || { echo "usage: $0 NAME (a plain name: the log dir $T/NAME is wiped first)"; exit 2; }; T=${LOGDIR:-$HOME/hakios-unicorn-logs}; mkdir -p $T; D=$T/$N; rm -rf $D; mkdir -p $D
cd "$(cd "$(dirname "$0")" && pwd)/../../.."   # the top of hakios-xnu-kernel (this repo sits in devices/unicorn)


for i in {1..60}; do timeout 5 fastboot devices | grep -q xnuloader && break; sleep 1; done
timeout 5 fastboot devices | grep -q xnuloader && echo "xnuloader up $(date +%T)" || echo "NO xnuloader after 60 s"
timeout 20 fastboot oem run 2>&1 | tail -1; S=$(date +%s); echo "run $(date +%T)"
ok=0; bad=0
while (( $(date +%s) - S < 140 )); do
  r=$(timeout 5 fastboot getvar result 2>&1 | head -1)
  case "$r" in *returned*|*done*|*panic*) echo "result: $r at $(( $(date +%s) - S )) s"; break;; esac
  if timeout 6 fastboot oem log >/dev/null 2>&1 && timeout 15 fastboot get_staged $D/live.part >/dev/null 2>&1; then mv $D/live.part $D/live; ok=$((ok+1)); else bad=$((bad+1)); fi
  sleep 3
done
echo "live logs ok $ok, failed $bad"
for i in {1..60}; do timeout 5 fastboot devices | grep -q xnuloader && break; sleep 2; done
timeout 8 fastboot oem stop >/dev/null 2>&1
for i in {1..30}; do r=$(timeout 5 fastboot getvar result 2>&1 | head -1); case "$r" in *running*) sleep 2;; *) break;; esac; done; echo "$r"
timeout 30 fastboot get_staged $D/final >/dev/null 2>&1; echo "final $(wc -c < $D/final 2>/dev/null) bytes, live $(wc -c < $D/live 2>/dev/null)"
