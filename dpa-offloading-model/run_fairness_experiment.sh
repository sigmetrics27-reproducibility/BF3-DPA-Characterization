#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${SCRIPT_DIR}/build/dpa_offloading_model_persistent"

A_WORKLOAD=""
B_WORKLOAD=""
A_WORK_VALUES=""
B_WORK_VALUES=""
A_PROCESSES=8
B_PROCESSES=8
A_ITERS=2000
B_ITERS=500
WARMUP=50
OFFLOAD_RATES="10,20,30,40,50,60,70"
STDDEV_FRACTION="0.1"
NUMA_NODE=0
READY_TIMEOUT_SECONDS=300
OUT_ROOT=""
FINAL_OUT_ROOT=""
STAGING_ROOT=""
SYMMETRIC=0
EXTENDED_SYMMETRIC=0
SKIP_BUILD=0
DRY_RUN=0

usage() {
  cat <<'EOF'
usage:
  ./run_fairness_experiment.sh --a-workload <compute|pointer-chasing> \
    --b-workload <compute|pointer-chasing> [options]

measure B alone, then with A running in the background
--symmetric measures both baselines and both groups running together
both modes try auto, same-core and different-core placement

required:
  --a-workload <name>       background workload A
  --b-workload <name>       measured workload B

workload options:
  --a-work-values <csv>     A ADD/elem or chase counts
                            defaults: compute=1024, pointer-chasing=4096
  --b-work-values <csv>     B ADD/elem or chase counts
                            defaults: compute=256, pointer-chasing=1024
  --a-processes <n>         A process count (default: 8)
  --b-processes <n>         B process count (default: 8)
  --a-iters <n>             A measured iterations per phase (default: 2000)
  --b-iters <n>             B measured iterations per phase (default: 500)
  --warmup <n>              warmup iterations for both groups (default: 50)

model options:
  --offload-rates <csv>     offload rates (default: 10,20,30,40,50,60,70)
  --stddev-fraction <value> gaussian overlap-delay ratio (default: 0.1)

run options:
  --symmetric               measure A/B baselines and both groups together
  --extended-symmetric      also run two homogeneous 8+8 cases
  --numa-node <n>           bind host processes and memory (default: 0)
  --out-root <dir>          output folder, must be empty or absent
  --ready-timeout <sec>     wait for A to enter DPA phases (default: 300)
  --skip-build              use the existing binary
  --dry-run                 check and print the config without running
  -h, --help                show this help

example:
  ./run_fairness_experiment.sh \
    --a-workload compute \
    --b-workload pointer-chasing \
    --a-work-values 1024 \
    --b-work-values 1024 \
    --a-processes 8 \
    --b-processes 8 \
    --symmetric
EOF
}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

require_value() {
  [[ $# -ge 2 ]] || die "Missing value for $1"
}

normalize_workload() {
  case "$1" in
    compute) echo "compute" ;;
    pointer|pointer-chasing|pointer_chasing|pchasing) echo "pointer-chasing" ;;
    *) die "Unsupported workload '$1'; use compute or pointer-chasing" ;;
  esac
}

workload_arg() {
  case "$1" in
    compute) echo "--adds-per-element" ;;
    pointer-chasing) echo "--ptr-chasing-iters" ;;
    *) die "Unsupported workload '$1'" ;;
  esac
}

workload_tag() {
  echo "${1//-/_}"
}

validate_positive_integer() {
  [[ "$2" =~ ^[1-9][0-9]*$ ]] || die "$1 must be a positive integer"
}

validate_nonnegative_integer() {
  [[ "$2" =~ ^[0-9]+$ ]] || die "$1 must be a non-negative integer"
}

