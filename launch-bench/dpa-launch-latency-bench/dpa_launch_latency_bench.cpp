#include <algorithm>
#include <cmath>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_sync_event.h>

#include "host/dpa_launch_latency_common.h"

extern "C" {
    doca_dpa_func_t dpa_rpc_delay;
    doca_dpa_func_t dpa_kernel_delay;
    doca_dpa_func_t dpa_kernel_event_trigger_timed;
    doca_dpa_func_t dpa_event_handler_thread;
}

extern struct doca_dpa_app *DPA_APP_NAME;

DOCA_LOG_REGISTER(DPA_LAUNCH_LATENCY_BENCH);

namespace {

constexpr uint64_t kSyncEventMask = UINT64_MAX;

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t warmup = 200;
    uint32_t iters = 2000;
    uint32_t dpa_clock_mhz = 1800;
    uint32_t event_timeout_us = 500000;
    std::vector<uint64_t> delay_cycles = {0, 1000, 10000, 100000, 500000, 1000000};
    bool run_rpc = true;
    bool run_kernel = true;
    bool run_event = true;
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

struct KernelLaunchState {
    SyncEventResource comp_event;
    SyncEventResource cycles_event;
    uint64_t seq = 0;
};

struct EventHandlerState {
    SyncEventResource comp_event;
    SyncEventResource launch_cycles_event;
    SyncEventResource inner_cycles_event;
    SyncEventResource trigger_comp_event;
    doca_dpa_thread *thread = nullptr;
    doca_dpa_notification_completion *notify_comp = nullptr;
    doca_dpa_dev_notification_completion_t notify_handle = 0;
    doca_dpa_dev_uintptr_t thread_ctx_dev_ptr = 0;
    dpa_event_thread_ctx thread_ctx{};
    uint64_t seq = 0;
};

struct Stats {
    double avg_launch_us = 0.0;
    double min_launch_us = 0.0;
    double p50_launch_us = 0.0;
    double p99_launch_us = 0.0;
    double max_launch_us = 0.0;
    double avg_inner_us = 0.0;
    double min_inner_us = 0.0;
    double p50_inner_us = 0.0;
    double p99_inner_us = 0.0;
    double max_inner_us = 0.0;
};

[[noreturn]] void throw_runtime(const std::string &msg)
{
    throw std::runtime_error(msg);
}

void check_doca(doca_error_t st, const char *what)
{
    if (st != DOCA_SUCCESS) {
        throw_runtime(std::string(what) + " failed: " + doca_error_get_descr(st));
    }
}

std::vector<uint64_t> parse_delay_list(const std::string &s)
{
    std::vector<uint64_t> values;
    std::stringstream ss(s);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }
        char *end = nullptr;
        uint64_t v = static_cast<uint64_t>(std::strtoull(token.c_str(), &end, 10));
        if (end == token.c_str() || *end != '\0') {
            throw_runtime("Invalid delay token: " + token);
        }
        values.push_back(v);
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
    bool run_kernel = false;
    bool run_event = false;

    std::stringstream ss(csv);
    std::string token;

