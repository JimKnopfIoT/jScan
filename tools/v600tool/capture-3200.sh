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
run s3200x0   --mode Color --resolution 3200 -l 0  -t 30 -x 10 -y 3
run s3200x50  --mode Color --resolution 3200 -l 50 -t 30 -x 10 -y 3
run s3200x20w20 --mode Color --resolution 3200 -l 20 -t 30 -x 20 -y 3
run s3200y60h8 --mode Color --resolution 3200 -l 20 -t 60 -x 10 -y 8
run s6400     --mode Color --x-resolution 6400 --y-resolution 6400 -l 20 -t 30 -x 5 -y 2
