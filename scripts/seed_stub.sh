#!/usr/bin/env bash
# Loads i2c-stub, seeds it with register values mimicking a BME280, and
# instantiates this project's driver on it. Must run as root (or via sudo)
# on a Linux box with i2c-tools and i2c-dev/i2c-stub available.
#
# Usage: sudo scripts/seed_stub.sh
set -euo pipefail

CHIP_ADDR=0x76

echo "Loading i2c-dev and i2c-stub..."
modprobe i2c-dev
modprobe i2c-stub chip_addr="${CHIP_ADDR}"

# i2c-stub creates a new adapter; find its bus number by matching the
# adapter name i2c-stub reports via i2cdetect -l.
BUS="$(i2cdetect -l | awk -F'\t' '/[Ss][Mm][Bb]us stub driver/ {gsub("i2c-","",$1); print $1}' | head -n1)"
if [ -z "${BUS}" ]; then
	echo "Could not find the i2c-stub adapter bus number." >&2
	exit 1
fi
echo "i2c-stub adapter is bus ${BUS}"

# Register 0x00: chip id (real BME280 reports 0x60 here)
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x00 0x60

# Registers 0x01-0x04: a small stand-in calibration block. Values are
# arbitrary and only need to be non-zero and stable, to prove the probe
# path reads real seeded bytes rather than zeroed/garbage memory.
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x01 0x1a
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x02 0x2b
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x03 0x3c
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x04 0x4d

# Register 0x05: the "measurement" register the poll thread re-reads every
# interval. Change it live while the module is loaded, e.g.:
#   sudo i2cset -y ${BUS} ${CHIP_ADDR} 0x05 0x99
# to prove successive reads pick up the new value.
i2cset -y "${BUS}" "${CHIP_ADDR}" 0x05 0x50

echo "Seeded registers:"
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x00
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x01
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x02
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x03
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x04
i2cget -y "${BUS}" "${CHIP_ADDR}" 0x05
echo "Bus number for later i2cset/i2cget calls: ${BUS}"

echo "Instantiating bme280_stub client on bus ${BUS} at ${CHIP_ADDR}..."
echo "bme280_stub ${CHIP_ADDR}" > "/sys/bus/i2c/devices/i2c-${BUS}/new_device"

echo "Done. Now insmod driver/sensor_i2c.ko and check dmesg."
