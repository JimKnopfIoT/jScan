#!/bin/sh
# Capture USB traffic of Epson's driver (epkowa) for a series of scan settings.
# Usage: capture-series.sh outdir
set -u
out=$1; mkdir -p "$out"
bus=$(lsusb -d 04b8:013a | sed -n 's/^Bus 0*\([0-9]*\).*/\1/p')
run() {
    name=$1; shift
    sudo -n tcpdump -i usbmon$bus -s 0 -U -w "$out/$name.pcap" 2>/dev/null &
    sleep 1
    scanimage -d epkowa "$@" --format=pnm -o "$out/$name.pnm" 2>&1 | sed "s/^/  $name: /"
    sleep 1
    sudo -n pkill -INT -f "tcpdump -i usbmon$bus" ; wait
    echo "$name done ($(stat -c %s "$out/$name.pcap") bytes pcap)"
}
for r in 800 1600; do
    run r${r}y0    --mode Color --resolution $r -l 20 -t 0  -x 20 -y 10
    run r${r}y10   --mode Color --resolution $r -l 20 -t 10 -x 20 -y 10
    run r${r}y60   --mode Color --resolution $r -l 20 -t 60 -x 20 -y 10
    run r${r}y60h30 --mode Color --resolution $r -l 20 -t 60 -x 20 -y 30
done
run r3200y30 --mode Color --resolution 3200 -l 20 -t 30 -x 10 -y 5
run r3200y0  --mode Color --resolution 3200 -l 20 -t 0  -x 10 -y 5
