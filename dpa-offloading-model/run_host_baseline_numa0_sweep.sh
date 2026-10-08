#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULT_ROOT="${SCRIPT_DIR}/results/host_baseline"
NUMA_NODE="${NUMA_NODE:-0}"
WARMUP="${WARMUP:-20}"
ITERS="${ITERS:-1000}"

COMPUTE_ITERS="16,128,256,512"
POINTER_ITERS="256,512,1024,2048"
OFFLOAD_RATES="5,10,15,20,25,30,35,40,45,50"
STDDEV_FRACTIONS="0.05,0.1,0.15,0.2,0.25,0.3"
PROCESSES=(1 2 4 8 16)

COMMON_ARGS=(
  --device mlx5_0
  --offload-rates "${OFFLOAD_RATES}"
  --overlap-delay-stddev-fraction "${STDDEV_FRACTIONS}"
  --random-overlap-delay on
  --warmup "${WARMUP}"
  --iters "${ITERS}"
  --verify
  --no-header
)

NUMA_PREFIX=(numactl "--cpunodebind=${NUMA_NODE}" "--membind=${NUMA_NODE}")
KERNEL_BIN="${SCRIPT_DIR}/build/dpa_offloading_model"
PARALLEL_KERNEL="${SCRIPT_DIR}/run_parallel_offloading_model.sh"
PARALLEL_PERSISTENT="${SCRIPT_DIR}/run_parallel_persistent.sh"

run_kernel_single() {
  local kernel="$1"
  local workload_dir="$2"
  local workload_arg="$3"
  local workload_values="$4"
  local out_dir="${RESULT_ROOT}/kernel_launch/${workload_dir}"
  mkdir -p "${out_dir}"
  {
    echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "numa_node=${NUMA_NODE}"
    printf 'cmd='
    printf '%q ' "${NUMA_PREFIX[@]}" "${KERNEL_BIN}" "${COMMON_ARGS[@]}" --kernel "${kernel}" "${workload_arg}" "${workload_values}"
    echo
  } > "${out_dir}/manifest.txt"
  "${NUMA_PREFIX[@]}" "${KERNEL_BIN}" "${COMMON_ARGS[@]}" --kernel "${kernel}" "${workload_arg}" "${workload_values}" \
    > "${out_dir}/process_1.csv" 2> "${out_dir}/process_1.stderr.log"
  echo "status=PASS" >> "${out_dir}/manifest.txt"
  echo "finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >> "${out_dir}/manifest.txt"
}

run_kernel_parallel_case() {
  local out_root="$1"
  local processes="$2"
  local kernel="$3"
  local tag="$4"
  local workload_arg="$5"
  local workload_values="$6"
  "${PARALLEL_KERNEL}" \
    --processes "${processes}" \
    --tag "${tag}" \
    --out-root "${out_root}/p${processes}" \
    --numa-node "${NUMA_NODE}" \
    --skip-build \
    -- \
    "${COMMON_ARGS[@]}" \
    --kernel "${kernel}" \
    "${workload_arg}" "${workload_values}"
}

run_persistent_parallel_case() {
  local out_root="$1"
  local processes="$2"
  local placement="$3"
  local kernel="$4"
  local tag="$5"
  local workload_arg="$6"
  local workload_values="$7"
  "${PARALLEL_PERSISTENT}" \
    --processes "${processes}" \
    --tag "${tag}" \
    --out-root "${out_root}/p${processes}" \
    --numa-node "${NUMA_NODE}" \
    --eu-placement "${placement}" \
    --skip-build \
    -- \
    "${COMMON_ARGS[@]}" \
    --kernel "${kernel}" \
    "${workload_arg}" "${workload_values}"
}

write_root_manifest() {
  local out_dir="$1"
  mkdir -p "${out_dir}"
  {
    echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "numa_node=${NUMA_NODE}"
    echo "warmup=${WARMUP}"
    echo "iters=${ITERS}"
    echo "compute_iters=${COMPUTE_ITERS}"
    echo "pointer_chasing_iters=${POINTER_ITERS}"
    echo "offload_rates=${OFFLOAD_RATES}"
    echo "stddev_fractions=${STDDEV_FRACTIONS}"
    printf 'processes='
    printf '%s ' "${PROCESSES[@]}"
    echo
  } > "${out_dir}/run_manifest.txt"
}

write_root_manifest "${RESULT_ROOT}/kernel_launch"
write_root_manifest "${RESULT_ROOT}/kernel_launch/concurrent"
write_root_manifest "${RESULT_ROOT}/kernel_launch/concurrent_10times"
write_root_manifest "${RESULT_ROOT}/persistent_thread"

echo "[INFO] Building benchmark binaries"
cmake --build "${SCRIPT_DIR}/build" -j

echo "[INFO] Running single-process kernel launch baseline"
run_kernel_single compute compute --adds-per-element "${COMPUTE_ITERS}"
run_kernel_single pointer-chasing pointer_chasing --ptr-chasing-iters "${POINTER_ITERS}"

echo "[INFO] Running kernel launch concurrent sweep"
for processes in "${PROCESSES[@]}"; do
  run_kernel_parallel_case "${RESULT_ROOT}/kernel_launch/concurrent" "${processes}" compute compute --adds-per-element "${COMPUTE_ITERS}"
  run_kernel_parallel_case "${RESULT_ROOT}/kernel_launch/concurrent" "${processes}" pointer-chasing pointer_chasing --ptr-chasing-iters "${POINTER_ITERS}"
done

echo "[INFO] Running kernel launch stability sweep"
for repeat in $(seq 1 10); do
  repeat_root="${RESULT_ROOT}/kernel_launch/concurrent_10times/concurrent_${repeat}"
  write_root_manifest "${repeat_root}"
  for processes in "${PROCESSES[@]}"; do
    run_kernel_parallel_case "${repeat_root}" "${processes}" compute compute --adds-per-element "${COMPUTE_ITERS}"
    run_kernel_parallel_case "${repeat_root}" "${processes}" pointer-chasing pointer_chasing --ptr-chasing-iters "${POINTER_ITERS}"
  done
done

echo "[INFO] Running persistent-thread concurrent sweep"
for placement_dir in concurrent_auto concurrent_samecore concurrent_spread; do
  case "${placement_dir}" in
    concurrent_auto) placement="none" ;;
    concurrent_samecore) placement="same-core" ;;
    concurrent_spread) placement="cross-core-12" ;;
    *) echo "Unknown placement directory: ${placement_dir}" >&2; exit 1 ;;
  esac
  out_root="${RESULT_ROOT}/persistent_thread/${placement_dir}"
  write_root_manifest "${out_root}"
  for processes in "${PROCESSES[@]}"; do
    run_persistent_parallel_case "${out_root}" "${processes}" "${placement}" compute compute --adds-per-element "${COMPUTE_ITERS}"
    run_persistent_parallel_case "${out_root}" "${processes}" "${placement}" pointer-chasing pointer_chasing --ptr-chasing-iters "${POINTER_ITERS}"
  done
done

echo "[INFO] All host baseline NUMA node ${NUMA_NODE} sweeps completed"