    while (std::getline(ss, token, ',')) {
        if (token.empty()) {
            continue;
        }

        std::string mode;
        mode.reserve(token.size());
        for (char c : token) {
            mode.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        if (mode == "all") {
            run_rpc = true;
            run_kernel = true;
            run_event = true;
            break;
        }
        if (mode == "rpc") {
            run_rpc = true;
        } else if (mode == "kernel" || mode == "kernel_launch") {
            run_kernel = true;
        } else if (mode == "event" || mode == "event_handler") {
            run_event = true;
        } else {
            throw_runtime("Unknown mode: " + token + " (valid: rpc,kernel,event,all)");
        }
    }

    if (!run_rpc && !run_kernel && !run_event) {
        throw_runtime("--modes produced empty mode set");
    }

    cfg.run_rpc = run_rpc;
    cfg.run_kernel = run_kernel;
    cfg.run_event = run_event;
}

Config parse_args(int argc, char **argv)
{
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto require_value = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw_runtime(std::string("Missing value for ") + name);
            }
            return std::string(argv[++i]);
        };

        if (a == "--device" || a == "--device-name") {
            cfg.device_name = require_value("--device-name");
        } else if (a == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(std::stoul(require_value("--warmup")));
        } else if (a == "--iters") {
            cfg.iters = static_cast<uint32_t>(std::stoul(require_value("--iters")));
        } else if (a == "--clock-mhz") {
            cfg.dpa_clock_mhz = static_cast<uint32_t>(std::stoul(require_value("--clock-mhz")));
        } else if (a == "--event-timeout-us") {
            cfg.event_timeout_us = static_cast<uint32_t>(std::stoul(require_value("--event-timeout-us")));
        } else if (a == "--delay-cycles-list") {
            cfg.delay_cycles = parse_delay_list(require_value("--delay-cycles-list"));
        } else if (a == "--modes") {
            parse_modes(require_value("--modes"), cfg);
        } else if (a == "-h" || a == "--help") {
            std::cout
                << "dpa_launch_latency_bench options:\n"
                << "  --device-name <ibdev>        Device name (default: mlx5_0)\n"
                << "  --warmup <num>               Warmup iterations (default: 200)\n"
                << "  --iters <num>                Measured iterations (default: 2000)\n"
                << "  --clock-mhz <num>            DPA clock for cycles->us (default: 1800)\n"
                << "  --event-timeout-us <num>     Event handler wait timeout in us (default: 500000)\n"
                << "  --delay-cycles-list <csv>    Delay cycles list (default: 0,1000,10000,100000,500000,1000000)\n"
                << "  --modes <csv>                Modes: rpc,kernel,event,all (default: all)\n";
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + a);
        }
    }

    if (cfg.iters == 0) {
        throw_runtime("--iters must be > 0");
    }
    if (cfg.dpa_clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }

    return cfg;
}

std::string modes_to_string(const Config &cfg)
{
    std::vector<std::string> parts;
    if (cfg.run_rpc) {
        parts.emplace_back("rpc");
    }
    if (cfg.run_kernel) {
        parts.emplace_back("kernel");
    }
    if (cfg.run_event) {
        parts.emplace_back("event");
    }

    std::ostringstream os;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            os << ",";
        }
        os << parts[i];
    }
    return os.str();
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
        doca_error_t st = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
        if (st != DOCA_SUCCESS) {
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

void create_sync_event_dpa_to_cpu(const Resources &r, const bool need_dpa_handle, SyncEventResource &se)
{
    check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
    check_doca(doca_sync_event_add_publisher_location_dpa(se.event, r.dpa), "doca_sync_event_add_publisher_location_dpa");
    check_doca(doca_sync_event_add_subscriber_location_cpu(se.event, r.dev), "doca_sync_event_add_subscriber_location_cpu");
    check_doca(doca_sync_event_start(se.event), "doca_sync_event_start");

    if (need_dpa_handle) {
        check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle), "doca_sync_event_get_dpa_handle");
    }
}

void create_sync_event_cpu_to_dpa(const Resources &r, const bool need_dpa_handle, SyncEventResource &se)
{
    check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
    check_doca(doca_sync_event_add_publisher_location_cpu(se.event, r.dev), "doca_sync_event_add_publisher_location_cpu");
    check_doca(doca_sync_event_add_subscriber_location_dpa(se.event, r.dpa), "doca_sync_event_add_subscriber_location_dpa");
    check_doca(doca_sync_event_start(se.event), "doca_sync_event_start");

    if (need_dpa_handle) {
        check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle), "doca_sync_event_get_dpa_handle");
    }
}

void init_kernel_launch_state(const Resources &r, KernelLaunchState &st)
{
    create_sync_event_dpa_to_cpu(r, false, st.comp_event);
    try {
        create_sync_event_dpa_to_cpu(r, true, st.cycles_event);
    } catch (...) {
        destroy_sync_event(st.comp_event);
        throw;
    }
}

void cleanup_kernel_launch_state(KernelLaunchState &st)
{
    destroy_sync_event(st.cycles_event);
    destroy_sync_event(st.comp_event);
    st.seq = 0;
}

