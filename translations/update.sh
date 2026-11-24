#!/bin/sh
# Collects the translatable strings of jScan (src/) into jscan-de.ts. English
# is the source language; translate the new entries in jscan-de.ts (Qt
# Linguist or by hand) afterwards. The Sailfish OS app harbour-jscan has its
# own copy of the core and of the translations.
cd "$(dirname "$0")/.." || exit 1
LUPDATE=${LUPDATE:-lupdate-qt5}
command -v "$LUPDATE" >/dev/null 2>&1 || LUPDATE=lupdate
exec "$LUPDATE" -locations relative -source-language en -target-language de -no-obsolete \
    src -ts translations/jscan-de.ts
