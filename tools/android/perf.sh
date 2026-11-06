mount -t debugfs none /sys/kernel/debug 2>/dev/null
O=/data/local/tmp/perf; rm -rf $O; mkdir -p $O; cd $O
sec() { echo "=== $1"; shift; "$@" 2>&1; }
{ cd /sys/devices/system/cpu; for c in cpu[0-7]; do echo "== $c"; for f in related_cpus cpuinfo_min_freq cpuinfo_max_freq scaling_max_freq scaling_min_freq scaling_cur_freq scaling_governor scaling_available_frequencies; do echo "$f: $(cat $c/cpufreq/$f 2>&1)"; done; ls $c/cpufreq/walt 2>/dev/null | while read f; do echo "walt/$f: $(cat $c/cpufreq/walt/$f 2>&1)"; done; done; cd $O; } > cpufreq.txt
{ for d in /sys/class/devfreq/*; do echo "== $d"; for f in governor cur_freq min_freq max_freq available_frequencies; do echo "$f: $(cat $d/$f 2>&1)"; done; done; } > devfreq.txt
{ find /sys/devices/system/cpu/bus_dcvs -type f 2>/dev/null | while read f; do echo "$f: $(cat $f 2>&1 | head -3 | tr '\n' ' ')"; done; } > bus_dcvs.txt
{ find /sys/class/kgsl/kgsl-3d0 -maxdepth 2 -type f 2>/dev/null | grep -v -E 'uevent|snapshot|reset|ft_|dump' | while read f; do echo "$f: $(cat $f 2>&1 | head -3 | tr '\n' ' ')"; done; } > kgsl.txt
cat /sys/kernel/debug/interconnect/interconnect_summary > icc_summary.txt 2>&1
cat /sys/kernel/debug/interconnect/interconnect_graph > icc_graph.txt 2>&1
ls -R /sys/kernel/debug/dri > dri_list.txt 2>&1
for f in /sys/kernel/debug/dri/0/state /sys/kernel/debug/dri/0/summary /sys/kernel/debug/dri/0/clients /sys/kernel/debug/dri/0/debug/dump; do [ -r $f ] && timeout 5 cat $f > dri_$(basename $f).txt 2>&1; done
for f in /sys/class/drm/card0-DSI-1/modes /sys/class/drm/card0-DSI-1/mode /sys/class/drm/card0-DSI-1/dynamic_fps /sys/class/backlight/*/brightness /sys/class/backlight/*/max_brightness; do echo "$f: $(cat $f 2>&1 | tr '\n' ' ')"; done > drm.txt
sysctl -a 2>/dev/null | grep -E '^kernel\.sched|^vm\.' > sysctl.txt
{ for f in /sys/kernel/msm_performance/parameters/* /sys/module/msm_performance/parameters/* /proc/sys/walt/*; do [ -f $f ] && echo "$f: $(cat $f 2>&1 | head -2 | tr '\n' ' ')"; done; } > walt.txt
cat /sys/kernel/debug/llcc*/* > llcc.txt 2>&1
cat /proc/meminfo > meminfo.txt; cat /proc/zoneinfo | head -60 > zone.txt; cat /proc/cmdline > cmdline.txt; cat /proc/device-tree/model > model.txt 2>&1
cat /sys/kernel/debug/gmu/* > gmu.txt 2>&1; ls /sys/kernel/debug/kgsl/kgsl-3d0 > kgsl_debug_list.txt 2>&1
dumpsys SurfaceFlinger --latency > sf_latency.txt 2>&1; dumpsys gfxinfo > gfxinfo.txt 2>&1; dumpsys cpuinfo > cpuinfo.txt 2>&1; dumpsys thermalservice > thermalservice.txt 2>&1; dumpsys power > power.txt 2>&1
getprop | grep -i -E 'hwui|renderthread|sf\.|surface_flinger|vendor.display|debug.sf|perf\.' > render_props.txt
chmod -R a+r $O; echo done