validate_positive_csv() {
  local name="$1"
  local csv="$2"
  local values=()
  IFS=',' read -r -a values <<< "${csv}"
  [[ ${#values[@]} -gt 0 ]] || die "${name} must not be empty"
  local value
  for value in "${values[@]}"; do
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || die "${name} contains invalid value '${value}'"
  done
}

validate_offload_rates() {
  validate_positive_csv "--offload-rates" "$1"
  local values=()
  IFS=',' read -r -a values <<< "$1"
  local value
  for value in "${values[@]}"; do
    (( value <= 100 )) || die "--offload-rates entries must be <= 100"
  done
}

csv_count() {
  local values=()
  IFS=',' read -r -a values <<< "$1"
  echo "${#values[@]}"
}

experiment_type() {
  if (( EXTENDED_SYMMETRIC )); then
    echo "extended-symmetric"
  elif (( SYMMETRIC )); then
    echo "symmetric"
  else
    echo "directional"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --a-workload) require_value "$@"; A_WORKLOAD="$2"; shift 2 ;;
    --b-workload) require_value "$@"; B_WORKLOAD="$2"; shift 2 ;;
    --a-work-values) require_value "$@"; A_WORK_VALUES="$2"; shift 2 ;;
    --b-work-values) require_value "$@"; B_WORK_VALUES="$2"; shift 2 ;;
    --a-processes) require_value "$@"; A_PROCESSES="$2"; shift 2 ;;
    --b-processes) require_value "$@"; B_PROCESSES="$2"; shift 2 ;;
    --a-iters) require_value "$@"; A_ITERS="$2"; shift 2 ;;
    --b-iters) require_value "$@"; B_ITERS="$2"; shift 2 ;;
    --warmup) require_value "$@"; WARMUP="$2"; shift 2 ;;
    --offload-rates) require_value "$@"; OFFLOAD_RATES="$2"; shift 2 ;;
    --stddev-fraction) require_value "$@"; STDDEV_FRACTION="$2"; shift 2 ;;
    --numa-node) require_value "$@"; NUMA_NODE="$2"; shift 2 ;;
    --out-root) require_value "$@"; OUT_ROOT="$2"; shift 2 ;;
    --ready-timeout) require_value "$@"; READY_TIMEOUT_SECONDS="$2"; shift 2 ;;
    --symmetric) SYMMETRIC=1; shift ;;
    --extended-symmetric) SYMMETRIC=1; EXTENDED_SYMMETRIC=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown argument: $1" ;;
  esac
done

[[ -n "${A_WORKLOAD}" ]] || die "--a-workload is required"
[[ -n "${B_WORKLOAD}" ]] || die "--b-workload is required"
A_WORKLOAD="$(normalize_workload "${A_WORKLOAD}")"
B_WORKLOAD="$(normalize_workload "${B_WORKLOAD}")"

if [[ -z "${A_WORK_VALUES}" ]]; then
  [[ "${A_WORKLOAD}" == "compute" ]] && A_WORK_VALUES="1024" || A_WORK_VALUES="4096"
fi
if [[ -z "${B_WORK_VALUES}" ]]; then
  [[ "${B_WORKLOAD}" == "compute" ]] && B_WORK_VALUES="256" || B_WORK_VALUES="1024"
fi

validate_positive_csv "--a-work-values" "${A_WORK_VALUES}"
validate_positive_csv "--b-work-values" "${B_WORK_VALUES}"
validate_offload_rates "${OFFLOAD_RATES}"
validate_positive_integer "--a-processes" "${A_PROCESSES}"
validate_positive_integer "--b-processes" "${B_PROCESSES}"
validate_positive_integer "--a-iters" "${A_ITERS}"
validate_positive_integer "--b-iters" "${B_ITERS}"
validate_nonnegative_integer "--warmup" "${WARMUP}"
validate_nonnegative_integer "--numa-node" "${NUMA_NODE}"
validate_positive_integer "--ready-timeout" "${READY_TIMEOUT_SECONDS}"
[[ "${STDDEV_FRACTION}" =~ ^[0-9]+([.][0-9]+)?$ ]] || die "--stddev-fraction must be non-negative"

(( A_PROCESSES <= 16 )) || die "--a-processes must be <= 16 for the DPA core mappings"
(( B_PROCESSES <= 16 )) || die "--b-processes must be <= 16 for the DPA core mappings"
(( A_PROCESSES + B_PROCESSES <= 16 )) || \
  die "same-core mode requires A processes + B processes <= 16"
