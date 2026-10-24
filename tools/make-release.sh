#!/bin/sh
# Builds the firmware and copies ready-to-flash images into firmware/.
set -e
cd "$(dirname "$0")/.."
idf.py build
mkdir -p firmware
cp build/bootloader/bootloader.bin build/partition_table/partition-table.bin build/baro_stand.bin firmware/
(cd build && python -m esptool --chip esp32 merge_bin -o ../firmware/baro_stand_full.bin @flash_args)
{
    echo "commit $(git rev-parse --short HEAD)$(git diff --quiet HEAD -- . ':!firmware' || echo ' (uncommitted changes)')"
    echo "esp-idf $(idf.py --version | sed 's/^ESP-IDF //')"
    echo "built $(date -u +%Y-%m-%dT%H:%MZ)"
} > firmware/VERSION.txt
(cd firmware && sha256sum *.bin > SHA256SUMS.txt)
cat firmware/VERSION.txt
