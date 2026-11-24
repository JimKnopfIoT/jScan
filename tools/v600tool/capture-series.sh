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
area="-l 20 -t 30 -x 40 -y 20"
run c400        --mode Color --resolution 400 $area
run c800        --mode Color --resolution 800 $area
run c1600       --mode Color --resolution 1600 $area
run c400off     --mode Color --resolution 400 -l 80 -t 120 -x 40 -y 20
run c400wide    --mode Color --resolution 400 -l 20 -t 30 -x 80 -y 40
run g400        --mode Gray --resolution 400 $area
run c400d16     --mode Color --resolution 400 --depth 16 $area
run c200x200    --mode Color --x-resolution 200 --y-resolution 200 $area
