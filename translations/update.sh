#!/bin/sh
# Collects the translatable strings of the shared core (src/) and the
# Sailfish app (sailfish/src, sailfish/qml) into jscan-de.ts. English is the
# source language; translate the new entries in jscan-de.ts (Qt Linguist or
# by hand) afterwards. Obsolete entries are kept so nothing gets lost.
cd "$(dirname "$0")/.." || exit 1
LUPDATE=${LUPDATE:-lupdate-qt5}
command -v "$LUPDATE" >/dev/null 2>&1 || LUPDATE=lupdate
exec "$LUPDATE" -locations relative -source-language en -target-language de \
    src sailfish/src sailfish/qml -ts translations/jscan-de.ts
