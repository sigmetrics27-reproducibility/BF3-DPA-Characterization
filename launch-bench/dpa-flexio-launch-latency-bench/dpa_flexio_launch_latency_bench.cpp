#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <infiniband/verbs.h>

#include <libflexio/flexio_ver.h>
#define FLEXIO_VER_USED FLEXIO_VER(25, 10, 0)
#include <libflexio/flexio.h>

#include "host/flexio_launch_latency_common.h"

extern "C" {
flexio_func_t flexio_latency_rpc_empty;
flexio_func_t flexio_latency_rpc_delay;
flexio_func_t flexio_latency_event_handler;
flexio_func_t flexio_latency_rpc_trigger_event;
}

extern struct flexio_app *DPA_APP_NAME;

namespace {

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t warmup = 200;
    uint32_t iters = 2000;
    uint32_t clock_mhz = FLEXIO_LAUNCH_LATENCY_CLOCK_MHZ_DEFAULT;
    std::vector<uint64_t> delay_cycles = {0, 1000, 10000, 100000, 500000, 1000000};
    bool run_rpc_mode = true;
    bool run_rpc_event_mode = true;
};

struct Resources {
    ibv_context *ibv_ctx = nullptr;
    flexio_process *process = nullptr;

    flexio_event_handler *event_handler = nullptr;
    flexio_uintptr_t event_ctx_daddr = 0;
    flexio_event_activation_ctx event_ctx{};
};

struct DistStats {
    double avg = 0.0;
    double min = 0.0;
    double p50 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
};

struct Stats {
    DistStats launch_us{};
    DistStats inner_us{};
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

std::vector<uint64_t> parse_delay_list(const std::string &csv)
{
    std::vector<uint64_t> values;
    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        values.push_back(parse_u64(token, "--delay-cycles-list"));
    }

    if (values.empty()) {
        throw_runtime("--delay-cycles-list produced empty list");
    }

    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

void parse_modes(const std::string &csv, Config &cfg)
{
    bool run_rpc = false;
    bool run_rpc_event = false;

    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        if (token == "all") {
            run_rpc = true;
            run_rpc_event = true;
            break;
        }
        if (token == "rpc") {
            run_rpc = true;
        } else if (token == "event" || token == "rpc_event" || token == "event_activation") {
            run_rpc_event = true;
        } else {
            throw_runtime("Unknown mode token: " + token + " (valid: rpc,event,all)");
        }
    }

    if (!run_rpc && !run_rpc_event) {
        throw_runtime("--modes produced empty mode set");
    }

    cfg.run_rpc_mode = run_rpc;
    cfg.run_rpc_event_mode = run_rpc_event;
}

std::string modes_to_string(const Config &cfg)
{
    std::ostringstream os;
    bool first = true;
    if (cfg.run_rpc_mode) {
        os << "rpc";
        first = false;
    }
    if (cfg.run_rpc_event_mode) {
        if (!first) {
            os << ",";
        }
        os << "event";
    }
    return os.str();
}

