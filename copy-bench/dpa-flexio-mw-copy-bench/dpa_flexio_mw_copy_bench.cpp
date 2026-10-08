#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

#include <libflexio/flexio_ver.h>
#define FLEXIO_VER_USED FLEXIO_VER(25, 10, 0)
#include <libflexio/flexio.h>

#include "host/flexio_mw_copy_common.h"

extern "C" {
flexio_func_t flexio_mw_copy_run_rpc;
}

extern struct flexio_app *DPA_APP_NAME;

namespace {

constexpr uint64_t kDefaultMinPayload = 64;
constexpr uint64_t kDefaultMaxPayload = 1ULL << 26; // 64 MiB
constexpr double kDefaultSizeFactor = 2.0;
constexpr uint32_t kDefaultWarmup = 20;
constexpr uint32_t kDefaultIters = 200;
constexpr uint32_t kDefaultClockMHz = 1800;
constexpr uint32_t kPatternSeed = 0x5A;
constexpr size_t kWindowLineBytes = 64;

enum class CopyMode : uint32_t {
    WindowApi = MW_COPY_MODE_WINDOW_API,
    CpuPointer = MW_COPY_MODE_CPU_POINTER,
};

enum class CopyDirection : uint32_t {
    H2D = MW_COPY_DIRECTION_H2D,
    D2H = MW_COPY_DIRECTION_D2H,
};

struct Config {
    std::string device_name = "mlx5_0";
    CopyMode mode = CopyMode::WindowApi;
    CopyDirection direction = CopyDirection::H2D;
    uint64_t min_payload = kDefaultMinPayload;
    uint64_t max_payload = kDefaultMaxPayload;
    double size_factor = kDefaultSizeFactor;
    uint32_t warmup = kDefaultWarmup;
    uint32_t iters = kDefaultIters;
    uint32_t clock_mhz = kDefaultClockMHz;
    bool verify = false;
};

struct Resources {
    ibv_context *ibv_ctx = nullptr;
    ibv_pd *process_pd = nullptr;
    ibv_mr *host_mr = nullptr;

    flexio_process *process = nullptr;
    flexio_window *window = nullptr;

    void *host_src = nullptr;
    size_t host_src_bytes = 0;

    uint8_t *verify_buf = nullptr;
    size_t verify_buf_bytes = 0;

    flexio_uintptr_t dst_daddr = 0;
    size_t dst_daddr_bytes = 0;

    flexio_uintptr_t arg_daddr = 0;
};

[[noreturn]] void throw_runtime(const std::string &msg)
{
    throw std::runtime_error(msg);
}

void check_flexio(flexio_status status, const char *what)
{
    if (status != FLEXIO_STATUS_SUCCESS) {
        throw_runtime(std::string(what) + " failed with status=" + std::to_string((int)status));
    }
}

uint64_t parse_u64(const std::string &s, const char *name)
{
    size_t idx = 0;
    uint64_t v = 0;
    try {
        v = std::stoull(s, &idx, 0);
    } catch (const std::exception &) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    if (idx != s.size()) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    return v;
}

double parse_double(const std::string &s, const char *name)
{
    size_t idx = 0;
    double v = 0;
    try {
        v = std::stod(s, &idx);
    } catch (const std::exception &) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    if (idx != s.size()) {
        throw_runtime(std::string("Invalid ") + name + ": " + s);
    }
    return v;
}

CopyMode parse_mode(const std::string &s)
{
    if (s == "api") {
        return CopyMode::WindowApi;
    }
    if (s == "cpu" || s == "cpu32") {
        return CopyMode::CpuPointer;
    }
    throw_runtime("Unsupported mode: " + s + " (expected: api|cpu)");
}

CopyDirection parse_direction(const std::string &s)
{
    if (s == "h2d") {
        return CopyDirection::H2D;
    }
    if (s == "d2h") {
        return CopyDirection::D2H;
    }
    throw_runtime("Unsupported direction: " + s + " (expected: h2d|d2h)");
}

const char *mode_to_string(CopyMode mode)
{
    switch (mode) {
    case CopyMode::WindowApi:
        return "api_copy";
    case CopyMode::CpuPointer:
        return "cpu_copy";
    default:
        return "unknown";
    }
}

const char *direction_to_string(CopyDirection direction)
{
    switch (direction) {
    case CopyDirection::H2D:
        return "h2d";
    case CopyDirection::D2H:
        return "d2h";
    default:
        return "unknown";
    }
}

void print_usage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --device mlx5_0 [options]\n\n"
        << "Options:\n"
        << "  --device <name>                 IB device name (default: mlx5_0)\n"
        << "  --mode api|cpu                  Copy path on DPA (default: api)\n"
        << "  --dir h2d|d2h                   Copy direction (default: h2d)\n"
        << "  --min-payload <bytes>           Min payload bytes (default: 64)\n"
        << "  --max-payload <bytes>           Max payload bytes (default: 67108864)\n"
        << "  --size-factor <float>           Payload growth factor (default: 2.0)\n"
        << "  --warmup <num>                  Warmup iterations per payload (default: 20)\n"
        << "  --iters <num>                   Measured iterations per payload (default: 200)\n"
        << "  --clock-mhz <num>               DPA clock MHz for ns conversion (default: 1800)\n"
        << "  --verify                        Verify copied payload content\n";
}

