# Logs from the phone

Real logs pulled over USB (`fastboot oem log` / `get_staged`, see `../tools/run.sh`). Each holds the
loader's and the kernel's messages, and the iOS system log the kernel forwards, up to the last 1.4 MB
(the buffer wraps on long runs).

**Empty, on purpose.** The 12S port's logs are the 12S's, and this port's first run has not happened.
The first one to add is the loader's own log:

```sh
fastboot boot xnu-unicorn-selftest.img
fastboot oem log > logs/selftest1.log        # the loader's [cpu] [display] [memmap] [payload] dump
```

Before it goes into git, **scrub it**: MAC addresses become `02:00:00:00:xx:yy` (consistently within a
file, so you can still follow one device), Wi-Fi network names `<ssid>`, public IPs `<ip>`, local paths
`~`, serials and IMEIs removed. The Bluetooth placeholder `00:00:00:00:5a:ad` is left as it is.

| File | Image | What it shows |
|---|---|---|
| *(none yet)* | | |

Filters that will matter once there is something to read:

```sh
LC_ALL=C tr -d '\0' < logs/NAME | grep -a 'fb_base\|width=\|stride='   # the real framebuffer
LC_ALL=C tr -d '\0' < logs/NAME | grep -a -A40 '\[memmap\]'            # the memory UEFI was given
LC_ALL=C tr -d '\0' < logs/NAME | grep -a 'sound: \|speaker: '         # audio graph and amplifiers
LC_ALL=C tr -d '\0' < logs/NAME | grep -a 'lx: \|qca6490'              # Wi-Fi bring-up
LC_ALL=C tr -d '\0' < logs/NAME | grep -a 'adreno7'                    # GPU steps
```
