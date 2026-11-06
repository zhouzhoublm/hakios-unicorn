#!/bin/zsh
# Everything worth having from the running Android (read-only; needs adb and root through Magisk). OUT= says where (default ./android-dump).
# The vendor tarballs at the end are proprietary: keep them to yourself.
D=${OUT:-$PWD/android-dump}/all; mkdir -p $D; cd $D
r() { timeout ${3:-30} adb shell "su -c '$2'" > "$1" 2>&1; }
r dmesg.txt dmesg 60; r getprop.txt getprop; r mounts.txt 'cat /proc/mounts'; r lsmod.txt 'cat /proc/modules'; r interrupts.txt 'cat /proc/interrupts'; r iomem.txt 'cat /proc/iomem'
r clk_summary.txt 'cat /sys/kernel/debug/clk/clk_summary' 60; r regulator_summary.txt 'cat /sys/kernel/debug/regulator/regulator_summary' 60; r gpio.txt 'cat /sys/kernel/debug/gpio'; r pinmux.txt 'cat /sys/kernel/debug/pinctrl/f000000.pinctrl/pinmux-pins'
r tinymix.txt 'tinymix' 60; r asound_cards.txt 'cat /proc/asound/cards; cat /proc/asound/pcm'; r audio_flinger.txt 'dumpsys media.audio_flinger' 40; r audio_policy.txt 'dumpsys media.audio_policy' 40
r gpu.txt 'cd /sys/class/kgsl/kgsl-3d0; for f in gpu_model gpu_clock_stats gpu_available_frequencies freq_table_mhz max_gpuclk gpuclk gpu_busy_percentage devfreq/governor devfreq/available_frequencies devfreq/cur_freq devfreq/max_freq devfreq/min_freq max_pwrlevel min_pwrlevel num_pwrlevels thermal_pwrlevel bus_split; do echo == \$f; cat \$f 2>&1; done'
r cpu.txt 'cd /sys/devices/system/cpu; for c in cpu[0-7]; do echo == \$c; cat \$c/cpufreq/scaling_available_frequencies \$c/cpufreq/cpuinfo_max_freq \$c/cpufreq/scaling_cur_freq \$c/cpufreq/scaling_governor 2>&1; cat \$c/cpufreq/related_cpus 2>&1; done; cat /proc/cpuinfo | head -12'
r display.txt 'dumpsys SurfaceFlinger' 40; r display2.txt 'dumpsys display' 40; r battery.txt 'dumpsys battery; cat /sys/class/power_supply/battery/uevent' 20; r thermal.txt 'for z in /sys/class/thermal/thermal_zone*; do echo \$z \$(cat \$z/type) \$(cat \$z/temp); done' 40; r camera.txt 'dumpsys media.camera' 40; r sensors.txt 'dumpsys sensorservice' 30; r bluetooth.txt 'dumpsys bluetooth_manager' 40; r wifi.txt 'dumpsys wifi' 40; r touch.txt 'dumpsys input' 30; r usb.txt 'dumpsys usb' 20
r partitions.txt 'ls -l /dev/block/by-name; cat /proc/partitions'
for f in /vendor/etc/audio /vendor/etc/acdbdata /vendor/etc/mixer_paths_waipio_mtp.xml /vendor/etc/resourcemanager_waipio_mtp.xml /vendor/etc/usecaseKvManager.xml /vendor/etc/aw882xx_acf.bin /vendor/etc/audio_policy_configuration.xml /vendor/etc/audio_platform_info.xml /vendor/etc/bluetooth /vendor/etc/init/hw /vendor/firmware/aw882xx_acf.bin; do adb pull "$f" vendor/ >/dev/null 2>&1; done
mkdir -p dt; timeout 60 adb shell 'su -c "cat /sys/firmware/fdt"' > dt/live.dtb 2>/dev/null
ls -la | wc -l; du -sh .

# vendor trees (last session took these with root: config 679 MB, modules, camera ~200 MB)
V=${OUT:-$PWD/android-dump}/vendor-etc; mkdir -p $V; cd $V
timeout 300 adb exec-out 'su -c "tar -cf - /vendor/etc /vendor/firmware /vendor/lib/modules /odm/etc 2>/dev/null"' > vendor-config.tar
timeout 120 adb exec-out 'su -c "tar -cf - /vendor_dlkm/lib/modules /vendor/bin/hw 2>/dev/null"' > vendor-modules.tar
timeout 200 adb exec-out 'su -c "tar -cf - /vendor/lib64/camera /vendor/lib/camera 2>/dev/null"' > vendor-camera.tar
ls -la $V
