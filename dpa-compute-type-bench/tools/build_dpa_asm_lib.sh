#!/usr/bin/env bash
# build the asm object and package it with device metadata and a host stub
# usage: build_dpa_asm_lib.sh <src.c|src.S> <out.a> <libname> [extra flags...]
set -euo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: $0 <src.c|src.S> <out.a> <libname> [extra dpa-clang flags...]" >&2
    exit 2
fi

SRC=$1; OUT=$2; LIBNAME=$3; shift 3
EXTRA_FLAGS=("$@")

DOCA=${DOCA_ROOT:-/opt/mellanox/doca}
DPA_CLANG=$DOCA/tools/dpa-clang
DPA_AR=$DOCA/lib/x86_64-linux-gnu/dpa_llvm/bin/llvm-ar
DPACC=$DOCA/tools/dpacc
MCPU=${DPA_MCPU:-nv-dpa-bf3}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# compile the asm with dpa-clang
"$DPA_CLANG" --target=riscv64 -mcpu="$MCPU" -O2 -ffreestanding \
    "${EXTRA_FLAGS[@]}" -c "$SRC" -o "$WORK/asm.o"

# generate metadata in the temp directory so the extra archives stay there too
printf 'int %s_dummy(void){return 0;}\n' "$LIBNAME" > "$WORK/dummy.c"
( cd "$WORK" && "$DPACC" "$WORK/dummy.c" -o "$LIBNAME" -mcpu="$MCPU" -hostcc=gcc --gen-libs \
    --devicecc-options=-O2 -I"$DOCA/include" \
    --device-libs="-L$DOCA/lib/x86_64-linux-gnu -ldoca_dpa_dev" \
    --keep --keep-dir="$WORK" >/dev/null 2>&1 ) || true
META="$WORK/$LIBNAME.lib.device.o"
[[ -f "$META" ]] || { echo "ERROR: metadata object not generated" >&2; exit 1; }

# pack the device library
mkdir -p "$(dirname "$OUT")"
"$DPA_AR" rcs "$OUT" "$META" "$WORK/asm.o"

# copy the matching host stub for registration at link time
HOST_SRC="$WORK/${LIBNAME}_host.a"
[[ -f "$HOST_SRC" ]] || { echo "ERROR: host stub archive not generated" >&2; exit 1; }
cp "$HOST_SRC" "${OUT%.a}_host.a"

echo "Built $OUT  (libname: $LIBNAME; device link: -l$LIBNAME; host link: ${OUT%.a}_host.a)"
