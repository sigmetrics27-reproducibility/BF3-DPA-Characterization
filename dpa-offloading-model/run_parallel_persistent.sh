#!/usr/bin/env bash
# run several benchmark processes with optional EU and NUMA placement
# --binary persistent|kernel picks the entry point, default is persistent
# --eu-placement none|same-eu|same-core|cross-core|cross-core-12
# placement applies to persistent workers, cross-core uses a stride of 16 EUs
# cross-core-12 fills 12 cores first, then uses the next EU on those cores
# --eu-base sets the first EU, --numa-node binds the host processes
# --processes sets the worker count, pass benchmark args after --
set -euo pipefail

PROCESSES=2
TAG=""
SKIP_BUILD=0
BINARY="persistent"
EU_PLACEMENT="none"
EU_BASE=0
OUT_ROOT=""
NUMA_NODE=""
BENCH_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary)        BINARY="${2:?}"; shift 2 ;;
    --processes)     PROCESSES="${2:?}"; shift 2 ;;
    --eu-placement)  EU_PLACEMENT="${2:?}"; shift 2 ;;
    --eu-base)       EU_BASE="${2:?}"; shift 2 ;;
    --tag)           TAG="${2:?}"; shift 2 ;;
    --out-root)      OUT_ROOT="${2:?}"; shift 2 ;;
    --numa-node)     NUMA_NODE="${2:?}"; shift 2 ;;
    --skip-build)    SKIP_BUILD=1; shift ;;
    --)              shift; BENCH_ARGS=("$@"); break ;;
    -h|--help)
      sed -n '2,/^[^#]/{ /^#/s/^# *//p; }' "$0"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
  esac
done

[[ "${PROCESSES}" =~ ^[1-9][0-9]*$ ]] || { echo "--processes must be a positive integer" >&2; exit 1; }
[[ "${BINARY}" == "persistent" || "${BINARY}" == "kernel" ]] || { echo "--binary must be persistent|kernel" >&2; exit 1; }
[[ "${EU_PLACEMENT}" =~ ^(none|same-eu|same-core|cross-core|cross-core-12)$ ]] || { echo "bad --eu-placement" >&2; exit 1; }
[[ -z "${NUMA_NODE}" || "${NUMA_NODE}" =~ ^[0-9]+$ ]] || { echo "--numa-node must be a non-negative integer" >&2; exit 1; }
[[ -z "${NUMA_NODE}" || -x "$(command -v numactl)" ]] || { echo "numactl is required when --numa-node is provided" >&2; exit 1; }

[[ -z "${TAG}" ]] && TAG="$(date -u '+%Y%m%d_%H%M%S')"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "${BINARY}" == "persistent" ]]; then
  BIN="${SCRIPT_DIR}/build/dpa_offloading_model_persistent"
else
  BIN="${SCRIPT_DIR}/build/dpa_offloading_model"
  # kernel mode has no affinity args
  EU_PLACEMENT="none"
fi

