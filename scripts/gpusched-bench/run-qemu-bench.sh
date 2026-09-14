#!/usr/bin/env bash
#
# Run the DRM scheduler KUnit quality suites under QEMU/KVM for each
# scheduling policy, saving one log per (policy, repetition).
#
# Usage: scripts/gpusched-bench/run-qemu-bench.sh [options]
#   -p POLICIES  space-separated policies to run (default: "1 2 3")
#                0 = RR, 1 = FIFO, 2 = FAIR, 3 = EEVDF
#   -r REPS      repetitions per policy (default: 1)
#   -f FILTER    KUnit filter glob (default: 'drm_sched_scheduler_*')
#   -s SMP       virtual CPUs given to QEMU (default: 4)
#   -c CPUS      host CPU list QEMU is pinned to (default: 0-3)
#   -t TIMEOUT   seconds allowed per run (default: 1800)
#   -o DIR       output directory (default: results/qemu-<timestamp>)
#   -b           rebuild the kernel before running
#   -h           show this help

set -Eeuo pipefail

trap 'echo "Error on line $LINENO"; exit 1' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

KUNITCONFIG="$SCRIPT_DIR/bench.kunitconfig"
BUILD_DIR=".kunit-qemu"
KERNEL_IMAGE="$BUILD_DIR/arch/x86/boot/bzImage"

POLICIES="1 2 3"
REPS=1
FILTER='drm_sched_scheduler_*'
SMP=4
CPUS="0-3"
TIMEOUT=1800
OUT_DIR=""
REBUILD=0

usage() {
	sed -n '3,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

policy_name() {
	case "$1" in
	0) echo rr ;;
	1) echo fifo ;;
	2) echo fair ;;
	3) echo eevdf ;;
	*) echo "p$1" ;;
	esac
}

while getopts "p:r:f:s:c:t:o:bh" opt; do
	case "$opt" in
	p) POLICIES="$OPTARG" ;;
	r) REPS="$OPTARG" ;;
	f) FILTER="$OPTARG" ;;
	s) SMP="$OPTARG" ;;
	c) CPUS="$OPTARG" ;;
	t) TIMEOUT="$OPTARG" ;;
	o) OUT_DIR="$OPTARG" ;;
	b) REBUILD=1 ;;
	h)
		usage
		exit 0
		;;
	*)
		usage >&2
		exit 2
		;;
	esac
done

for p in $POLICIES; do
	if [[ ! "$p" =~ ^[0-3]$ ]]; then
		echo "Invalid policy '$p': expected 0 (RR), 1 (FIFO), 2 (FAIR) or 3 (EEVDF)." >&2
		exit 2
	fi
done

if [[ ! "$REPS" =~ ^[1-9][0-9]*$ ]]; then
	echo "Invalid repetition count '$REPS': expected a positive integer." >&2
	exit 2
fi

cd "$SRC_DIR"

if [[ -z "$OUT_DIR" ]]; then
	OUT_DIR="results/qemu-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$OUT_DIR"

if ((REBUILD)) || [[ ! -f "$KERNEL_IMAGE" ]]; then
	./tools/testing/kunit/kunit.py build \
		--arch=x86_64 \
		--build_dir="$BUILD_DIR" \
		--kunitconfig="$KUNITCONFIG" \
		--jobs="$(nproc)"
fi

# Record exactly what produced these results.
{
	echo "date:        $(date -Is)"
	echo "commit:      $(git rev-parse HEAD)$(git diff --quiet HEAD -- || echo ' (dirty, see source.diff)')"
	echo "kernel:      $KERNEL_IMAGE built $(stat -c %y "$KERNEL_IMAGE")"
	echo "kunitconfig: $KUNITCONFIG"
	echo "policies:    $POLICIES"
	echo "reps:        $REPS"
	echo "filter:      $FILTER"
	echo "qemu smp:    $SMP (pinned to host CPUs $CPUS)"
	echo "timeout:     ${TIMEOUT}s"
	echo "host:        $(uname -r), $(nproc) CPUs, governor $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
	echo "qemu:        $(qemu-system-x86_64 --version | head -n1)"
} >"$OUT_DIR/meta.txt"
git diff HEAD -- >"$OUT_DIR/source.diff"

problems=0

for rep in $(seq 1 "$REPS"); do
	for p in $POLICIES; do
		name="$(policy_name "$p")"
		log="$OUT_DIR/$name-r$rep.log"

		echo "==> Policy $p ($name), run $rep of $REPS: $log"

		status=0
		taskset -c "$CPUS" ./tools/testing/kunit/kunit.py exec \
			--arch=x86_64 \
			--build_dir="$BUILD_DIR" \
			--qemu_args="-smp $SMP" \
			--kernel_args="gpu_sched.sched_policy=$p" \
			--timeout="$TIMEOUT" \
			--raw_output=all \
			"$FILTER" | tee "$log" || status=$?

		if ! grep -q "Kernel command line:.*gpu_sched\.sched_policy=$p" "$log"; then
			echo "WARNING: $log: policy $p not found on the kernel command line." >&2
			problems=$((problems + 1))
		fi

		if grep -qE '^(\[ *[0-9]+\.[0-9]+\] )?not ok [0-9]+ ' "$log"; then
			echo "WARNING: $log: failed suites:" >&2
			grep -E '^(\[ *[0-9]+\.[0-9]+\] )?not ok [0-9]+ ' "$log" >&2
			problems=$((problems + 1))
		fi

		if ((status != 0)); then
			echo "WARNING: $log: kunit.py exited with status $status." >&2
			problems=$((problems + 1))
		fi
	done
done

echo "Finished $REPS run(s) of policies [$POLICIES] with $problems warning(s). Results in $OUT_DIR"

if ((problems)); then
	exit 1
fi
