#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_sync_event.h>

#include "host/dpa_compute_type_common.h"

extern "C" {
// one entry per asm workload and baseline
#define DECL_K(name) extern doca_dpa_func_t name;
DECL_K(k_lat_u64_add) DECL_K(k_lat_u64_mul) DECL_K(k_lat_u64_div) DECL_K(k_lat_u64_min)
DECL_K(k_lat_u64_max) DECL_K(k_lat_u64_xor) DECL_K(k_lat_u64_and)
DECL_K(k_lat_u32_add) DECL_K(k_lat_u32_mul) DECL_K(k_lat_u32_div) DECL_K(k_lat_u32_min)
DECL_K(k_lat_u32_max) DECL_K(k_lat_u32_xor) DECL_K(k_lat_u32_and)
DECL_K(k_lat_u8_add)  DECL_K(k_lat_u8_mul)  DECL_K(k_lat_u8_div)  DECL_K(k_lat_u8_min)
DECL_K(k_lat_u8_max)  DECL_K(k_lat_u8_xor)  DECL_K(k_lat_u8_and)
DECL_K(k_tpu_u64_add) DECL_K(k_tpu_u64_mul) DECL_K(k_tpu_u64_div) DECL_K(k_tpu_u64_min)
DECL_K(k_tpu_u64_max) DECL_K(k_tpu_u64_xor) DECL_K(k_tpu_u64_and)
DECL_K(k_tpu_u32_add) DECL_K(k_tpu_u32_mul) DECL_K(k_tpu_u32_div) DECL_K(k_tpu_u32_min)
DECL_K(k_tpu_u32_max) DECL_K(k_tpu_u32_xor) DECL_K(k_tpu_u32_and)
DECL_K(k_tpu_u8_add)  DECL_K(k_tpu_u8_mul)  DECL_K(k_tpu_u8_div)  DECL_K(k_tpu_u8_min)
DECL_K(k_tpu_u8_max)  DECL_K(k_tpu_u8_xor)  DECL_K(k_tpu_u8_and)
DECL_K(k_lat_base)
DECL_K(k_tpu_base)
DECL_K(k_lat_u64_div_base) DECL_K(k_tpu_u64_div_base)
DECL_K(k_lat_u32_div_base) DECL_K(k_tpu_u32_div_base)
DECL_K(k_lat_u8_div_base) DECL_K(k_tpu_u8_div_base)
DECL_K(k_lat_f64_add) DECL_K(k_lat_f64_mul) DECL_K(k_lat_f64_div) DECL_K(k_lat_f64_min) DECL_K(k_lat_f64_max)
DECL_K(k_lat_f32_add) DECL_K(k_lat_f32_mul) DECL_K(k_lat_f32_div) DECL_K(k_lat_f32_min) DECL_K(k_lat_f32_max)
DECL_K(k_tpu_f64_add) DECL_K(k_tpu_f64_mul) DECL_K(k_tpu_f64_div)
DECL_K(k_tpu_f32_add) DECL_K(k_tpu_f32_mul) DECL_K(k_tpu_f32_div)
DECL_K(k_lat_fp_base)
DECL_K(k_tpu_fp_base)
#undef DECL_K
}

extern struct doca_dpa_app *DPA_APP_NAME;
DOCA_LOG_REGISTER(DPA_COMPUTE_TYPE_BENCH);

