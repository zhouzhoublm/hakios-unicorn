O=/data/local/tmp/m; rm -rf $O; mkdir -p $O/hapfw; cd $O
for f in $(ls /vendor/firmware | grep -i -E 'hapt|rtp|vib|lra'); do cp /vendor/firmware/$f hapfw/ 2>/dev/null; done
for s in phone telephony.registry carrier_config isub ims iphonesubinfo connectivity netstats; do timeout 25 dumpsys $s > dumpsys_$s.txt 2>&1; done
getprop | grep -i -E 'gsm\.|ril|radio|telephony|modem|mcfg|ims|persist.vendor.dpm|data|qcril|sim' > radio_props.txt
dmesg | grep -i -E 'mpss|modem|remoteproc|sysmon|qrtr|glink.*mpss|ipa_|rmnet|subsys|pil|qmi.*wda|cnss-|esoc|sdx|mhi' > dmesg_modem.txt
ls -la /dev/block/by-name | grep -i -E 'modem|fsg|fsc|mdm|persist|dsp|apdp|mpss' > parts.txt
ls -la /vendor/firmware_mnt/image 2>/dev/null | head -80 > firmware_mnt.txt
ls -R /vendor/rfs /mnt/vendor/persist/rfs /vendor/firmware_mnt/image/modem_pr 2>/dev/null | head -200 > rfs_list.txt
ls -l /sys/class/remoteproc/ > remoteproc.txt 2>&1; for r in /sys/class/remoteproc/*; do echo "$r $(cat $r/name 2>/dev/null) $(cat $r/state 2>/dev/null) $(cat $r/firmware 2>/dev/null)"; done >> remoteproc.txt
ip addr > ip_addr.txt 2>&1; ip route > ip_route.txt 2>&1; cat /proc/net/dev > netdev.txt
ps -A | grep -i -E 'rild|qcrild|cnd|netmgr|ims|dpm|qti|port-bridge|modem|rmt_storage|tftp|pd-mapper|pm-service|qrtr' > procs.txt
ls /vendor/etc/init | grep -i -E 'qcril|rild|radio|ims|cne|dpm|netmgr|data|modem|rmt|tftp|pd_map|pm-|qrtr' > init_list.txt
chmod -R a+r $O; echo ok
