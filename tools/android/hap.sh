mount -t debugfs none /sys/kernel/debug 2>/dev/null
O=/data/local/tmp/hap; rm -rf $O; mkdir -p $O; cd $O
dmesg | grep -i -E 'hapt|vibrat|aw86|cs40|drv26|qti_hap|qcom-hv' > dmesg.txt
dumpsys vibrator_manager > vibrator_manager.txt 2>&1
ls -l /sys/class/leds /sys/class/qcom-haptics /sys/class/haptic* /sys/class/timed_output /dev/input 2>&1 > classes.txt
for d in /sys/class/qcom-haptics /sys/class/leds/vibrator /sys/class/leds/vibrator_* /sys/class/haptic*/*; do [ -d "$d" ] || continue; echo "== $d"; for f in $d/*; do [ -f "$f" ] && [ -r "$f" ] && echo "$(basename $f): $(timeout 1 cat $f 2>&1 | head -3 | tr '\n' ' ')"; done; done > sysfs.txt 2>&1
getevent -p > getevent.txt 2>&1
getprop | grep -i -E 'vibrat|haptic' > props.txt
ps -A | grep -i -E 'vibrat|hapt' > procs.txt
ls -la /vendor/firmware /vendor/etc 2>/dev/null | grep -i -E 'hapt|vib|aw8|\.rtp|\.bin$' | head -40 > firmware_list.txt
find /vendor /odm -iname '*vibrat*' -o -iname '*haptic*' 2>/dev/null > files.txt
ls /vendor/lib/modules > modules_list.txt 2>&1; grep -i -E 'hapt|vib|aw8|cs40' modules_list.txt > modules_hap.txt
ls /sys/kernel/debug/regmap 2>/dev/null | grep -i -E 'spmi|hapt|pm8350|7400' > regmap_list.txt
chmod -R a+r $O; echo ok