if (( EXTENDED_SYMMETRIC )); then
  [[ "${A_WORKLOAD}" != "${B_WORKLOAD}" ]] || \
    die "extended symmetric mode requires two distinct workloads"
  (( 2 * A_PROCESSES <= 16 )) || die "extended symmetric mode requires 2 * A processes <= 16"
  (( 2 * B_PROCESSES <= 16 )) || die "extended symmetric mode requires 2 * B processes <= 16"
fi

A_PHASES=$(( $(csv_count "${A_WORK_VALUES}") * $(csv_count "${OFFLOAD_RATES}") ))
if (( ! SYMMETRIC )); then
  (( A_PHASES >= 2 )) || die "A must have at least two measurement phases so B can overlap it"
fi

A_TAG="$(workload_tag "${A_WORKLOAD}")"
B_TAG="$(workload_tag "${B_WORKLOAD}")"
if [[ -z "${OUT_ROOT}" ]]; then
  TIMESTAMP="$(date -u '+%Y%m%d_%H%M%S')"
  if (( EXTENDED_SYMMETRIC )); then
    OUT_ROOT="${SCRIPT_DIR}/results/host_baseline/fairness/extended_symmetric/${A_TAG}_with_${B_TAG}/${TIMESTAMP}"
  elif (( SYMMETRIC )); then
    OUT_ROOT="${SCRIPT_DIR}/results/host_baseline/fairness/symmetric/${A_TAG}_with_${B_TAG}/${TIMESTAMP}"
  else
    OUT_ROOT="${SCRIPT_DIR}/results/host_baseline/fairness/${A_TAG}_on_${B_TAG}/${TIMESTAMP}"
  fi
fi
OUT_ROOT="$(realpath -m "${OUT_ROOT}")"

if [[ -d "${OUT_ROOT}" ]] && find "${OUT_ROOT}" -mindepth 1 -print -quit | grep -q .; then
  die "Output directory is not empty: ${OUT_ROOT}"
fi
FINAL_OUT_ROOT="${OUT_ROOT}"

print_config() {
  cat <<EOF
Fairness experiment:
  type=$(experiment_type)
  A: ${A_WORKLOAD}, values=${A_WORK_VALUES}, processes=${A_PROCESSES}, iters=${A_ITERS}
  B: ${B_WORKLOAD}, values=${B_WORK_VALUES}, processes=${B_PROCESSES}, iters=${B_ITERS}
  warmup=${WARMUP}, rates=${OFFLOAD_RATES}, stddev=${STDDEV_FRACTION}
  NUMA node=${NUMA_NODE}
  output=${OUT_ROOT}
  modes=auto,samecore,different_core
EOF
}

print_config
if (( DRY_RUN )); then
  exit 0
fi

command -v numactl >/dev/null || die "numactl is required"
command -v stdbuf >/dev/null || die "stdbuf is required"
[[ -x "${BIN}" || ${SKIP_BUILD} -eq 0 ]] || die "Missing benchmark binary: ${BIN}"

if (( ! SKIP_BUILD )); then
  cmake --build "${SCRIPT_DIR}/build" -j
fi
[[ -x "${BIN}" ]] || die "Missing benchmark binary after build: ${BIN}"

mkdir -p "$(dirname "${FINAL_OUT_ROOT}")"
if [[ "$(stat -f -c '%T' "$(dirname "${FINAL_OUT_ROOT}")")" == nfs* ]]; then
  STAGING_ROOT="$(mktemp -d /tmp/dpa_fairness.XXXXXX)"
  OUT_ROOT="${STAGING_ROOT}"
  echo "[STAGING] Writing locally before copying to NFS: ${STAGING_ROOT}"
else
  mkdir -p "${OUT_ROOT}"
fi