void init_event_handler_state(const Resources &r, EventHandlerState &st)
{
    create_sync_event_cpu_to_dpa(r, true, st.comp_event);
    try {
        create_sync_event_dpa_to_cpu(r, true, st.launch_cycles_event);
        create_sync_event_dpa_to_cpu(r, true, st.inner_cycles_event);
        create_sync_event_dpa_to_cpu(r, false, st.trigger_comp_event);

        check_doca(doca_dpa_mem_alloc(r.dpa, sizeof(st.thread_ctx), &st.thread_ctx_dev_ptr), "doca_dpa_mem_alloc(thread_ctx)");

        st.thread_ctx.completion_se = st.comp_event.dpa_handle;
        st.thread_ctx.event_cycles_se = st.inner_cycles_event.dpa_handle;
        st.thread_ctx.delay_cycles = 0;

        check_doca(doca_dpa_h2d_memcpy(r.dpa, st.thread_ctx_dev_ptr, &st.thread_ctx, sizeof(st.thread_ctx)),
                   "doca_dpa_h2d_memcpy(thread_ctx)");

        check_doca(doca_dpa_thread_create(r.dpa, &st.thread), "doca_dpa_thread_create");
        check_doca(doca_dpa_thread_set_func_arg(st.thread, &dpa_event_handler_thread, 0), "doca_dpa_thread_set_func_arg");
        check_doca(doca_dpa_thread_set_local_storage(st.thread, st.thread_ctx_dev_ptr), "doca_dpa_thread_set_local_storage");
        check_doca(doca_dpa_thread_start(st.thread), "doca_dpa_thread_start");

        check_doca(doca_dpa_notification_completion_create(r.dpa, st.thread, &st.notify_comp),
                   "doca_dpa_notification_completion_create");
        check_doca(doca_dpa_notification_completion_start(st.notify_comp), "doca_dpa_notification_completion_start");
        check_doca(doca_dpa_notification_completion_get_dpa_handle(st.notify_comp, &st.notify_handle),
                   "doca_dpa_notification_completion_get_dpa_handle");
        check_doca(doca_dpa_thread_run(st.thread), "doca_dpa_thread_run");
    } catch (...) {
        if (st.notify_comp != nullptr) {
            (void)doca_dpa_notification_completion_stop(st.notify_comp);
            (void)doca_dpa_notification_completion_destroy(st.notify_comp);
            st.notify_comp = nullptr;
        }
        if (st.thread != nullptr) {
            (void)doca_dpa_thread_stop(st.thread);
            (void)doca_dpa_thread_destroy(st.thread);
            st.thread = nullptr;
        }
        if (st.thread_ctx_dev_ptr != 0) {
            (void)doca_dpa_mem_free(r.dpa, st.thread_ctx_dev_ptr);
            st.thread_ctx_dev_ptr = 0;
        }
        destroy_sync_event(st.trigger_comp_event);
        destroy_sync_event(st.inner_cycles_event);
        destroy_sync_event(st.launch_cycles_event);
        destroy_sync_event(st.comp_event);
        throw;
    }
}

void cleanup_event_handler_state(const Resources &r, EventHandlerState &st)
{
    if (st.notify_comp != nullptr) {
        (void)doca_dpa_notification_completion_stop(st.notify_comp);
        (void)doca_dpa_notification_completion_destroy(st.notify_comp);
        st.notify_comp = nullptr;
    }

    if (st.thread != nullptr) {
        (void)doca_dpa_thread_stop(st.thread);
        (void)doca_dpa_thread_destroy(st.thread);
        st.thread = nullptr;
    }

    if (st.thread_ctx_dev_ptr != 0) {
        (void)doca_dpa_mem_free(r.dpa, st.thread_ctx_dev_ptr);
        st.thread_ctx_dev_ptr = 0;
    }

    destroy_sync_event(st.trigger_comp_event);
    destroy_sync_event(st.inner_cycles_event);
    destroy_sync_event(st.launch_cycles_event);
    destroy_sync_event(st.comp_event);
    st.notify_handle = 0;
    st.seq = 0;
}

size_t percentile_index(const size_t n, const double p)
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

