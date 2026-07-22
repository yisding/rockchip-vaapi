#!/bin/sh
# Assert that real H.264 and VP9 hardware frames use MPP's external pool.

set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)
WORK=$(mktemp -d "$REPO_ROOT/.test-work.zero-copy.XXXXXX") || exit 1
LOG=$WORK/driver.log

# shellcheck disable=SC2317,SC2329 # Invoked by the EXIT trap.
cleanup()
{
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

RK_VAAPI_LOG=$LOG TEST_SET=synthetic RISKY_VECTORS=skip \
    "$SCRIPT_DIR/validate.sh"

ready_count=$(awk '/external_group: ready/ { count++ } END { print count + 0 }' "$LOG")
destroyed_count=$(awk '/external_group: destroyed/ { count++ } END { print count + 0 }' "$LOG")
frame_count=$(awk '/zero_copy=1 external=1/ { count++ } END { print count + 0 }' "$LOG")
worker_started=$(awk '/decode worker: started/ { count++ } END { print count + 0 }' "$LOG")
worker_stopped=$(awk '/decode worker: stopped/ { count++ } END { print count + 0 }' "$LOG")
bad_count=$(awk '
    /mode=internal-fallback/ || /zero_copy=0/ || /copied=1/ ||
    /external buffer mismatch/ || /unsafe internal layout/ ||
    /has no pending route/ || /submission failed/ ||
    /output wait failed/ || /SyncSurface: .*draining MPP/ { count++ }
    END { print count + 0 }
' "$LOG")

if [ "$ready_count" -eq 0 ] || [ "$frame_count" -eq 0 ]; then
    echo "error: external MPP pool did not produce hardware frames" >&2
    exit 1
fi
if [ "$destroyed_count" -ne "$ready_count" ]; then
    echo "error: $ready_count external pools created but $destroyed_count destroyed" >&2
    exit 1
fi
if [ "$worker_started" -ne "$ready_count" ] ||
   [ "$worker_stopped" -ne "$worker_started" ]; then
    echo "error: worker lifecycle mismatch: started=$worker_started stopped=$worker_stopped pools=$ready_count" >&2
    exit 1
fi
if [ "$bad_count" -ne 0 ]; then
    echo "error: zero-copy fallback or ownership failure found in driver log" >&2
    exit 1
fi

echo "zero-copy/worker gate: OK ($ready_count contexts, $frame_count frames)"
