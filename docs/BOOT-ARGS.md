# Boot arguments that matter on mayfly

`tools/build-image.sh NAME "ARGS"` always adds the base set:

```
loader_return_after=300 root_apfs=ufs_darwin ufs=2 bootcpu=7 cpus=7 touchscreen=1 touch_bitbang_ticks=4
gpu=1 panel_hz=120 fb_hz=120 ca_vbl=1 fb_flip=1
```

The daily image adds:

```
fb_hud=1 cpu_mhz=2400 adsp=1 sound=4 sound_sd=2 sound_below=80 sound_gain=0 speaker_sr=8 qcabt=6
wifi_pcie=12 wifi_lx=2 wifi_station=1 wifi_read_ms=0 wifi_wait_ms=10000 wifi_mac=<your MAC>
```

| Argument | Meaning |
|---|---|
| `root_apfs=ufs_darwin ufs=2` | root from the Darwin image on `userdata`, served by the loader |
| `bootcpu=7 cpus=7` | boot on the prime core, use 7 cores |
| `touchscreen=1 touch_bitbang_ticks=4` | ST FTS touch (and the PMIC buttons) |
| `gpu=1` | keep the Adreno 730 on for the window server (Mesa) |
| `adreno7=N adreno7_spin=1` | run GPU bring-up steps 1..N (debug) |
| `adreno7_speed=I` | GPU level index: 3 = 515, 4 = 580 (default), 5 = 645, 6 = 765, 7 = 816 MHz |
| `panel_hz=120 fb_hz=120 ca_vbl=1` | 120 Hz panel and vsync to Core Animation |
| `fb_flip=1` | double-buffered page flip on the MDP pipe (no tearing); default in build-image.sh |
| `fb_hud=1` | overlay: fps, hottest TSENS, CPU MHz, free memory, 60 s graph |
| `cpu_mhz=N` | CPU frequency ceiling; 2400 is a good daily value |
| `ddr_vote=0xNNNN` | DDR bandwidth vote (default 0x3fff = max) |
| `panel_bright=N` | panel brightness in ‰ at boot |
| `adsp=1 adsp_loud=1` | start the audio DSP (also needed for battery); loud = verbose |
| `battmgr=2` | dump the battery manager's properties |
| `sound=2/3/4` | 2 = tone, amps off; 3 = tone with amps; 4 = iOS audio |
| `sound_sd=2` | TDM data on gpio124 (correct for mayfly) |
| `sound_below=N` | attenuation in 1/8 dB (80 = -10 dB) |
| `sound_gain=0` | no extra gain (anything more clips) |
| `speaker_sr=8` | amplifier sample-rate code for 48 kHz |
| `speaker_spksw=0/1` | gpio52 (keep 0 = LOW for the speakers) |
| `speaker_only=0x34/0x35` | one amplifier only |
| `sound_matrix=N` | try a matrix of bus/amp formats (debug) |
| `qcabt=6` | Bluetooth up to the iOS bluetoothd bridge (lower numbers stop earlier) |
| `qcabt_delay=5 qcabt_wait_ms=60000 qcabt_pwrfollow=0` | Bluetooth timing knobs used during bring-up |
| `wifi_pcie=12 wifi_lx=2` | Wi-Fi bring-up through ath11k |
| `wifi_station=1` | hand the interface to configd for DHCP |
| `wifi_mac=xx:xx:..` | required; from `persist:qca6490/wlan_mac.bin` |
| `wifi_scan=1` | scan once and stop (debug) |
| `cpu_sample=1` / `glprof=500` / `gllog=1` | profiling: per-process CPU every 5 s / eagl GL profile / eagl frame logs |
| `island=1` | **do not use** (SpringBoard boot loop) |
| `loader_return_after=S` | the kernel returns to the loader after S seconds (test runs) |
