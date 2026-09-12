#!/usr/bin/env bash

set -Eeuo pipefail

trap 'echo "Error on line $LINENO"; exit 1' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

VMLINUX_SRC="$SCRIPT_DIR/../"
BUILD_DIR="$SCRIPT_DIR/../build/"
LOCALVERSION="-eevdf-build"

cd "$VMLINUX_SRC"
make mrproper
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

zcat /proc/config.gz >.config

make -C "$VMLINUX_SRC" O="$BUILD_DIR" olddefconfig

"$VMLINUX_SRC/scripts/config" --file .config \
	--set-str LOCALVERSION "$LOCALVERSION" \
	--enable FTRACE \
	--enable FUNCTION_TRACER \
	--enable PRINTK \
	--enable DRM \
	--enable DRM_SCHED \
	--enable DRM_DEBUG \
	--disable MACB \
	--disable MACB_PCI

make -C "$VMLINUX_SRC" O="$BUILD_DIR" -j"$(nproc)" | tee build.log

sudo make -C "$VMLINUX_SRC" O="$BUILD_DIR" modules_install
sudo make -C "$VMLINUX_SRC" O="$BUILD_DIR" install

CMDLINE_EXTRA="gpu_sched.sched_policy=3"

BOOT_ENTRY=$(ls -t /boot/loader/entries/*eevdf-build* | head -n1)
if [[ -f "$BOOT_ENTRY" ]]; then
	sudo sed -i 's/^title.*/title   EEVDF BUILD/' "$BOOT_ENTRY"

	# Append the GPU scheduler policy to the kernel cmdline, replacing any
	# previous gpu_sched.sched_policy= value so reruns stay idempotent.
	sudo sed -i -E \
		-e "s/[[:space:]]*gpu_sched\\.sched_policy=[^[:space:]]*//g" \
		-e "s|^(options[[:space:]].*)$|\\1 $CMDLINE_EXTRA|" \
		"$BOOT_ENTRY"
fi

echo "Built Kernel Successfully."