Config parse_args(int argc, char **argv)
{
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw_runtime(std::string("Missing value for ") + name);
            }
            return std::string(argv[++i]);
        };

        if (arg == "--device") {
            cfg.device_name = next("--device");
        } else if (arg == "--mode") {
            cfg.mode = parse_mode(next("--mode"));
        } else if (arg == "--dir") {
            cfg.direction = parse_direction(next("--dir"));
        } else if (arg == "--min-payload") {
            cfg.min_payload = parse_u64(next("--min-payload"), "--min-payload");
        } else if (arg == "--max-payload") {
            cfg.max_payload = parse_u64(next("--max-payload"), "--max-payload");
        } else if (arg == "--size-factor") {
            cfg.size_factor = parse_double(next("--size-factor"), "--size-factor");
        } else if (arg == "--warmup") {
            cfg.warmup = (uint32_t)parse_u64(next("--warmup"), "--warmup");
        } else if (arg == "--iters") {
            cfg.iters = (uint32_t)parse_u64(next("--iters"), "--iters");
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = (uint32_t)parse_u64(next("--clock-mhz"), "--clock-mhz");
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }

    if (cfg.min_payload == 0) {
        throw_runtime("--min-payload must be > 0");
    }
    if (cfg.max_payload < cfg.min_payload) {
        throw_runtime("--max-payload must be >= --min-payload");
    }
    if (cfg.size_factor <= 1.0) {
        throw_runtime("--size-factor must be > 1.0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }
    if (cfg.max_payload > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("--max-payload must fit in uint32");
    }
    uint64_t total_iters = (uint64_t)cfg.warmup + (uint64_t)cfg.iters;
    if (total_iters > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("warmup + iters exceeds uint32 range");
    }

    return cfg;
}

std::vector<uint64_t> build_payload_list(const Config &cfg)
{
    std::vector<uint64_t> payloads;
    uint64_t cur = cfg.min_payload;

    while (cur <= cfg.max_payload) {
        payloads.push_back(cur);

        uint64_t next = (uint64_t)((double)cur * cfg.size_factor);
        if (next <= cur) {
            next = cur + 1;
        }
        if (next > cfg.max_payload && cur != cfg.max_payload) {
            payloads.push_back(cfg.max_payload);
            break;
        }
        cur = next;
    }

    if (payloads.empty()) {
        payloads.push_back(cfg.max_payload);
    }

    payloads.erase(std::unique(payloads.begin(), payloads.end()), payloads.end());
    return payloads;
}

ibv_context *open_ibv_device(const std::string &name)
{
    int dev_num = 0;
    ibv_device **dev_list = ibv_get_device_list(&dev_num);
    if (dev_list == nullptr || dev_num <= 0) {
        throw_runtime("ibv_get_device_list failed");
    }

    ibv_context *ctx = nullptr;
    for (int i = 0; i < dev_num; ++i) {
        const char *dev_name = ibv_get_device_name(dev_list[i]);
        if (dev_name != nullptr && name == dev_name) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }

    ibv_free_device_list(dev_list);

    if (ctx == nullptr) {
        throw_runtime("Failed to open IB device: " + name);
    }
    return ctx;
}

void fill_src_pattern(uint8_t *buf, uint32_t payload)
{
    for (uint32_t i = 0; i < payload; ++i) {
        buf[i] = (uint8_t)((((uint32_t)i * 131U) + payload + kPatternSeed) & 0xFFU);
    }
}

void cleanup(Resources &r)
{
    if (r.arg_daddr != 0 && r.process != nullptr) {
        (void)flexio_buf_dev_free(r.process, r.arg_daddr);
        r.arg_daddr = 0;
    }

    if (r.dst_daddr != 0 && r.process != nullptr) {
        (void)flexio_buf_dev_free(r.process, r.dst_daddr);
        r.dst_daddr = 0;
    }

    if (r.host_mr != nullptr) {
        (void)ibv_dereg_mr(r.host_mr);
        r.host_mr = nullptr;
    }

    if (r.window != nullptr) {
        (void)flexio_window_destroy(r.window);
        r.window = nullptr;
    }

    if (r.process != nullptr) {
        (void)flexio_process_destroy(r.process);
        r.process = nullptr;
    }

    if (r.verify_buf != nullptr) {
        free(r.verify_buf);
        r.verify_buf = nullptr;
    }

    if (r.host_src != nullptr) {
        free(r.host_src);
        r.host_src = nullptr;
    }

    if (r.ibv_ctx != nullptr) {
        (void)ibv_close_device(r.ibv_ctx);
        r.ibv_ctx = nullptr;
    }
}

void init_resources(const Config &cfg, Resources &r)
{
    check_flexio(flexio_version_set(FLEXIO_VER_USED), "flexio_version_set");

    r.ibv_ctx = open_ibv_device(cfg.device_name);

    check_flexio(flexio_process_create(r.ibv_ctx, DPA_APP_NAME, nullptr, &r.process),
                 "flexio_process_create");

    r.process_pd = flexio_process_get_pd(r.process);
    if (r.process_pd == nullptr) {
        throw_runtime("flexio_process_get_pd returned null");
    }

    check_flexio(flexio_window_create(r.process, r.process_pd, &r.window), "flexio_window_create");

    r.host_src_bytes = (size_t)cfg.max_payload;
    if (r.host_src_bytes < kWindowLineBytes) {
        r.host_src_bytes = kWindowLineBytes;
    }
    r.host_src_bytes = (r.host_src_bytes + (kWindowLineBytes - 1)) & ~(kWindowLineBytes - 1);

    if (posix_memalign(&r.host_src, 64, r.host_src_bytes) != 0 || r.host_src == nullptr) {
        throw_runtime("posix_memalign failed for host source buffer");
    }
    std::memset(r.host_src, 0, r.host_src_bytes);

    r.host_mr = ibv_reg_mr(r.process_pd,
                           r.host_src,
                           r.host_src_bytes,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (r.host_mr == nullptr) {
        throw_runtime("ibv_reg_mr failed for host source buffer");
    }

    r.dst_daddr_bytes = (size_t)cfg.max_payload;
    if (r.dst_daddr_bytes < kWindowLineBytes) {
        r.dst_daddr_bytes = kWindowLineBytes;
    }
    r.dst_daddr_bytes = (r.dst_daddr_bytes + (kWindowLineBytes - 1)) & ~(kWindowLineBytes - 1);
    check_flexio(flexio_buf_dev_alloc(r.process, r.dst_daddr_bytes, &r.dst_daddr),
                 "flexio_buf_dev_alloc(dst_daddr)");

    check_flexio(flexio_buf_dev_alloc(r.process, sizeof(struct mw_copy_rpc_args), &r.arg_daddr),
                 "flexio_buf_dev_alloc(arg_daddr)");

    r.verify_buf_bytes = r.host_src_bytes;
    r.verify_buf = (uint8_t *)malloc(r.verify_buf_bytes);
    if (r.verify_buf == nullptr) {
        throw_runtime("malloc failed for verify buffer");
    }
}

struct mw_copy_rpc_args fetch_args_from_dpa(const Resources &r)
{
    struct mw_copy_rpc_args out{};

    struct flexio_mem src{};
    src.mem_type = FLEXIO_MEMTYPE_DPA;
    src.daddr = r.arg_daddr;

    struct flexio_mem dst{};
    dst.mem_type = FLEXIO_MEMTYPE_HOST;
    dst.haddr = (uintptr_t)&out;

    check_flexio(flexio_memcpy(r.process, &src, &dst, sizeof(out)), "flexio_memcpy(args dpa->host)");
    return out;
}

uint64_t run_one_payload(const Config &cfg, Resources &r, uint32_t payload)
{
    struct mw_copy_rpc_args args{};
    args.window_id = flexio_window_get_id(r.window);
    args.mkey = r.host_mr->lkey;
    args.copy_mode = (uint32_t)cfg.mode;
    args.copy_direction = (uint32_t)cfg.direction;
    args.payload_bytes = payload;
    args.warmup_iters = cfg.warmup;
    args.measure_iters = cfg.iters;
    args.copy_status = MW_COPY_STATUS_OK;
    args.fail_iter = UINT32_MAX;
    args.src_haddr = (uint64_t)(uintptr_t)r.host_src;
    args.dst_daddr = r.dst_daddr;

    check_flexio(flexio_host2dev_memcpy(r.process, &args, sizeof(args), r.arg_daddr),
                 "flexio_host2dev_memcpy(args)");

    uint64_t rpc_ret = 0;
    check_flexio(flexio_process_call(r.process, &flexio_mw_copy_run_rpc, &rpc_ret, r.arg_daddr),
                 "flexio_process_call(flexio_mw_copy_run_rpc)");

    struct mw_copy_rpc_args post = fetch_args_from_dpa(r);
    if (post.copy_status != MW_COPY_STATUS_OK) {
        throw_runtime("device copy failed with status=" + std::to_string(post.copy_status) +
                      " fail_iter=" + std::to_string(post.fail_iter));
    }

    return post.total_cycles;
}

void verify_payload(const Config &cfg, const Resources &r, uint32_t payload)
{
    if (cfg.direction == CopyDirection::H2D) {
        struct flexio_mem src{};
        src.mem_type = FLEXIO_MEMTYPE_DPA;
        src.daddr = r.dst_daddr;

        struct flexio_mem dst{};
        dst.mem_type = FLEXIO_MEMTYPE_HOST;
        dst.haddr = (uintptr_t)r.verify_buf;

        check_flexio(flexio_memcpy(r.process, &src, &dst, payload), "flexio_memcpy(dst dpa->host verify)");

        if (std::memcmp(r.verify_buf, r.host_src, payload) == 0) {
            return;
        }

        for (uint32_t i = 0; i < payload; ++i) {
            if (r.verify_buf[i] != ((uint8_t *)r.host_src)[i]) {
                throw_runtime("verify failed: copied payload mismatch at byte " + std::to_string(i) +
                              " actual=" + std::to_string((uint32_t)r.verify_buf[i]) +
                              " expected=" + std::to_string((uint32_t)((uint8_t *)r.host_src)[i]));
            }
        }
        throw_runtime("verify failed: copied payload mismatch");
    }

    if (std::memcmp(r.host_src, r.verify_buf, payload) == 0) {
        return;
    }

    for (uint32_t i = 0; i < payload; ++i) {
        if (((uint8_t *)r.host_src)[i] != r.verify_buf[i]) {
            throw_runtime("verify failed: copied payload mismatch at byte " + std::to_string(i) +
                          " actual=" + std::to_string((uint32_t)((uint8_t *)r.host_src)[i]) +
                          " expected=" + std::to_string((uint32_t)r.verify_buf[i]));
        }
    }
    throw_runtime("verify failed: copied payload mismatch");
}

void run_benchmark(const Config &cfg)
{
    Resources r{};

    try {
        init_resources(cfg, r);

        std::vector<uint64_t> payloads = build_payload_list(cfg);

        std::cout << "mode=" << mode_to_string(cfg.mode)
                  << " direction=" << direction_to_string(cfg.direction)
                  << " device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " verify=" << (cfg.verify ? 1 : 0)
                  << "\n";
        std::cout << "payload_bytes,avg_latency_ns,bandwidth_GBps,total_cycles\n";

        for (uint64_t payload_u64 : payloads) {
            uint32_t payload = (uint32_t)payload_u64;
            if (cfg.direction == CopyDirection::H2D) {
                fill_src_pattern((uint8_t *)r.host_src, payload);
                if (cfg.verify) {
                    check_flexio(flexio_buf_dev_memset(r.process, 0, payload, r.dst_daddr),
                                 "flexio_buf_dev_memset(dst)");
                }
            } else {
                uint8_t *prep_src = (uint8_t *)r.host_src;
                if (cfg.verify) {
                    fill_src_pattern(r.verify_buf, payload);
                    prep_src = r.verify_buf;
                } else {
                    fill_src_pattern((uint8_t *)r.host_src, payload);
                }
                check_flexio(flexio_host2dev_memcpy(r.process, prep_src, payload, r.dst_daddr),
                             "flexio_host2dev_memcpy(d2h src)");
                std::memset(r.host_src, 0, payload);
            }

            uint64_t cycles = run_one_payload(cfg, r, payload);

            if (cfg.verify) {
                verify_payload(cfg, r, payload);
            }

            double total_ns = (double)cycles * 1000.0 / (double)cfg.clock_mhz;
            double avg_latency_ns = (cfg.iters == 0) ? 0.0 : total_ns / (double)cfg.iters;
            double bandwidth_gbps = (total_ns > 0.0)
                                        ? ((double)payload * (double)cfg.iters) / total_ns
                                        : 0.0;

            std::cout << payload << "," << std::fixed << std::setprecision(2) << avg_latency_ns << ","
                      << std::fixed << std::setprecision(3) << bandwidth_gbps << "," << cycles << "\n";
        }

        cleanup(r);
    } catch (...) {
        cleanup(r);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