Stats analyze_samples(std::vector<double> launch_us, std::vector<double> inner_us)
{
    if (launch_us.empty() || inner_us.empty() || launch_us.size() != inner_us.size()) {
        return {};
    }

    Stats s{};

    std::sort(launch_us.begin(), launch_us.end());
    std::sort(inner_us.begin(), inner_us.end());

    const double launch_sum = std::accumulate(launch_us.begin(), launch_us.end(), 0.0);
    const double inner_sum = std::accumulate(inner_us.begin(), inner_us.end(), 0.0);

    s.avg_launch_us = launch_sum / static_cast<double>(launch_us.size());
    s.min_launch_us = launch_us.front();
    s.p50_launch_us = launch_us[percentile_index(launch_us.size(), 0.50)];
    s.p99_launch_us = launch_us[percentile_index(launch_us.size(), 0.99)];
    s.max_launch_us = launch_us.back();

    s.avg_inner_us = inner_sum / static_cast<double>(inner_us.size());
    s.min_inner_us = inner_us.front();
    s.p50_inner_us = inner_us[percentile_index(inner_us.size(), 0.50)];
    s.p99_inner_us = inner_us[percentile_index(inner_us.size(), 0.99)];
    s.max_inner_us = inner_us.back();
    return s;
}

template <typename InvokeFn>
Stats run_iterations_host_launch(const Config &cfg, InvokeFn &&invoke_once)
{
    std::vector<double> launch_us;
    std::vector<double> inner_us;
    launch_us.reserve(cfg.iters);
    inner_us.reserve(cfg.iters);

    for (uint32_t i = 0; i < cfg.warmup + cfg.iters; ++i) {
        uint64_t inner_cycles = 0;
        const auto t0 = std::chrono::steady_clock::now();
        invoke_once(inner_cycles);
        const auto t1 = std::chrono::steady_clock::now();

        if (i >= cfg.warmup) {
            launch_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            inner_us.push_back(static_cast<double>(inner_cycles) / static_cast<double>(cfg.dpa_clock_mhz));
        }
    }

    return analyze_samples(std::move(launch_us), std::move(inner_us));
}

template <typename InvokeFn>
Stats run_iterations_device_launch(const Config &cfg, InvokeFn &&invoke_once)
{
    std::vector<double> launch_us;
    std::vector<double> inner_us;
    launch_us.reserve(cfg.iters);
    inner_us.reserve(cfg.iters);

    for (uint32_t i = 0; i < cfg.warmup + cfg.iters; ++i) {
        uint64_t launch_cycles = 0;
        uint64_t inner_cycles = 0;
        invoke_once(launch_cycles, inner_cycles);

        if (i >= cfg.warmup) {
            launch_us.push_back(static_cast<double>(launch_cycles) / static_cast<double>(cfg.dpa_clock_mhz));
            inner_us.push_back(static_cast<double>(inner_cycles) / static_cast<double>(cfg.dpa_clock_mhz));
        }
    }

    return analyze_samples(std::move(launch_us), std::move(inner_us));
}

Stats run_rpc_mode(const Config &cfg, const Resources &r, const uint64_t delay_cycles)
{
    return run_iterations_host_launch(cfg, [&](uint64_t &inner_cycles) {
        check_doca(doca_dpa_rpc(r.dpa, &dpa_rpc_delay, &inner_cycles, r.dpa_handle, delay_cycles),
                   "doca_dpa_rpc(dpa_rpc_delay)");
    });
}

Stats run_kernel_launch_mode(const Config &cfg, const Resources &r, KernelLaunchState &st, const uint64_t delay_cycles)
{
    return run_iterations_host_launch(cfg, [&](uint64_t &inner_cycles) {
        st.seq++;

        check_doca(doca_dpa_kernel_launch_update_set(r.dpa,
                                                     nullptr,
                                                     0,
                                                     st.comp_event.event,
                                                     st.seq,
                                                     1,
                                                     &dpa_kernel_delay,
                                                     r.dpa_handle,
                                                     st.cycles_event.dpa_handle,
                                                     delay_cycles),
                   "doca_dpa_kernel_launch_update_set(dpa_kernel_delay)");

        check_doca(doca_sync_event_wait_gt(st.comp_event.event, st.seq - 1, kSyncEventMask), "doca_sync_event_wait_gt(kernel)");
        check_doca(doca_sync_event_get(st.cycles_event.event, &inner_cycles), "doca_sync_event_get(kernel_cycles)");
    });
}

