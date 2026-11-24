#!/usr/bin/env python3
"""Turn a usbmon capture of Epson's V600 driver into a replay script for v600tool.

Steps:  W <hex>        bulk OUT
        R <n>          bulk IN, n bytes (logged)
        P <hex>        poll 1b03 until status == hex (end of a poll run)
        D <size> <cnt> cnt bulk IN reads of `size` bytes (image/calibration data)
Usage: pcap2seq.py capture.pcap devaddr start_time > scan.seq
"""
import subprocess, sys

pcap, dev, start = sys.argv[1], sys.argv[2], float(sys.argv[3])
out = subprocess.run(['tshark', '-r', pcap, '-Y', f'usb.device_address=={dev} && usb.transfer_type==0x03',
                      '-T', 'fields', '-E', 'separator=/t', '-e', 'frame.time_relative', '-e', 'usb.endpoint_address',
                      '-e', 'usb.urb_type', '-e', 'usb.urb_len', '-e', 'usb.capdata'],
                     capture_output=True, text=True, check=True).stdout
ev = []
for line in out.splitlines():
    t, ep, ut, ln, d = (line.split('\t') + [''] * 5)[:5]
    if float(t) < start:
        continue
    ut = ut.strip("'")
    ev.append((ep, ut, int(ln or 0), d.replace(':', '')))

steps = []
i = 0
while i < len(ev):
    ep, ut, ln, d = ev[i]
    if ep == '0x02' and ut == 'S':
        if d == '1b03':
            # find matching IN completion
            j = i + 1
            while j < len(ev) and not (ev[j][0] == '0x81' and ev[j][1] == 'C'):
                j += 1
            st = ev[j][3] if j < len(ev) else ''
            if steps and steps[-1][0] == 'P':
                steps[-1] = ('P', st)
            else:
                steps.append(('P', st))
            i = j + 1
            continue
        steps.append(('W', d))
    elif ep == '0x81' and ut == 'S' and ln > 1000:
        if steps and steps[-1][0] == 'D' and steps[-1][1] == ln:
            steps[-1] = ('D', ln, steps[-1][2] + 1)
        else:
            steps.append(('D', ln, 1))
    elif ep == '0x81' and ut == 'C' and ln <= 1000:
        steps.append(('R', ln))
    i += 1

for s in steps:
    print(' '.join(str(x) for x in s))