namespace {

constexpr uint64_t kSyncEventMask = UINT64_MAX;

// 8 ops per loop, chained for latency or independent for throughput
// total_ops = op_iterations * kWidth
constexpr uint32_t kWidth = 8;
constexpr uint32_t kClockMhz = 1800;

struct NameId {
    const char *name;
    uint32_t id;
};

constexpr NameId kModes[] = {
    {"latency", DPA_COMPUTE_MODE_LATENCY},
    {"throughput", DPA_COMPUTE_MODE_THROUGHPUT},
};
constexpr NameId kDtypes[] = {
    {"u8", DPA_COMPUTE_DTYPE_UINT8},
    {"u32", DPA_COMPUTE_DTYPE_UINT32},
    {"u64", DPA_COMPUTE_DTYPE_UINT64},
    {"f32", DPA_COMPUTE_DTYPE_FLOAT32},
    {"f64", DPA_COMPUTE_DTYPE_FLOAT64},
};

bool dtype_is_softfp(uint32_t dtype)
{
    return dtype == DPA_COMPUTE_DTYPE_FLOAT32 || dtype == DPA_COMPUTE_DTYPE_FLOAT64;
}
constexpr NameId kOps[] = {
    {"add", DPA_COMPUTE_OP_ADD},
    {"mul", DPA_COMPUTE_OP_MUL},
    {"div", DPA_COMPUTE_OP_DIV},
    {"min", DPA_COMPUTE_OP_MIN},
    {"max", DPA_COMPUTE_OP_MAX},
    {"xor", DPA_COMPUTE_OP_XOR},
    {"and", DPA_COMPUTE_OP_AND},
};

struct FixedDivCase {
    uint32_t dtype;
    uint64_t dividend;
    doca_dpa_func_t *latency_base;
    doca_dpa_func_t *throughput_base;
};

const FixedDivCase kFixedDivCases[] = {
    {DPA_COMPUTE_DTYPE_UINT8, 0xffULL, &k_lat_u8_div_base, &k_tpu_u8_div_base},
    {DPA_COMPUTE_DTYPE_UINT32, 0xffffffffULL, &k_lat_u32_div_base, &k_tpu_u32_div_base},
    {DPA_COMPUTE_DTYPE_UINT64, 0x7fffffffffffffffULL, &k_lat_u64_div_base, &k_tpu_u64_div_base},
};

const FixedDivCase *fixed_div_case(uint32_t dtype)
{
    for (const auto &c : kFixedDivCases) {
        if (c.dtype == dtype) {
            return &c;
        }
    }
    return nullptr;
}

struct KernelCase {
    uint32_t mode;
    uint32_t dtype;
    uint32_t op;
    doca_dpa_func_t *workload;
};

#define K(MODE_ID, DTYPE_ID, OP_ID, FN) {MODE_ID, DTYPE_ID, OP_ID, &FN}

constexpr KernelCase kKernelCases[] = {
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_ADD, k_lat_u8_add),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MUL, k_lat_u8_mul),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_DIV, k_lat_u8_div),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MIN, k_lat_u8_min),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MAX, k_lat_u8_max),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_XOR, k_lat_u8_xor),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_AND, k_lat_u8_and),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_ADD, k_lat_u32_add),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MUL, k_lat_u32_mul),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_DIV, k_lat_u32_div),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MIN, k_lat_u32_min),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MAX, k_lat_u32_max),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_XOR, k_lat_u32_xor),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_AND, k_lat_u32_and),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_ADD, k_lat_u64_add),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MUL, k_lat_u64_mul),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_DIV, k_lat_u64_div),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MIN, k_lat_u64_min),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MAX, k_lat_u64_max),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_XOR, k_lat_u64_xor),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_AND, k_lat_u64_and),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_ADD, k_tpu_u8_add),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MUL, k_tpu_u8_mul),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_DIV, k_tpu_u8_div),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MIN, k_tpu_u8_min),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_MAX, k_tpu_u8_max),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_XOR, k_tpu_u8_xor),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT8,  DPA_COMPUTE_OP_AND, k_tpu_u8_and),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_ADD, k_tpu_u32_add),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MUL, k_tpu_u32_mul),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_DIV, k_tpu_u32_div),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MIN, k_tpu_u32_min),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_MAX, k_tpu_u32_max),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_XOR, k_tpu_u32_xor),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_OP_AND, k_tpu_u32_and),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_ADD, k_tpu_u64_add),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MUL, k_tpu_u64_mul),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_DIV, k_tpu_u64_div),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MIN, k_tpu_u64_min),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_MAX, k_tpu_u64_max),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_XOR, k_tpu_u64_xor),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_UINT64, DPA_COMPUTE_OP_AND, k_tpu_u64_and),
    // soft-float ops, min/max only have latency tests
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_ADD, k_lat_f32_add),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_MUL, k_lat_f32_mul),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_DIV, k_lat_f32_div),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_MIN, k_lat_f32_min),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_MAX, k_lat_f32_max),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_ADD, k_lat_f64_add),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_MUL, k_lat_f64_mul),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_DIV, k_lat_f64_div),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_MIN, k_lat_f64_min),
    K(DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_MAX, k_lat_f64_max),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_ADD, k_tpu_f32_add),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_MUL, k_tpu_f32_mul),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT32, DPA_COMPUTE_OP_DIV, k_tpu_f32_div),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_ADD, k_tpu_f64_add),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_MUL, k_tpu_f64_mul),
    K(DPA_COMPUTE_MODE_THROUGHPUT, DPA_COMPUTE_DTYPE_FLOAT64, DPA_COMPUTE_OP_DIV, k_tpu_f64_div),
};
#undef K

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t warmup = 20;
    uint32_t iters = 100;
    uint64_t op_iterations = 20000;
    bool print_header = true;
    std::vector<uint32_t> modes = {DPA_COMPUTE_MODE_LATENCY, DPA_COMPUTE_MODE_THROUGHPUT};
    std::vector<uint32_t> dtypes = {
        DPA_COMPUTE_DTYPE_UINT8, DPA_COMPUTE_DTYPE_UINT32, DPA_COMPUTE_DTYPE_UINT64};
    std::vector<uint32_t> ops = {
        DPA_COMPUTE_OP_ADD, DPA_COMPUTE_OP_MUL, DPA_COMPUTE_OP_DIV,
        DPA_COMPUTE_OP_MIN, DPA_COMPUTE_OP_MAX, DPA_COMPUTE_OP_XOR, DPA_COMPUTE_OP_AND};
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;
};

