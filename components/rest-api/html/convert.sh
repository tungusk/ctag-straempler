#!/bin/bash
# index.html -> include/index.html.h (the page is served from FLASH, not the card).
#
# THIS IS NOT AUTOMATIC. CMake does not run it — edit index.html, forget this, and your
# change silently does not ship. Always run it and check that the .h actually moved.
#
# `sed -i -e` on BSD/macOS treats the -e as the backup SUFFIX, which is why a stale
# index.html.h-e kept appearing next to the real header. Do it in one pass instead.
set -e
cd "$(dirname "$0")"
xxd -i -a index.html > ../include/index.html.h
sed -i '' \
    -e 's/unsigned char/const char/g' \
    -e 's/unsigned int/const unsigned int/g' \
    ../include/index.html.h
rm -f ../include/index.html.h-e          # kill the old BSD-sed artefact if it exists
echo "index.html.h regenerated ($(wc -c < ../include/index.html.h) bytes)"