COMMON_ARGS=(
  --device mlx5_0
  --offload-rates "${OFFLOAD_RATES}"
  --overlap-delay-stddev-fraction "${STDDEV_FRACTION}"
  --random-overlap-delay on
  --warmup "${WARMUP}"
  --verify
  --no-header
)

A_RUNNER_PID=""
B_RUNNER_PID=""
cleanup_runner() {
  local runner_pid="$1"
  if [[ -n "${runner_pid}" ]] && kill -0 "${runner_pid}" 2>/dev/null; then
    pkill -TERM -P "${runner_pid}" 2>/dev/null || true
    kill -TERM "${runner_pid}" 2>/dev/null || true
    wait "${runner_pid}" 2>/dev/null || true
  fi
}

cleanup_background_groups() {
  cleanup_runner "${A_RUNNER_PID}"
  cleanup_runner "${B_RUNNER_PID}"
}
trap cleanup_background_groups EXIT INT TERM

affinity_args() {
  local placement="$1"
  local eu_base="$2"
  local process_index="$3"
  if [[ "${placement}" == "none" ]]; then
    echo "--affinity off"
  else
    echo "--affinity on --eu-id $((eu_base + process_index - 1))"
  fi
}

run_group() {
  local out_dir="$1"
  local workload="$2"
  local work_values="$3"
  local processes="$4"
  local placement="$5"
  local eu_base="$6"
  local iters="$7"
  local role="$8"
  local work_arg
  work_arg="$(workload_arg "${workload}")"

  mkdir -p "${out_dir}"
  local pids=()
  local index
  for ((index = 1; index <= processes; ++index)); do
    local affinity=()
    read -r -a affinity <<< "$(affinity_args "${placement}" "${eu_base}" "${index}")"
    local command=(
      stdbuf -oL -eL
      numactl "--cpunodebind=${NUMA_NODE}" "--membind=${NUMA_NODE}"
      "${BIN}"
      "${COMMON_ARGS[@]}"
      --kernel "${workload}"
      "${work_arg}" "${work_values}"
      --iters "${iters}"
      "${affinity[@]}"
    )
    printf '%q ' "${command[@]}" > "${out_dir}/process_${index}.command.txt"
    printf '\n' >> "${out_dir}/process_${index}.command.txt"
    "${command[@]}" > "${out_dir}/process_${index}.csv" \
      2> "${out_dir}/process_${index}.stderr.log" &
    local process_pid="$!"
    pids+=("${process_pid}")
    echo "[RUN] ${role} process_${index} pid=${process_pid}"
  done

  local failed=0
  for ((index = 1; index <= processes; ++index)); do
    if wait "${pids[index - 1]}"; then
      echo "[PASS] ${role} process_${index}"
    else
      echo "[FAIL] ${role} process_${index}" >&2
      failed=1
    fi
  done

  local expected_rows=$(( $(csv_count "${work_values}") * $(csv_count "${OFFLOAD_RATES}") ))
  for ((index = 1; index <= processes; ++index)); do
    local csv_path="${out_dir}/process_${index}.csv"
    local stderr_path="${out_dir}/process_${index}.stderr.log"
    if [[ ! -f "${csv_path}" || ! -f "${stderr_path}" ]] ||
       ! awk -F, -v expected="${expected_rows}" '
           /^[0-9]+,/ {
             rows++
             if (NF != 34 || $NF != "ok") bad = 1
           }
           END { exit !(rows == expected && bad == 0) }
         ' "${csv_path}"; then
      echo "[FAIL] Invalid output for ${role} process_${index}" >&2
      failed=1
    fi
  done
  return "${failed}"
}

wait_for_a_dpa_phase() {
  local out_dir="$1"
  local processes="$2"
  local runner_pid="$3"
  local start_seconds="${SECONDS}"

  while true; do
    local ready=1
    local index
    for ((index = 1; index <= processes; ++index)); do
      local csv_path="${out_dir}/process_${index}.csv"
      if [[ ! -f "${csv_path}" ]] || ! grep -qE '^[0-9]+,' "${csv_path}"; then
        ready=0
        break
      fi
    done
    if (( ready )); then
      return 0
    fi
    if ! kill -0 "${runner_pid}" 2>/dev/null; then
      wait "${runner_pid}" || true
      die "A group exited before reaching its first DPA measurement phase"
    fi
    if (( SECONDS - start_seconds >= READY_TIMEOUT_SECONDS )); then
      die "Timed out waiting for A to enter its DPA measurement phases"
    fi
    sleep 0.1
  done
}