[[ -z "${OUT_ROOT}" ]] && OUT_ROOT="${SCRIPT_DIR}/results/persistent_test/concurrent"
[[ "${OUT_ROOT}" != /* ]] && OUT_ROOT="${SCRIPT_DIR}/${OUT_ROOT}"
OUT_DIR="${OUT_ROOT}/${TAG}"
mkdir -p "${OUT_DIR}"
CMD_PREFIX=()
if [[ -n "${NUMA_NODE}" ]]; then
  CMD_PREFIX=(numactl "--cpunodebind=${NUMA_NODE}" "--membind=${NUMA_NODE}")
fi

if [[ "${#BENCH_ARGS[@]}" -eq 0 ]]; then
  BENCH_ARGS=(--device mlx5_0 --kernel compute --adds-per-element 16 --offload-rates 50 --iters 100 --warmup 30 --no-header)
fi

# add a device if none was given
has_dev=0
for a in "${BENCH_ARGS[@]}"; do [[ "$a" == "--device" || "$a" == "--device-name" ]] && has_dev=1; done
[[ ${has_dev} -eq 0 ]] && BENCH_ARGS=(--device mlx5_0 "${BENCH_ARGS[@]}")

if [[ ${SKIP_BUILD} -eq 0 ]]; then
  echo "[INFO] Building..."
  cmake --build "${SCRIPT_DIR}/build" -j >/dev/null
fi

eu_args_for() {  # $1 = 1-based process index
  local idx="$1"
  if [[ "${BINARY}" != "persistent" || "${EU_PLACEMENT}" == "none" ]]; then
    echo ""
  elif [[ "${EU_PLACEMENT}" == "same-eu" ]]; then
    echo "--affinity on --eu-id ${EU_BASE}"
  elif [[ "${EU_PLACEMENT}" == "same-core" ]]; then
    echo "--affinity on --eu-id $(( EU_BASE + idx - 1 ))"
  elif [[ "${EU_PLACEMENT}" == "cross-core-12" ]]; then
    # fill one EU per core first, then use the next EU on each core
    if [[ ${idx} -le 12 ]]; then
      echo "--affinity on --eu-id $(( EU_BASE + (idx - 1) * 16 ))"
    else
      echo "--affinity on --eu-id $(( EU_BASE + (idx - 13) * 16 + 1 ))"
    fi
  else  # cross-core
    echo "--affinity on --eu-id $(( EU_BASE + (idx - 1) * 16 ))"
  fi
}

MANIFEST="${OUT_DIR}/manifest.txt"
{
  echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "binary=${BINARY} (${BIN})"
  echo "processes=${PROCESSES}"
  echo "eu_placement=${EU_PLACEMENT}"
  echo "eu_base=${EU_BASE}"
  echo "numa_node=${NUMA_NODE:-none}"
  printf 'bench_args='; printf '%q ' "${BENCH_ARGS[@]}"; echo
} > "${MANIFEST}"
echo "[INFO] ${OUT_DIR}: binary=${BINARY} procs=${PROCESSES} placement=${EU_PLACEMENT} base=${EU_BASE} numa=${NUMA_NODE:-none}"

pids=()
for idx in $(seq 1 "${PROCESSES}"); do
  csv="${OUT_DIR}/process_${idx}.csv"
  log="${OUT_DIR}/process_${idx}.stderr.log"
  eu_args="$(eu_args_for "${idx}")"
  # shellcheck disable=SC2086
  run_args=( ${BENCH_ARGS[@]} ${eu_args} )
  {
    echo "# process=${idx} eu_args=${eu_args}"
    echo "# started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "# numa_node=${NUMA_NODE:-none}"
    printf '# cmd='; printf '%q ' "${CMD_PREFIX[@]}" "${BIN}" "${run_args[@]}"; echo
  } > "${log}"
  echo "[RUN] process_${idx} eu_args='${eu_args}'"
  "${CMD_PREFIX[@]}" "${BIN}" "${run_args[@]}" > "${csv}" 2>> "${log}" &
  pids+=($!)
done

failed=0
for i in "${!pids[@]}"; do
  idx=$((i + 1))
  if wait "${pids[$i]}"; then
    echo "# status=PASS finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "${OUT_DIR}/process_${idx}.stderr.log"
    echo "[PASS] process_${idx}"
  else
    rc=$?
    echo "# status=FAIL(${rc}) finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "${OUT_DIR}/process_${idx}.stderr.log"
    echo "[FAIL] process_${idx} rc=${rc}" >&2
    failed=1
  fi
done
echo "status=$([[ ${failed} -eq 0 ]] && echo PASS || echo FAIL)" >> "${MANIFEST}"
[[ ${failed} -eq 0 ]] && echo "[INFO] All ${PROCESSES} done -> ${OUT_DIR}"
exit ${failed}
