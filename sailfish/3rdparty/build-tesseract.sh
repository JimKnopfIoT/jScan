#!/bin/bash
# Builds a static tesseract (with static leptonica) for the Sailfish OS aarch64
# target inside the Platform SDK chroot. $1 = directory with leptonica/ and
# tesseract/ source trees; result: $1/stage/bin/tesseract
set -e
T=SailfishOS-5.2.0.17-aarch64
cd "$1"
STAGE="$PWD/stage"
# keep build paths out of the binary
MAP="-ffile-prefix-map=$PWD=/build"
sb2 -t $T -m sdk-install -R zypper --non-interactive in libjpeg-turbo-devel libpng-devel zlib-devel >/dev/null 2>&1 || :

sb2 -t $T cmake -S leptonica -B build-lept -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$MAP" -DCMAKE_CXX_FLAGS="$MAP" \
    -DCMAKE_INSTALL_PREFIX="$STAGE" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DSW_BUILD=OFF -DBUILD_PROG=OFF -DENABLE_GIF=OFF -DENABLE_TIFF=OFF -DENABLE_WEBP=OFF \
    -DENABLE_OPENJPEG=OFF -DENABLE_PNG=ON -DENABLE_JPEG=ON -DENABLE_ZLIB=ON > lept-cmake.log 2>&1
sb2 -t $T cmake --build build-lept -j8 > lept-build.log 2>&1
sb2 -t $T cmake --install build-lept > /dev/null

sb2 -t $T cmake -S tesseract -B build-tess -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS="$MAP" -DCMAKE_CXX_FLAGS="$MAP" \
    -DCMAKE_INSTALL_PREFIX="$STAGE" -DCMAKE_PREFIX_PATH="$STAGE" -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_TRAINING_TOOLS=OFF -DBUILD_TESTS=OFF \
    -DDISABLE_ARCHIVE=ON -DDISABLE_CURL=ON -DGRAPHICS_DISABLED=ON -DOPENMP_BUILD=OFF \
    -DENABLE_LTO=OFF -DSW_BUILD=OFF -DDISABLE_TIFF=ON -DINSTALL_CONFIGS=ON > tess-cmake.log 2>&1
sb2 -t $T cmake --build build-tess -j8 > tess-build.log 2>&1
sb2 -t $T cmake --install build-tess > /dev/null
sb2 -t $T strip "$STAGE/bin/tesseract"
file "$STAGE/bin/tesseract"