write_mode_mapping() {
  local mode_dir="$1"
  local a_placement="$2"
  local a_base="$3"
  local b_placement="$4"
  local b_base="$5"
  {
    echo -e "role\tprocess\taffinity\teu_id"
    local role process placement base count
    for role in A B; do
      if [[ "${role}" == "A" ]]; then
        placement="${a_placement}"; base="${a_base}"; count="${A_PROCESSES}"
      else
        placement="${b_placement}"; base="${b_base}"; count="${B_PROCESSES}"
      fi
      for ((process = 1; process <= count; ++process)); do
        if [[ "${placement}" == "none" ]]; then
          echo -e "${role}\t${process}\toff\tauto"
        else
          echo -e "${role}\t${process}\ton\t$((base + process - 1))"
        fi
      done
    done
  } > "${mode_dir}/eu_mapping.tsv"
}

run_directional_mode() {
  local mode="$1"
  local a_placement="$2"
  local a_base="$3"
  local b_placement="$4"
  local b_base="$5"
  local mode_dir="${OUT_ROOT}/${mode}"
  local baseline_dir="${mode_dir}/baseline_b"
  local a_dir="${mode_dir}/corun/a"
  local b_dir="${mode_dir}/corun/b"

  mkdir -p "${mode_dir}/corun"
  write_mode_mapping "${mode_dir}" "${a_placement}" "${a_base}" \
    "${b_placement}" "${b_base}"

  echo "[BASELINE] mode=${mode}"
  run_group "${baseline_dir}" "${B_WORKLOAD}" "${B_WORK_VALUES}" "${B_PROCESSES}" \
    "${b_placement}" "${b_base}" "${B_ITERS}" "baseline-B"

  echo "[CORUN] mode=${mode}: starting A"
  run_group "${a_dir}" "${A_WORKLOAD}" "${A_WORK_VALUES}" "${A_PROCESSES}" \
    "${a_placement}" "${a_base}" "${A_ITERS}" "corun-A" \
    > "${mode_dir}/a_runner.log" 2>&1 &
  A_RUNNER_PID="$!"

  wait_for_a_dpa_phase "${a_dir}" "${A_PROCESSES}" "${A_RUNNER_PID}"
  local a_ready_utc
  a_ready_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "[CORUN] mode=${mode}: A is active, starting B"
  local b_start_utc
  b_start_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  run_group "${b_dir}" "${B_WORKLOAD}" "${B_WORK_VALUES}" "${B_PROCESSES}" \
    "${b_placement}" "${b_base}" "${B_ITERS}" "corun-B"
  local b_finish_utc
  b_finish_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

  if ! kill -0 "${A_RUNNER_PID}" 2>/dev/null; then
    wait "${A_RUNNER_PID}" || true
    die "Invalid ${mode} run: A finished before B"
  fi
  echo "[CORUN] mode=${mode}: B finished while A is still active"
  if ! wait "${A_RUNNER_PID}"; then
    A_RUNNER_PID=""
    die "A group failed in ${mode} mode"
  fi
  A_RUNNER_PID=""

  {
    echo "mode=${mode}"
    echo "a_ready_utc=${a_ready_utc}"
    echo "b_start_utc=${b_start_utc}"
    echo "b_finish_utc=${b_finish_utc}"
    echo "a_alive_at_b_finish=1"
    echo "status=PASS"
  } > "${mode_dir}/coverage.txt"
}

