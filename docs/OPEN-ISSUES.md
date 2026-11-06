# Open issues (2026-11-01)

Ordered by how much they hurt daily use. Each has what is known and the best next step. Help welcome.

1. **Resets under heavy load.** The phone reboots now and then, always around 86-94 °C (TSENS); it sees 93 °C
   about 40 s after boot. Heat or power is suspected, not proven. Next: halve `ddr_vote`, GPU at 515 MHz
   (`adreno7_speed=3`), measure which helps; the real fix is load-based DCVS for CPU, GPU and DDR.
2. **Memory.** With apps open at most ~500 MB stays free and many apps freeze the UI. Logs say
   "compressor 0 (holding 0) MB": the VM compressor may not be running. Next: check vm_compressor and the
   jetsam/memorystatus logs.
3. **Frame drops when apps open.** GPU submission is the hot spot (`glFlush` / IOConnectCallStructMethod ~7%,
   3-4 submits a frame, ~68 buffers); Mesa reports `submit failed: -5` about 11 times in 70 s.
4. **Battery Health / Charging page spins.** `startSpinner` is called, `clearSpinner` never; `isAuthSpinner` is 0,
   so it waits for other data. MaximumCapacityPercent, BatteryData, Serial keys and an AppleAuthCPRelay nub
   were added (drivers/power/hakios.battery.cpp) — not enough.
5. **Wi-Fi start is flaky** (~50%: no QMI answer, or a reset at bus start). No retry logic yet; suspected race
   with the ADSP start.
6. **Bottom amplifier (0x35)** current sense reads 0: the bottom speaker may be silent; not checked by ear.
7. **YouTube "error licensing this video"** with a Bluetooth speaker connected (fine without BT). mediaserverd
   SIGKILLs itself, fairplayd keeps exiting. FairPlay is suspected, not proven.
8. **Dynamic Island**: four approaches failed (see FINDINGS.md, "User space"). Next: re-enable ReportCrash (remove it
   from mkjobs.py DENY) and find where SpringBoard's SIGSEGV is.
9. Not started: microphone, camera, haptics (PM8350B hv-haptics), cellular, sensors, charging control, USB in iOS,
   GPU DCVS, hardware scaling on pipe vig0 (needs an MDP SMMU mapping for iOS surfaces), 120 fps checked inside apps.
10. **Housekeeping**: the UEFI debug scaffolding (`uefi/patches/`) has to be cleaned and the mayfly parts sent to
    hakios-uefi; the Wi-Fi status-bar icon stays grey while internet works; the IP field in Settings is empty.
