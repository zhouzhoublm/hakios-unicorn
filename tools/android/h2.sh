mount -t debugfs none /sys/kernel/debug 2>/dev/null
O=/data/local/tmp/h2; rm -rf $O; mkdir -p $O/fw; cd $O
ls /vendor/firmware | grep -i -E 'hapt|rtp|vib|f0|lra' > fwlist.txt
for f in $(cat fwlist.txt); do cp /vendor/firmware/$f fw/ 2>/dev/null; done
ls -R /sys/kernel/debug/haptics /sys/kernel/debug/qcom-hv-haptics > dbg_list.txt 2>&1
for d in /sys/kernel/debug/haptics /sys/kernel/debug/qcom-hv-haptics; do [ -d $d ] && find $d -type f | while read f; do echo "== $f"; timeout 1 cat $f 2>&1 | head -40; done; done > dbg_vals.txt 2>&1
ls /sys/kernel/debug/regmap > regmaps.txt 2>&1
ls /sys/kernel/debug/spmi* > spmi_list.txt 2>&1
i=0; while [ $i -lt 6 ]; do echo "--- t$i"; for r in /sys/kernel/debug/regmap/*f000* /sys/kernel/debug/regmap/*0-03*; do [ -r "$r/registers" ] && { echo "$r"; timeout 2 grep -E '^f[0-2][0-9a-f]{2}:' "$r/registers" | head -300; }; done; i=$((i+1)); sleep 0.4; done > regs_live.txt 2>&1
chmod -R a+r $O; echo ok