run_homogeneous_corun() {
  local mode_dir="$1"
  local workload_tag="$2"
  local workload="$3"
  local work_values="$4"
  local processes="$5"
  local iters="$6"
  local group_1_placement="$7"
  local group_1_base="$8"
  local group_2_placement="$9"
  local group_2_base="${10}"
  local pair_dir="${mode_dir}/homogeneous/${workload_tag}"
  local group_1_dir="${pair_dir}/group_1"
  local group_2_dir="${pair_dir}/group_2"
  local start_utc
  start_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"

  mkdir -p "${pair_dir}"
  echo "[HOMOGENEOUS] mode=$(basename "${mode_dir}"): ${workload}"
  run_group "${group_1_dir}" "${workload}" "${work_values}" "${processes}" \
    "${group_1_placement}" "${group_1_base}" "${iters}" "${workload}-group-1" \
    > "${pair_dir}/group_1_runner.log" 2>&1 &
  A_RUNNER_PID="$!"
  run_group "${group_2_dir}" "${workload}" "${work_values}" "${processes}" \
    "${group_2_placement}" "${group_2_base}" "${iters}" "${workload}-group-2" \
    > "${pair_dir}/group_2_runner.log" 2>&1 &
  B_RUNNER_PID="$!"

  local group_1_status=0
  local group_2_status=0
  wait "${A_RUNNER_PID}" || group_1_status=$?
  wait "${B_RUNNER_PID}" || group_2_status=$?
  A_RUNNER_PID=""
  B_RUNNER_PID=""
  (( group_1_status == 0 )) || die "${workload} group 1 failed in $(basename "${mode_dir}") mode"
  (( group_2_status == 0 )) || die "${workload} group 2 failed in $(basename "${mode_dir}") mode"

  {
    echo "workload=${workload}"
    echo "start_utc=${start_utc}"
    echo "finish_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "status=PASS"
  } > "${pair_dir}/timing.txt"
}

run_symmetric_mode() {
  local mode="$1"
  local a_placement="$2"
  local a_base="$3"
  local b_placement="$4"
  local b_base="$5"
  local mode_dir="${OUT_ROOT}/${mode}"
  local baseline_a_dir="${mode_dir}/baseline_a"
  local baseline_b_dir="${mode_dir}/baseline_b"
  local a_dir="${mode_dir}/corun/a"
  local b_dir="${mode_dir}/corun/b"
  local a_finish_file="${mode_dir}/.a_finish_utc"
  local b_finish_file="${mode_dir}/.b_finish_utc"

  mkdir -p "${mode_dir}/corun"
  write_mode_mapping "${mode_dir}" "${a_placement}" "${a_base}" \
    "${b_placement}" "${b_base}"

  echo "[BASELINE] mode=${mode}: A"
  run_group "${baseline_a_dir}" "${A_WORKLOAD}" "${A_WORK_VALUES}" "${A_PROCESSES}" \
    "${a_placement}" "${a_base}" "${A_ITERS}" "baseline-A"
  echo "[BASELINE] mode=${mode}: B"
  run_group "${baseline_b_dir}" "${B_WORKLOAD}" "${B_WORK_VALUES}" "${B_PROCESSES}" \
    "${b_placement}" "${b_base}" "${B_ITERS}" "baseline-B"

  local corun_start_utc
  corun_start_utc="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "[CORUN] mode=${mode}: starting A and B"
  (
    set +e
    run_group "${a_dir}" "${A_WORKLOAD}" "${A_WORK_VALUES}" "${A_PROCESSES}" \
      "${a_placement}" "${a_base}" "${A_ITERS}" "corun-A"
    local status=$?
    date -u '+%Y-%m-%dT%H:%M:%SZ' > "${a_finish_file}"
    exit "${status}"
  ) > "${mode_dir}/a_runner.log" 2>&1 &
  A_RUNNER_PID="$!"
  (
    set +e
    run_group "${b_dir}" "${B_WORKLOAD}" "${B_WORK_VALUES}" "${B_PROCESSES}" \
      "${b_placement}" "${b_base}" "${B_ITERS}" "corun-B"
    local status=$?
    date -u '+%Y-%m-%dT%H:%M:%SZ' > "${b_finish_file}"
    exit "${status}"
  ) > "${mode_dir}/b_runner.log" 2>&1 &
  B_RUNNER_PID="$!"

  local a_status=0
  local b_status=0
  wait "${A_RUNNER_PID}" || a_status=$?
  wait "${B_RUNNER_PID}" || b_status=$?
  A_RUNNER_PID=""
  B_RUNNER_PID=""
  (( a_status == 0 )) || die "A group failed in ${mode} mode"
  (( b_status == 0 )) || die "B group failed in ${mode} mode"

  if (( EXTENDED_SYMMETRIC )); then
    run_homogeneous_corun "${mode_dir}" "${A_TAG}" "${A_WORKLOAD}" \
      "${A_WORK_VALUES}" "${A_PROCESSES}" "${A_ITERS}" \
      "${a_placement}" "${a_base}" "${b_placement}" "${b_base}"
    run_homogeneous_corun "${mode_dir}" "${B_TAG}" "${B_WORKLOAD}" \
      "${B_WORK_VALUES}" "${B_PROCESSES}" "${B_ITERS}" \
      "${b_placement}" "${b_base}" "${a_placement}" "${a_base}"
  fi

  local a_finish_utc
  local b_finish_utc
  a_finish_utc="$(<"${a_finish_file}")"
  b_finish_utc="$(<"${b_finish_file}")"
  rm -f "${a_finish_file}" "${b_finish_file}"
  {
    echo "mode=${mode}"
    echo "experiment_type=$(experiment_type)"
    echo "corun_start_utc=${corun_start_utc}"
    echo "a_finish_utc=${a_finish_utc}"
    echo "b_finish_utc=${b_finish_utc}"
    echo "status=PASS"
  } > "${mode_dir}/coverage.txt"
  echo "[PASS] mode=${mode}: symmetric corun completed"
}

