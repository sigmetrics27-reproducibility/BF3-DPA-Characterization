#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage:
  run_parallel_offloading_model.sh [options] [-- benchmark args...]

options:
  --processes <num>  worker count (default: 2)
  --tag <tag>        result folder tag (default: UTC timestamp)
  --out-root <dir>   output root (default: results/concurrent_<UTC timestamp>)
  --numa-node <num>  bind workers to this NUMA node
  --skip-build      use the existing binary
  -h, --help        show this help

pass benchmark args after --, the device defaults to mlx5_0
without benchmark args the sweep uses
  --kernel compute
  --adds-per-element 16,64,128
  --random-overlap-delay on

examples:
  ./run_parallel_offloading_model.sh --processes 4

  ./run_parallel_offloading_model.sh --processes 4 --tag smoke -- \
    --device mlx5_0 \
    --kernel compute \
    --adds-per-element 16,64,128 \
    --random-overlap-delay on \
    --verify
EOF
}

PROCESSES=2
TAG="$(date -u '+%Y%m%d_%H%M%S')"
SKIP_BUILD=0
BENCH_ARGS=()
OUT_ROOT=""
NUMA_NODE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --processes)
      PROCESSES="${2:-}"
      shift 2
      ;;
    --tag)
      TAG="${2:-}"
      shift 2
      ;;
    --out-root)
      OUT_ROOT="${2:-}"
      shift 2
      ;;
    --numa-node)
      NUMA_NODE="${2:-}"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --)
      shift
      BENCH_ARGS=("$@")
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if ! [[ "${PROCESSES}" =~ ^[1-9][0-9]*$ ]]; then
  echo "--processes must be a positive integer" >&2
  exit 1
fi
if [[ -n "${NUMA_NODE}" ]] && ! [[ "${NUMA_NODE}" =~ ^[0-9]+$ ]]; then
  echo "--numa-node must be a non-negative integer" >&2
  exit 1
fi
if [[ -n "${NUMA_NODE}" ]] && ! command -v numactl >/dev/null 2>&1; then
  echo "numactl is required when --numa-node is provided" >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/build/dpa_offloading_model"
if [[ -z "${OUT_ROOT}" ]]; then
  OUT_ROOT="${SCRIPT_DIR}/results/concurrent_$(date -u '+%Y%m%d_%H%M%S')"
elif [[ "${OUT_ROOT}" != /* ]]; then
  OUT_ROOT="${SCRIPT_DIR}/${OUT_ROOT}"
fi
OUT_DIR="${OUT_ROOT}/${TAG}"
MANIFEST="${OUT_DIR}/manifest.txt"
RUN_LOG="${OUT_ROOT}/run_commands.log"
CMD_PREFIX=()
if [[ -n "${NUMA_NODE}" ]]; then
  CMD_PREFIX=(numactl "--cpunodebind=${NUMA_NODE}" "--membind=${NUMA_NODE}")
fi

if [[ "${#BENCH_ARGS[@]}" -eq 0 ]]; then
  BENCH_ARGS=(
    --kernel compute
    --adds-per-element 16,64,128
    --random-overlap-delay on
  )
fi

has_device_arg=0
for arg in "${BENCH_ARGS[@]}"; do
  if [[ "${arg}" == "--device" || "${arg}" == "--device-name" ]]; then
    has_device_arg=1
    break
  fi
done
if [[ "${has_device_arg}" -eq 0 ]]; then
  BENCH_ARGS=(--device mlx5_0 "${BENCH_ARGS[@]}")
fi

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
  echo "[INFO] Building dpa_offloading_model..."
  cmake --build "${SCRIPT_DIR}/build" -j
fi

mkdir -p "${OUT_DIR}"
mkdir -p "${OUT_ROOT}"
{
  echo "tag=${TAG}"
  echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "processes=${PROCESSES}"
  echo "numa_node=${NUMA_NODE:-none}"
  echo "binary=${BIN}"
  printf 'benchmark_args='
  printf '%q ' "${BENCH_ARGS[@]}"
  echo
} > "${MANIFEST}"

{
  echo "tag=${TAG}"
  echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "processes=${PROCESSES}"
  echo "numa_node=${NUMA_NODE:-none}"
  echo "output_dir=${OUT_DIR}"
  echo "skip_build=${SKIP_BUILD}"
  printf 'command='
  logged_cmd=("${BASH_SOURCE[0]}" --processes "${PROCESSES}" --tag "${TAG}" --out-root "${OUT_ROOT}")
  if [[ -n "${NUMA_NODE}" ]]; then
    logged_cmd+=(--numa-node "${NUMA_NODE}")
  fi
  if [[ "${SKIP_BUILD}" -ne 0 ]]; then
    logged_cmd+=(--skip-build)
  fi
  logged_cmd+=(-- "${BENCH_ARGS[@]}")
  printf '%q ' "${logged_cmd[@]}"
  echo
  printf 'benchmark_args='
  printf '%q ' "${BENCH_ARGS[@]}"
  echo
  echo
} >> "${RUN_LOG}"

echo "[INFO] Output directory: ${OUT_DIR}"
echo "[INFO] Launching ${PROCESSES} benchmark processes"

pids=()
for idx in $(seq 1 "${PROCESSES}"); do
  csv="${OUT_DIR}/process_${idx}.csv"
  log="${OUT_DIR}/process_${idx}.stderr.log"
  {
    echo "# process=${idx}"
    echo "# started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "# numa_node=${NUMA_NODE:-none}"
    printf '# cmd='
    printf '%q ' "${CMD_PREFIX[@]}" "${BIN}" "${BENCH_ARGS[@]}"
    echo
  } > "${log}"

  "${CMD_PREFIX[@]}" "${BIN}" "${BENCH_ARGS[@]}" > "${csv}" 2>> "${log}" &
  pid=$!
  pids+=("${pid}")
  echo "[RUN] process_${idx} pid=${pid}"
done

failed=0
for i in "${!pids[@]}"; do
  idx=$((i + 1))
  pid="${pids[$i]}"
  log="${OUT_DIR}/process_${idx}.stderr.log"
  if wait "${pid}"; then
    {
      echo "# status=PASS"
      echo "# finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >> "${log}"
    echo "[PASS] process_${idx}"
  else
    rc=$?
    {
      echo "# status=FAIL(${rc})"
      echo "# finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } >> "${log}"
    echo "[FAIL] process_${idx} rc=${rc}" >&2
    failed=1
  fi
done

{
  echo "finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "status=$([[ "${failed}" -eq 0 ]] && echo PASS || echo FAIL)"
} >> "${MANIFEST}"

if [[ "${failed}" -ne 0 ]]; then
  echo "[ERROR] One or more benchmark processes failed. See ${OUT_DIR}" >&2
  exit 1
fi

echo "[INFO] All benchmark processes completed successfully"
echo "[INFO] Results: ${OUT_DIR}"
