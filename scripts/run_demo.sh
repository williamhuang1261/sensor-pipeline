#!/usr/bin/env bash
# End-to-end demo: builds the driver and daemon, loads i2c-stub, seeds it
# with fake BME280 register values, insmod's the driver, runs the daemon
# against the real /dev/sensor0 device, then tears everything down cleanly.
#
# Must run on a real Linux box with kernel headers, build-essential and
# i2c-tools installed (this project was built and verified inside a Lima
# Ubuntu VM -- see the README's "Hardware re-scoping" section for why).
#
# Usage: sudo scripts/run_demo.sh [sample_count]
set -euo pipefail

cd "$(dirname "$0")/.."

SAMPLE_COUNT="${1:-8}"

echo "=== Building driver ==="
make -C driver

echo
echo "=== Building daemon ==="
make -C daemon

echo
echo "=== Seeding i2c-stub ==="
bash scripts/seed_stub.sh

echo
echo "=== Loading sensor_i2c.ko ==="
insmod driver/sensor_i2c.ko
chmod 666 /dev/sensor0

echo
echo "=== dmesg (probe output) ==="
dmesg | tail -6

echo
echo "=== Running sensord ==="
./daemon/sensord /dev/sensor0 "${SAMPLE_COUNT}"

echo
echo "=== Tearing down ==="
rmmod sensor_i2c
modprobe -r i2c_stub
echo "Clean unload complete."
