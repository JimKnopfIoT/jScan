#!/bin/bash
set -e
T=SailfishOS-5.2.0.17-aarch64
cd "$1"
sb2 -t $T -m sdk-install -R zypper --non-interactive in libusb1-devel >/dev/null 2>&1 || sb2 -t $T -m sdk-install -R zypper --non-interactive in libusb1-devel
sb2 -t $T make distclean >/dev/null 2>&1 || :
sb2 -t $T ./configure CFLAGS="-O2 -fPIC" --with-pic --host=aarch64-meego-linux-gnu --prefix=/usr --sysconfdir=/etc \
    BACKENDS="canon_dr" --enable-static --disable-shared --without-v4l --without-snmp \
    --without-avahi --without-gphoto2 --without-usb-record-replay --disable-locking \
    --enable-pthread --disable-ipv6 --without-systemd --without-poppler-glib --without-libcurl > configure.log 2>&1
sb2 -t $T make -j8 -C lib > make.log 2>&1
sb2 -t $T make -j8 -C sanei >> make.log 2>&1
sb2 -t $T make -j8 -C backend >> make.log 2>&1
sb2 -t $T make -j8 -C frontend scanimage >> make.log 2>&1
file frontend/scanimage