void wait_sync_event_gt_with_timeout(const char *what,
                                     doca_sync_event *event,
                                     const uint64_t threshold,
                                     const uint64_t timeout_us)
{
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t value = 0;

    while (true) {
        check_doca(doca_sync_event_get(event, &value), "doca_sync_event_get(timeout_poll)");
        if ((value & kSyncEventMask) > (threshold & kSyncEventMask)) {
            return;
        }

        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - t0)
                                    .count();
        if (elapsed_us > static_cast<long long>(timeout_us)) {
            throw_runtime(std::string(what) + " timed out");
        }
    }
}

void update_event_thread_delay(const Resources &r, EventHandlerState &st, const uint64_t delay_cycles)
{
    st.thread_ctx.delay_cycles = delay_cycles;
    check_doca(doca_dpa_h2d_memcpy(r.dpa, st.thread_ctx_dev_ptr, &st.thread_ctx, sizeof(st.thread_ctx)),
               "doca_dpa_h2d_memcpy(update_thread_ctx)");
}

Stats run_event_handler_mode(const Config &cfg, const Resources &r, EventHandlerState &st, const uint64_t delay_cycles)
{
    update_event_thread_delay(r, st, delay_cycles);
    return run_iterations_device_launch(cfg, [&](uint64_t &launch_cycles, uint64_t &inner_cycles) {
        st.seq++;

        check_doca(doca_dpa_kernel_launch_update_set(r.dpa,
                                                     nullptr,
                                                     0,
                                                     st.trigger_comp_event.event,
                                                     st.seq,
                                                     1,
                                                     &dpa_kernel_event_trigger_timed,
                                                     r.dpa_handle,
                                                     st.notify_handle,
                                                     st.comp_event.dpa_handle,
                                                     st.launch_cycles_event.dpa_handle),
                   "doca_dpa_kernel_launch_update_set(dpa_kernel_event_trigger_timed)");

        wait_sync_event_gt_with_timeout("event trigger kernel completion",
                                        st.trigger_comp_event.event,
                                        st.seq - 1,
                                        cfg.event_timeout_us);

        check_doca(doca_sync_event_get(st.launch_cycles_event.event, &launch_cycles),
                   "doca_sync_event_get(event_launch_cycles)");
        check_doca(doca_sync_event_get(st.inner_cycles_event.event, &inner_cycles),
                   "doca_sync_event_get(event_inner_cycles)");
    });
}

void print_header()
{
    std::cout
        << "mode,launch_scope,inner_scope,delay_cycles,avg_launch_us,min_launch_us,p50_launch_us,p99_launch_us,max_launch_us,avg_inner_us,min_inner_us,p50_inner_us,p99_inner_us,max_inner_us,delta_launch_vs_mode_base_us,delta_inner_vs_mode_base_us\n"
        << std::flush;
}

void print_stats_row(const std::string &mode,
                     const std::string &launch_scope,
                     const std::string &inner_scope,
                     const uint64_t delay_cycles,
                     const Stats &stats,
                     const double base_launch_us,
                     const double base_inner_us)
{
    std::cout << std::fixed << std::setprecision(3)
              << mode
              << "," << launch_scope
              << "," << inner_scope
              << "," << delay_cycles
              << "," << stats.avg_launch_us
              << "," << stats.min_launch_us
              << "," << stats.p50_launch_us
              << "," << stats.p99_launch_us
              << "," << stats.max_launch_us
              << "," << stats.avg_inner_us
              << "," << stats.min_inner_us
              << "," << stats.p50_inner_us
              << "," << stats.p99_inner_us
              << "," << stats.max_inner_us
              << "," << (stats.avg_launch_us - base_launch_us)
              << "," << (stats.avg_inner_us - base_inner_us)
              << "\n"
              << std::flush;
}