run_mode() {
  if (( SYMMETRIC )); then
    run_symmetric_mode "$@"
  else
    run_directional_mode "$@"
  fi
}

{
  echo "started=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
  echo "git_head=$(git -C "${SCRIPT_DIR}" rev-parse HEAD)"
  echo "experiment_type=$(experiment_type)"
  echo "extended_symmetric=${EXTENDED_SYMMETRIC}"
  echo "a_workload=${A_WORKLOAD}"
  echo "a_work_values=${A_WORK_VALUES}"
  echo "a_processes=${A_PROCESSES}"
  echo "a_iters=${A_ITERS}"
  echo "b_workload=${B_WORKLOAD}"
  echo "b_work_values=${B_WORK_VALUES}"
  echo "b_processes=${B_PROCESSES}"
  echo "b_iters=${B_ITERS}"
  echo "warmup=${WARMUP}"
  echo "offload_rates=${OFFLOAD_RATES}"
  echo "stddev_fraction=${STDDEV_FRACTION}"
  echo "numa_node=${NUMA_NODE}"
} > "${OUT_ROOT}/manifest.txt"

run_mode auto none 0 none 0
run_mode samecore same-core "${B_PROCESSES}" same-core 0
run_mode different_core same-core 16 same-core 0

{
  echo "status=PASS"
  echo "finished=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} >> "${OUT_ROOT}/manifest.txt"

if [[ -n "${STAGING_ROOT}" ]]; then
  mkdir -p "${FINAL_OUT_ROOT}"
  cp -a "${STAGING_ROOT}/." "${FINAL_OUT_ROOT}/"
  if ! diff -q \
      <(cd "${STAGING_ROOT}" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
      <(cd "${FINAL_OUT_ROOT}" && find . -type f -print0 | sort -z | xargs -0 sha256sum) \
      >/dev/null; then
    die "NFS copy verification failed; local results remain at ${STAGING_ROOT}"
  fi
  rm -rf -- "${STAGING_ROOT}"
  OUT_ROOT="${FINAL_OUT_ROOT}"
fi

trap - EXIT INT TERM
echo "[PASS] Fairness experiment completed: ${OUT_ROOT}"