struct SyncEventResource {
    doca_sync_event *event = nullptr;
    doca_dpa_dev_sync_event_t dpa_handle = 0;
};

struct KernelState {
    SyncEventResource completion;
    SyncEventResource cycles;
    SyncEventResource instret;
    SyncEventResource checksum;
    uint64_t seq = 0;
};

// keep the minimum, sums for the mean and a checksum
struct Stats {
    uint64_t min_cycles = UINT64_MAX;
    uint64_t sum_cycles = 0;
    uint64_t min_instret = UINT64_MAX;
    uint64_t checksum = 0; // checksum from one iteration
};

volatile uint64_t g_checksum_sink = 0;

[[noreturn]] void throw_runtime(const std::string &msg) { throw std::runtime_error(msg); }

void check_doca(doca_error_t st, const char *what)
{
    if (st != DOCA_SUCCESS) {
        throw_runtime(std::string(what) + " failed: " + doca_error_get_descr(st));
    }
}

std::string to_lower_ascii(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

template <size_t N>
std::vector<uint32_t> parse_name_list(const std::string &csv, const NameId (&table)[N],
                                      const char *kind_name)
{
    std::vector<uint32_t> ids;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        token = to_lower_ascii(token);
        if (token == "all") {
            ids.clear();
            for (const auto &e : table) {
                ids.push_back(e.id);
            }
            return ids;
        }
        bool found = false;
        for (const auto &e : table) {
            if (token == e.name) {
                ids.push_back(e.id);
                found = true;
                break;
            }
        }
        if (!found) {
            throw_runtime(std::string("Unknown ") + kind_name + " token: " + token);
        }
    }
    if (ids.empty()) {
        throw_runtime(std::string("Empty ") + kind_name + " set");
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

template <size_t N>
const char *name_of_id(uint32_t id, const NameId (&table)[N], const char *unknown)
{
    for (const auto &e : table) {
        if (e.id == id) {
            return e.name;
        }
    }
    return unknown;
}

const KernelCase *find_kernel_case(uint32_t mode, uint32_t dtype, uint32_t op)
{
    for (const auto &c : kKernelCases) {
        if (c.mode == mode && c.dtype == dtype && c.op == op) {
            return &c;
        }
    }
    return nullptr;
}

doca_dpa_func_t *baseline_for(uint32_t mode, uint32_t dtype)
{
    // integer and float loops need separate baselines
    if (dtype_is_softfp(dtype)) {
        return (mode == DPA_COMPUTE_MODE_LATENCY) ? &k_lat_fp_base : &k_tpu_fp_base;
    }
    return (mode == DPA_COMPUTE_MODE_LATENCY) ? &k_lat_base : &k_tpu_base;
}

Config parse_args(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw_runtime(std::string("Missing value for ") + name);
            }
            return std::string(argv[++i]);
        };
        if (arg == "--device" || arg == "--device-name") {
            cfg.device_name = require_value("--device-name");
        } else if (arg == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(std::stoul(require_value("--warmup")));
        } else if (arg == "--iters") {
            cfg.iters = static_cast<uint32_t>(std::stoul(require_value("--iters")));
        } else if (arg == "--op-iterations") {
            cfg.op_iterations = std::stoull(require_value("--op-iterations"));
        } else if (arg == "--modes") {
            cfg.modes = parse_name_list(require_value("--modes"), kModes, "mode");
        } else if (arg == "--types") {
            cfg.dtypes = parse_name_list(require_value("--types"), kDtypes, "type");
        } else if (arg == "--ops") {
            cfg.ops = parse_name_list(require_value("--ops"), kOps, "op");
        } else if (arg == "--no-header") {
            cfg.print_header = false;
        } else if (arg == "-h" || arg == "--help") {
            std::cout
                << "dpa_compute_type_bench options:\n"
                << "  --device-name <ibdev>      Device name (default: mlx5_0)\n"
                << "  --warmup <num>             Warmup launches per case (default: 20)\n"
                << "  --iters <num>              Measured launches per case (default: 100)\n"
                << "  --op-iterations <num>      Loop iters per timed kernel (default: 20000)\n"
                << "  --modes <csv>              latency,throughput,all (default: all)\n"
                << "  --types <csv>              u8,u32,u64,all (default: all)\n"
                << "  --ops <csv>                add,mul,div,min,max,xor,and,all (default: all)\n"
                << "  --no-header                Suppress CSV header line\n"
                << "  latency uses 8 chained ops, throughput uses 8 independent lanes\n"
                << "  asm timings use the minimum cycles minus the baseline\n"
                << "  div keeps its input fixed, latency uses a matching xor control\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }
    if (cfg.iters == 0) {
        throw_runtime("--iters must be > 0");
    }
    if (cfg.op_iterations == 0 || cfg.op_iterations > UINT64_MAX / kWidth) {
        throw_runtime("--op-iterations must produce a nonzero, non-overflowing operation count");
    }
    return cfg;
}

void init_doca_log()
{
    check_doca(doca_log_backend_create_standard(), "doca_log_backend_create_standard");
    struct doca_log_backend *sdk_log = nullptr;
    check_doca(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
    check_doca(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING), "doca_log_backend_set_sdk_level");
}

void open_device_by_ibdev_name(const std::string &device_name, doca_dev **dev)
{
    struct doca_devinfo **devinfo_list = nullptr;
    uint32_t nb_devs = 0;
    check_doca(doca_devinfo_create_list(&devinfo_list, &nb_devs), "doca_devinfo_create_list");

    char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
    bool found = false;
    for (uint32_t i = 0; i < nb_devs; ++i) {
        if (doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != DOCA_SUCCESS) {
            continue;
        }
        if (device_name == ibdev_name) {
            check_doca(doca_dev_open(devinfo_list[i], dev), "doca_dev_open");
            found = true;
            break;
        }
    }
    check_doca(doca_devinfo_destroy_list(devinfo_list), "doca_devinfo_destroy_list");
    if (!found || *dev == nullptr) {
        throw_runtime("IB device not found: " + device_name);
    }
}

void init_resources(const Config &cfg, Resources &r)
{
    open_device_by_ibdev_name(cfg.device_name, &r.dev);
    check_doca(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
    check_doca(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
    check_doca(doca_dpa_start(r.dpa), "doca_dpa_start");
    check_doca(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");
}

void cleanup_resources(Resources &r)
{
    if (r.dpa != nullptr) {
        (void)doca_dpa_stop(r.dpa);
        (void)doca_dpa_destroy(r.dpa);
        r.dpa = nullptr;
    }
    if (r.dev != nullptr) {
        (void)doca_dev_close(r.dev);
        r.dev = nullptr;
    }
}

void destroy_sync_event(SyncEventResource &se)
{
    if (se.event != nullptr) {
        (void)doca_sync_event_stop(se.event);
        (void)doca_sync_event_destroy(se.event);
        se.event = nullptr;
        se.dpa_handle = 0;
    }
}

void create_sync_event_dpa_to_cpu(const Resources &r, bool need_dpa_handle, SyncEventResource &se)
{
    check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
    check_doca(doca_sync_event_add_publisher_location_dpa(se.event, r.dpa), "add_publisher_location_dpa");
    check_doca(doca_sync_event_add_subscriber_location_cpu(se.event, r.dev), "add_subscriber_location_cpu");
    check_doca(doca_sync_event_start(se.event), "sync_event_start");
    if (need_dpa_handle) {
        check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle), "sync_event_get_dpa_handle");
    }
}

void init_kernel_state(const Resources &r, KernelState &st)
{
    create_sync_event_dpa_to_cpu(r, false, st.completion);
    try {
        create_sync_event_dpa_to_cpu(r, true, st.cycles);
        create_sync_event_dpa_to_cpu(r, true, st.instret);
        create_sync_event_dpa_to_cpu(r, true, st.checksum);
    } catch (...) {
        destroy_sync_event(st.checksum);
        destroy_sync_event(st.instret);
        destroy_sync_event(st.cycles);
        destroy_sync_event(st.completion);
        throw;
    }
}

void cleanup_kernel_state(KernelState &st)
{
    destroy_sync_event(st.checksum);
    destroy_sync_event(st.instret);
    destroy_sync_event(st.cycles);
    destroy_sync_event(st.completion);
    st.seq = 0;
}

// run fn on one thread and collect its counters
void launch_and_read(const Resources &r, KernelState &st, doca_dpa_func_t *fn, uint64_t op_iterations,
                     uint64_t &cycles, uint64_t &instret, uint64_t &checksum)
{
    ++st.seq;
    check_doca(doca_dpa_kernel_launch_update_set(
                   r.dpa, nullptr, 0, st.completion.event, st.seq, 1, fn,
                   r.dpa_handle, st.cycles.dpa_handle, st.instret.dpa_handle, st.checksum.dpa_handle,
                   op_iterations),
               "doca_dpa_kernel_launch_update_set");
    check_doca(doca_sync_event_wait_gt(st.completion.event, st.seq - 1, kSyncEventMask),
               "doca_sync_event_wait_gt(completion)");
    check_doca(doca_sync_event_get(st.cycles.event, &cycles), "sync_event_get(cycles)");
    check_doca(doca_sync_event_get(st.instret.event, &instret), "sync_event_get(instret)");
    check_doca(doca_sync_event_get(st.checksum.event, &checksum), "sync_event_get(checksum)");
}

Stats measure(const Config &cfg, const Resources &r, KernelState &st, doca_dpa_func_t *fn,
              uint64_t op_iterations, const uint64_t *expected_checksum = nullptr)
{
    Stats s;
    const uint64_t total_runs = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
    for (uint64_t i = 0; i < total_runs; ++i) {
        uint64_t cycles = 0, instret = 0, checksum = 0;
        launch_and_read(r, st, fn, op_iterations, cycles, instret, checksum);
        if (expected_checksum != nullptr && (checksum != *expected_checksum || cycles == 0)) {
            throw_runtime("Fixed-input integer div checksum/cycle validation failed");
        }
        if (i >= cfg.warmup) {
            s.min_cycles = std::min(s.min_cycles, cycles);
            s.sum_cycles += cycles;
            s.min_instret = std::min(s.min_instret, instret);
            s.checksum = checksum; // checksum from the last measured iteration
        }
    }
    return s;
}

void print_header()
{
    std::cout
        << "mode,dtype,op,status,op_iterations,factor,total_ops,"
        << "min_cycles,mean_cycles,baseline_cycles,cycles_per_op,ops_per_cycle,mops,"
        << "instret_per_op,checksum,raw_cycles_per_op\n";
}

// write one CSV row with the matching baseline
void emit_row(uint32_t mode, uint32_t dtype, uint32_t op, const char *status,
              uint64_t op_iterations, uint32_t iters, const Stats &wl, const Stats &base,
              uint64_t checksum)
{
    const double total_ops = static_cast<double>(op_iterations) * static_cast<double>(kWidth);
    const double baseline = static_cast<double>(base.min_cycles);
    const double compute_cycles = static_cast<double>(wl.min_cycles) - baseline;
    // timing covers the asm call and counter reads, not result publication
    // subtracting the baseline gives an estimate
    const double mean_cycles = static_cast<double>(wl.sum_cycles) / static_cast<double>(iters);
    const double cycles_per_op = (compute_cycles > 0.0) ? (compute_cycles / total_ops) : 0.0;
    const double ops_per_cycle = (compute_cycles > 0.0) ? (total_ops / compute_cycles) : 0.0;
    const double mops = ops_per_cycle * static_cast<double>(kClockMhz);
    const double compute_instret = (wl.min_instret > base.min_instret)
                                       ? static_cast<double>(wl.min_instret - base.min_instret)
                                       : 0.0;
    const double instret_per_op = (compute_instret > 0.0) ? (compute_instret / total_ops) : 0.0;
    if (!dtype_is_softfp(dtype) && op == DPA_COMPUTE_OP_DIV &&
        (compute_cycles <= 0.0 || instret_per_op < 0.99 || instret_per_op > 1.01)) {
        throw_runtime("Fixed-input integer div timing/instruction validation failed");
    }

    std::cout << std::fixed << std::setprecision(3)
              << name_of_id(mode, kModes, "unknown_mode") << ","
              << name_of_id(dtype, kDtypes, "unknown_dtype") << ","
              << name_of_id(op, kOps, "unknown_op") << ","
              << status << ","
              << op_iterations << ","
              << kWidth << ","
              << total_ops << ","
              << static_cast<double>(wl.min_cycles) << ","
              << mean_cycles << ","
              << baseline << ","
              << cycles_per_op << ","
              << ops_per_cycle << ","
              << mops << ","
              << instret_per_op << ","
              << checksum << ","
              << static_cast<double>(wl.min_cycles) / total_ops << "\n";
    std::cout << std::unitbuf;
}

void run_benchmark(const Config &cfg)
{
    Resources r{};
    KernelState st{};
    try {
        init_resources(cfg, r);
        init_kernel_state(r, st);

        std::cout << "# device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " op_iterations=" << cfg.op_iterations
                  << " clock_mhz=" << kClockMhz
                  << " width=" << kWidth
                  << " div_inputs=u8:0xff,u32:0xffffffff,u64:0x7fffffffffffffff"
                  << " div_divisor=3 div_latency=div_xor_minus_matched_control"
                  << " (latency=1 chain x" << kWidth << " unrolled, throughput=" << kWidth << " indep lanes)"
                  << "\n";
        if (cfg.print_header) {
            print_header();
        }

        for (uint32_t mode : cfg.modes) {
            // measure each baseline once and reuse it for matching cases
            const Stats int_base = measure(cfg, r, st, baseline_for(mode, DPA_COMPUTE_DTYPE_UINT64),
                                           cfg.op_iterations);
            const Stats fp_base = measure(cfg, r, st, baseline_for(mode, DPA_COMPUTE_DTYPE_FLOAT64),
                                          cfg.op_iterations);
            g_checksum_sink ^= int_base.checksum ^ fp_base.checksum;

            for (uint32_t dtype : cfg.dtypes) {
                const Stats &base = dtype_is_softfp(dtype) ? fp_base : int_base;
                const char *status = dtype_is_softfp(dtype) ? "ok_softfp" : "ok";
                for (uint32_t op : cfg.ops) {
                    const KernelCase *c = find_kernel_case(mode, dtype, op);
                    if (c == nullptr) {
                        std::cout << name_of_id(mode, kModes, "?") << ","
                                  << name_of_id(dtype, kDtypes, "?") << ","
                                  << name_of_id(op, kOps, "?") << ",n/a\n";
                        continue;
                    }
                    const FixedDivCase *div = op == DPA_COMPUTE_OP_DIV ? fixed_div_case(dtype) : nullptr;
                    Stats selected_base = base;
                    const char *row_status = status;
                    uint64_t expected_checksum = 0;
                    if (div != nullptr) {
                        const bool latency = mode == DPA_COMPUTE_MODE_LATENCY;
                        expected_checksum = latency ? div->dividend : (div->dividend / 3) * kWidth + 3;
                        selected_base = measure(cfg, r, st,
                                                latency ? div->latency_base : div->throughput_base,
                                                cfg.op_iterations, &expected_checksum);
                        row_status = latency ? "ok_fixed_div_xor_estimate" : "ok_fixed_div";
                    }
                    const Stats wl = measure(cfg, r, st, c->workload, cfg.op_iterations,
                                             div != nullptr ? &expected_checksum : nullptr);
                    g_checksum_sink ^= wl.checksum;
                    emit_row(mode, dtype, op, row_status, cfg.op_iterations, cfg.iters,
                             wl, selected_base, wl.checksum);
                }
            }

        }

        cleanup_kernel_state(st);
        cleanup_resources(r);
    } catch (...) {
        cleanup_kernel_state(st);
        cleanup_resources(r);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        init_doca_log();
        const Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
