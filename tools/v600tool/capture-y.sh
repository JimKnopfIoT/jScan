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
for y in 0 2 4 6 8 10; do
    run y$y --mode Color --resolution 400 -l 20 -t $y -x 40 -y 15
done