void print_usage(const char *prog)
{
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --device mlx5_0 [options]\n\n"
        << "Options:\n"
        << "  --device <name>                 IB device name (default: mlx5_0)\n"
        << "  --warmup <num>                  Warmup iterations per delay (default: 200)\n"
        << "  --iters <num>                   Measured iterations per delay (default: 2000)\n"
        << "  --clock-mhz <num>               DPA clock MHz for cycles->us conversion (default: 1800)\n"
        << "  --delay-cycles-list <csv>       Delay cycles list (default: 0,1000,10000,100000,500000,1000000)\n"
        << "  --modes <csv>                   Modes: rpc,event,all (default: all; runs both)\n";
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

        if (arg == "--device" || arg == "--device-name") {
            cfg.device_name = next("--device");
        } else if (arg == "--warmup") {
            cfg.warmup = (uint32_t)parse_u64(next("--warmup"), "--warmup");
        } else if (arg == "--iters") {
            cfg.iters = (uint32_t)parse_u64(next("--iters"), "--iters");
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = (uint32_t)parse_u64(next("--clock-mhz"), "--clock-mhz");
        } else if (arg == "--delay-cycles-list") {
            cfg.delay_cycles = parse_delay_list(next("--delay-cycles-list"));
        } else if (arg == "--modes") {
            parse_modes(next("--modes"), cfg);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }

    if (cfg.iters == 0) {
        throw_runtime("--iters must be > 0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }

    return cfg;
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

void cleanup(Resources &r)
{
    if (r.event_handler != nullptr) {
        (void)flexio_event_handler_destroy(r.event_handler);
        r.event_handler = nullptr;
    }

    if (r.event_ctx_daddr != 0 && r.process != nullptr) {
        (void)flexio_buf_dev_free(r.process, r.event_ctx_daddr);
        r.event_ctx_daddr = 0;
    }

    if (r.process != nullptr) {
        (void)flexio_process_destroy(r.process);
        r.process = nullptr;
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
}

void init_rpc_event_resources(Resources &r)
{
    check_flexio(flexio_buf_dev_alloc(r.process,
                                      sizeof(struct flexio_event_activation_ctx),
                                      &r.event_ctx_daddr),
                 "flexio_buf_dev_alloc(event_ctx)");

    std::memset(&r.event_ctx, 0, sizeof(r.event_ctx));

    struct flexio_event_handler_attr eh_attr = {};
    eh_attr.host_stub_func = &flexio_latency_event_handler;
    eh_attr.affinity.type = FLEXIO_AFFINITY_NONE;
    check_flexio(flexio_event_handler_create(r.process, &eh_attr, &r.event_handler),
                 "flexio_event_handler_create");

    const uint32_t activation_id = flexio_event_handler_get_activation_id(r.event_handler);
    if (activation_id == UINT32_MAX) {
        throw_runtime("flexio_event_handler_get_activation_id failed");
    }

    r.event_ctx.activation_id = activation_id;
    check_flexio(flexio_host2dev_memcpy(r.process,
                                        &r.event_ctx,
                                        sizeof(r.event_ctx),
                                        r.event_ctx_daddr),
                 "flexio_host2dev_memcpy(event_ctx init)");

    check_flexio(flexio_event_handler_run(r.event_handler, r.event_ctx_daddr), "flexio_event_handler_run");
}

size_t percentile_index(size_t n, double p)
{
    if (n == 0) {
        return 0;
    }
    const double rank = std::ceil(p * static_cast<double>(n));
    size_t idx = (rank <= 1.0) ? 0 : static_cast<size_t>(rank - 1.0);
    if (idx >= n) {
        idx = n - 1;
    }
    return idx;
}

DistStats summarize_dist(const std::vector<double> &samples)
{
    if (samples.empty()) {
        throw_runtime("Invalid empty sample vector while summarizing");
    }

    DistStats s{};
    std::vector<double> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    s.min = sorted.front();
    s.p50 = sorted[percentile_index(sorted.size(), 0.50)];
    s.p99 = sorted[percentile_index(sorted.size(), 0.99)];
    s.max = sorted.back();

    const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    s.avg = sum / static_cast<double>(samples.size());
    return s;
}

struct flexio_event_activation_ctx fetch_event_ctx_from_dpa(const Resources &r)
{
    struct flexio_event_activation_ctx out{};

    struct flexio_mem src{};
    src.mem_type = FLEXIO_MEMTYPE_DPA;
    src.daddr = r.event_ctx_daddr;

    struct flexio_mem dst{};
    dst.mem_type = FLEXIO_MEMTYPE_HOST;
    dst.haddr = (uintptr_t)&out;

    check_flexio(flexio_memcpy(r.process, &src, &dst, sizeof(out)), "flexio_memcpy(event ctx dpa->host)");
    return out;
}

Stats run_rpc_delay_case(const Config &cfg, const Resources &r, uint64_t delay_cycles)
{
    const uint32_t total_iters = cfg.warmup + cfg.iters;

    std::vector<double> launch_us;
    std::vector<double> inner_us;
    launch_us.reserve(cfg.iters);
    inner_us.reserve(cfg.iters);

    flexio_func_t *fn = (delay_cycles == 0) ? &flexio_latency_rpc_empty : &flexio_latency_rpc_delay;
    const flexio_uintptr_t arg = static_cast<flexio_uintptr_t>(delay_cycles);

    for (uint32_t i = 0; i < total_iters; ++i) {
        uint64_t rpc_ret = 0;

        const auto t0 = std::chrono::steady_clock::now();
        check_flexio(flexio_process_call(r.process, fn, &rpc_ret, arg), "flexio_process_call");
        const auto t1 = std::chrono::steady_clock::now();

        if (i >= cfg.warmup) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            launch_us.push_back(static_cast<double>(ns) / 1000.0);
            inner_us.push_back(static_cast<double>(rpc_ret) / static_cast<double>(cfg.clock_mhz));
        }
    }

    Stats out{};
    out.launch_us = summarize_dist(launch_us);
    out.inner_us = summarize_dist(inner_us);
    return out;
}

Stats run_rpc_event_case(const Config &cfg, Resources &r, uint64_t delay_cycles)
{
    const uint32_t total_iters = cfg.warmup + cfg.iters;

    r.event_ctx.delay_cycles = delay_cycles;
    check_flexio(flexio_host2dev_memcpy(r.process,
                                        &r.event_ctx,
                                        sizeof(r.event_ctx),
                                        r.event_ctx_daddr),
                 "flexio_host2dev_memcpy(event_ctx update)");

    std::vector<double> launch_us;
    std::vector<double> inner_us;
    launch_us.reserve(cfg.iters);
    inner_us.reserve(cfg.iters);

    for (uint32_t i = 0; i < total_iters; ++i) {
        uint64_t rpc_ret = 0;
        check_flexio(flexio_process_call(r.process,
                                         &flexio_latency_rpc_trigger_event,
                                         &rpc_ret,
                                         r.event_ctx_daddr),
                     "flexio_process_call(flexio_latency_rpc_trigger_event)");
        const struct flexio_event_activation_ctx post = fetch_event_ctx_from_dpa(r);

        if (i >= cfg.warmup) {
            launch_us.push_back(static_cast<double>(rpc_ret) / static_cast<double>(cfg.clock_mhz));
            inner_us.push_back(static_cast<double>(post.last_event_cycles) / static_cast<double>(cfg.clock_mhz));
        }
    }

    Stats out{};
    out.launch_us = summarize_dist(launch_us);
    out.inner_us = summarize_dist(inner_us);
    return out;
}

void print_csv_header()
{
    std::cout << "bench,device,mode,launch_scope,inner_scope,delay_cycles,warmup,iters,clock_mhz,"
                 "avg_launch_us,min_launch_us,p50_launch_us,p99_launch_us,max_launch_us,"
                 "avg_inner_us,min_inner_us,p50_inner_us,p99_inner_us,max_inner_us\n"
              << std::flush;
}

void print_csv_row(const Config &cfg,
                   const char *mode,
                   const char *launch_scope,
                   const char *inner_scope,
                   uint64_t delay_cycles,
                   const Stats &s)
{
    std::cout << "flexio_launch_latency" << ","
              << cfg.device_name << ","
              << mode << ","
              << launch_scope << ","
              << inner_scope << ","
              << delay_cycles << ","
              << cfg.warmup << ","
              << cfg.iters << ","
              << cfg.clock_mhz << ","
              << std::fixed << std::setprecision(3)
              << s.launch_us.avg << ","
              << s.launch_us.min << ","
              << s.launch_us.p50 << ","
              << s.launch_us.p99 << ","
              << s.launch_us.max << ","
              << s.inner_us.avg << ","
              << s.inner_us.min << ","
              << s.inner_us.p50 << ","
              << s.inner_us.p99 << ","
              << s.inner_us.max
              << "\n"
              << std::flush;
}

void run_benchmark(const Config &cfg)
{
    Resources r{};

    try {
        init_resources(cfg, r);
        if (cfg.run_rpc_event_mode) {
            init_rpc_event_resources(r);
        }

        std::cout << "bench=flexio_launch_latency"
                  << " device=" << cfg.device_name
                  << " modes=" << modes_to_string(cfg)
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " clock_mhz=" << cfg.clock_mhz
                  << "\n"
                  << std::flush;
        print_csv_header();

        if (cfg.run_rpc_mode) {
            for (uint64_t delay : cfg.delay_cycles) {
                const Stats s = run_rpc_delay_case(cfg, r, delay);
                print_csv_row(cfg,
                              "rpc_call",
                              "host_call_to_return",
                              "rpc_entry_to_return",
                              delay,
                              s);
            }
        }

        if (cfg.run_rpc_event_mode) {
            for (uint64_t delay : cfg.delay_cycles) {
                const Stats s = run_rpc_event_case(cfg, r, delay);
                print_csv_row(cfg,
                              "rpc_activate_event",
                              "rpc_activate_to_event_done",
                              "event_thread_entry_to_exit",
                              delay,
                              s);
            }
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
        const Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