std::vector<uint64_t> get_non_zero_delays(const std::vector<uint64_t> &delays)
{
    std::vector<uint64_t> ret;
    ret.reserve(delays.size());

    for (uint64_t d : delays) {
        if (d != 0) {
            ret.push_back(d);
        }
    }

    return ret;
}

void run_rpc_benchmark(const Config &cfg, const Resources &r)
{
    const Stats base = run_rpc_mode(cfg, r, 0);
    print_stats_row("rpc_delay",
                    "host_call_to_return",
                    "rpc_entry_to_return",
                    0,
                    base,
                    base.avg_launch_us,
                    base.avg_inner_us);

    for (const uint64_t delay : get_non_zero_delays(cfg.delay_cycles)) {
        const Stats s = run_rpc_mode(cfg, r, delay);
        print_stats_row("rpc_delay",
                        "host_call_to_return",
                        "rpc_entry_to_return",
                        delay,
                        s,
                        base.avg_launch_us,
                        base.avg_inner_us);
    }
}

void run_kernel_launch_benchmark(const Config &cfg, const Resources &r, KernelLaunchState &st)
{
    const Stats base = run_kernel_launch_mode(cfg, r, st, 0);
    print_stats_row("kernel_launch",
                    "host_launch_to_completion",
                    "kernel_entry_to_exit",
                    0,
                    base,
                    base.avg_launch_us,
                    base.avg_inner_us);

    for (const uint64_t delay : get_non_zero_delays(cfg.delay_cycles)) {
        const Stats s = run_kernel_launch_mode(cfg, r, st, delay);
        print_stats_row("kernel_launch",
                        "host_launch_to_completion",
                        "kernel_entry_to_exit",
                        delay,
                        s,
                        base.avg_launch_us,
                        base.avg_inner_us);
    }
}

void run_event_handler_benchmark(const Config &cfg, const Resources &r, EventHandlerState &st)
{
    const Stats base = run_event_handler_mode(cfg, r, st, 0);
    print_stats_row("event_handler_rtt_kernel_trigger",
                    "kernel_notify_to_event_done",
                    "event_handler_entry_to_exit",
                    0,
                    base,
                    base.avg_launch_us,
                    base.avg_inner_us);

    for (const uint64_t delay : get_non_zero_delays(cfg.delay_cycles)) {
        const Stats s = run_event_handler_mode(cfg, r, st, delay);
        print_stats_row("event_handler_rtt_kernel_trigger",
                        "kernel_notify_to_event_done",
                        "event_handler_entry_to_exit",
                        delay,
                        s,
                        base.avg_launch_us,
                        base.avg_inner_us);
    }
}

void run_benchmark(const Config &cfg)
{
    Resources r{};
    KernelLaunchState kernel_state{};
    EventHandlerState event_state{};

    try {
        init_resources(cfg, r);

        if (cfg.run_kernel) {
            init_kernel_launch_state(r, kernel_state);
        }
        if (cfg.run_event) {
            init_event_handler_state(r, event_state);
        }

        std::cout << "device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " dpa_clock_mhz=" << cfg.dpa_clock_mhz
                  << " modes=" << modes_to_string(cfg)
                  << "\n"
                  << std::flush;

        print_header();

        if (cfg.run_rpc) {
            run_rpc_benchmark(cfg, r);
        }
        if (cfg.run_kernel) {
            run_kernel_launch_benchmark(cfg, r, kernel_state);
        }
        if (cfg.run_event) {
            try {
                run_event_handler_benchmark(cfg, r, event_state);
            } catch (const std::exception &e) {
                if (!cfg.run_rpc && !cfg.run_kernel) {
                    throw;
                }
                std::cerr << "WARNING: event handler benchmark failed and was skipped: " << e.what() << "\n";
            }
        }

        cleanup_event_handler_state(r, event_state);
        cleanup_kernel_launch_state(kernel_state);
        cleanup_resources(r);
    } catch (...) {
        cleanup_event_handler_state(r, event_state);
        cleanup_kernel_launch_state(kernel_state);
        cleanup_resources(r);
        throw;
    }
}

} // namespace

int main(int argc, char **argv)
{
    try {
        init_doca_log();
        Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
