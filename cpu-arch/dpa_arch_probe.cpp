// check thread counters with single-thread, scaling and cross-core runs

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_sync_event.h>

#include "host/dpa_arch_probe_common.h"

extern "C" {
extern doca_dpa_func_t k_mem_random;
extern doca_dpa_func_t k_mem_stream;
extern doca_dpa_func_t k_compute;
extern doca_dpa_func_t k_kickoff;
}

extern struct doca_dpa_app *DPA_APP_NAME;

DOCA_LOG_REGISTER(DPA_ARCH_PROBE);

namespace {

const uint64_t kSeed = 0x243f6a8885a308d3ULL;
constexpr uint64_t kDataEntries = 1ULL << 16;

struct Config {
    std::string device_name = "mlx5_0";
    uint64_t M = 30000;
    uint32_t max_eus = 64;
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;
};

// one result channel per thread
struct SyncEv {
    doca_sync_event *event = nullptr;
    doca_dpa_dev_sync_event_t dpa_handle = 0;
};

// pinned thread and its notification handle
struct PinnedThread {
    doca_dpa_thread *thread = nullptr;
    doca_dpa_eu_affinity *aff = nullptr;
    doca_dpa_notification_completion *nc = nullptr;
    doca_dpa_dev_notification_completion_t nc_handle = 0;
    doca_dpa_dev_uintptr_t cfg_dev = 0;
};

// per-thread counters from run_set
struct Result {
    std::vector<uint64_t> cyc;
    std::vector<uint64_t> time;
    std::vector<uint64_t> inst;
};

enum class WorkloadKind {
    RandomMem,
    StreamMem,
    Compute,
};

struct Workload {
    const char *key;
    const char *label;
    doca_dpa_func_t *fn;
    WorkloadKind kind;
    uint64_t stride;
};

struct DpaBuffer {
    doca_dpa_dev_uintptr_t addr = 0;
    uint64_t mask = 0;
};

const Workload kWorkloads[] = {
    {"random_mem", "Random memory", &k_mem_random, WorkloadKind::RandomMem, 1},
    {"stream_mem", "Sequential memory", &k_mem_stream, WorkloadKind::StreamMem, 1},
    {"compute", "Compute", &k_compute, WorkloadKind::Compute, 0},
};

[[noreturn]] void die(const std::string &m) { throw std::runtime_error(m); }
void check(doca_error_t st, const char *what)
{
    if (st != DOCA_SUCCESS) {
        die(std::string(what) + " failed: " + doca_error_get_descr(st));
    }
}

void init_log()
{
    check(doca_log_backend_create_standard(), "doca_log_backend_create_standard");
    struct doca_log_backend *sdk_log = nullptr;
    check(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
    check(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING), "doca_log_backend_set_sdk_level");
}

void open_device(const std::string &name, doca_dev **dev)
{
    struct doca_devinfo **list = nullptr;
    uint32_t n = 0;
    check(doca_devinfo_create_list(&list, &n), "doca_devinfo_create_list");
    char ibdev[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (doca_devinfo_get_ibdev_name(list[i], ibdev, DOCA_DEVINFO_IBDEV_NAME_SIZE) != DOCA_SUCCESS) {
            continue;
        }
        if (name == ibdev) {
            check(doca_dev_open(list[i], dev), "doca_dev_open");
            found = true;
            break;
        }
    }
    check(doca_devinfo_destroy_list(list), "doca_devinfo_destroy_list");
    if (!found || *dev == nullptr) {
        die("IB device not found: " + name);
    }
}

void create_ev(const Resources &r, SyncEv &se)
{
    check(doca_sync_event_create(&se.event), "sync_event_create");
    check(doca_sync_event_add_publisher_location_dpa(se.event, r.dpa), "add_publisher_location_dpa");
    check(doca_sync_event_add_subscriber_location_cpu(se.event, r.dev), "add_subscriber_location_cpu");
    check(doca_sync_event_start(se.event), "sync_event_start");
    check(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle), "sync_event_get_dpa_handle");
}

void destroy_ev(SyncEv &se)
{
    if (se.event) {
        (void)doca_sync_event_stop(se.event);
        (void)doca_sync_event_destroy(se.event);
        se.event = nullptr;
        se.dpa_handle = 0;
    }
}

uint64_t read_ev(const SyncEv &se)
{
    uint64_t v = 0;
    check(doca_sync_event_get(se.event, &v), "sync_event_get");
    return v;
}

void create_pinned(const Resources &r, uint32_t eu, const dpa_aff_cfg &cfg, PinnedThread &pt,
                   doca_dpa_func_t *fn)
{
    check(doca_dpa_mem_alloc(r.dpa, sizeof(cfg), &pt.cfg_dev), "mem_alloc(cfg)");
    check(doca_dpa_h2d_memcpy(r.dpa, pt.cfg_dev, const_cast<dpa_aff_cfg *>(&cfg), sizeof(cfg)), "h2d(cfg)");
    check(doca_dpa_eu_affinity_create(r.dpa, &pt.aff), "eu_affinity_create");
    check(doca_dpa_eu_affinity_set(pt.aff, eu), "eu_affinity_set");
    check(doca_dpa_thread_create(r.dpa, &pt.thread), "thread_create");
    check(doca_dpa_thread_set_func_arg(pt.thread, fn, pt.cfg_dev), "set_func_arg");
    check(doca_dpa_thread_set_affinity(pt.thread, pt.aff), "set_affinity");
    check(doca_dpa_thread_start(pt.thread), "thread_start");
    check(doca_dpa_notification_completion_create(r.dpa, pt.thread, &pt.nc), "notify_create");
    check(doca_dpa_notification_completion_start(pt.nc), "notify_start");
    check(doca_dpa_notification_completion_get_dpa_handle(pt.nc, &pt.nc_handle), "notify_get_handle");
    check(doca_dpa_thread_run(pt.thread), "thread_run");
}


void destroy_pinned(const Resources &r, PinnedThread &pt)
{
    if (pt.nc != nullptr) {
        (void)doca_dpa_notification_completion_stop(pt.nc);
        (void)doca_dpa_notification_completion_destroy(pt.nc);
        pt.nc = nullptr;
    }
    if (pt.thread != nullptr) {
        (void)doca_dpa_thread_stop(pt.thread);
        (void)doca_dpa_thread_destroy(pt.thread);
        pt.thread = nullptr;
    }
    if (pt.aff != nullptr) {
        (void)doca_dpa_eu_affinity_destroy(pt.aff);
        pt.aff = nullptr;
    }
    if (pt.cfg_dev != 0) {
        (void)doca_dpa_mem_free(r.dpa, pt.cfg_dev);
        pt.cfg_dev = 0;
    }
}

// notify all pinned threads with one RPC
void kickoff(const Resources &r, std::vector<PinnedThread> &pts)
{
    std::vector<uint64_t> handles;
    handles.reserve(pts.size());
    for (auto &p : pts) {
        handles.push_back(static_cast<uint64_t>(p.nc_handle));
    }
    doca_dpa_dev_uintptr_t buf = 0;
    check(doca_dpa_mem_alloc(r.dpa, handles.size() * sizeof(uint64_t), &buf), "mem_alloc(handles)");
    check(doca_dpa_h2d_memcpy(r.dpa, buf, handles.data(), handles.size() * sizeof(uint64_t)), "h2d(handles)");
    uint64_t ret = 0;
    check(doca_dpa_rpc(r.dpa, &k_kickoff, &ret, static_cast<uint64_t>(r.dpa_handle),
                       static_cast<uint64_t>(buf), static_cast<uint32_t>(pts.size())), "rpc(kickoff)");
    (void)doca_dpa_mem_free(r.dpa, buf);
}

void reset_dpa(Resources &r)
{
    if (r.dpa != nullptr) {
        (void)doca_dpa_stop(r.dpa);
        (void)doca_dpa_destroy(r.dpa);
        r.dpa = nullptr;
        r.dpa_handle = 0;
    }
    check(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create(reset)");
    check(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app(reset)");
    check(doca_dpa_start(r.dpa), "doca_dpa_start(reset)");
    check(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "get_dpa_handle(reset)");
}

DpaBuffer create_workload_buffer(const Resources &r, const Workload &w)
{
    DpaBuffer out;
    if (w.kind == WorkloadKind::Compute) {
        return out;
    }

    std::vector<uint64_t> data(kDataEntries);
    if (w.kind == WorkloadKind::RandomMem) {
        std::vector<uint64_t> perm(kDataEntries);
        std::iota(perm.begin(), perm.end(), 0);
        std::mt19937_64 rng(kSeed);
        std::shuffle(perm.begin(), perm.end(), rng);
        for (size_t i = 0; i < perm.size(); ++i) {
            data[perm[i]] = perm[(i + 1) & (perm.size() - 1)];
        }
    } else {
        uint64_t x = kSeed;
        for (size_t i = 0; i < data.size(); ++i) {
            x += 0x9e3779b97f4a7c15ULL;
            uint64_t z = x;
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
            data[i] = z ^ (z >> 31);
        }
    }

    out.mask = kDataEntries - 1;
    check(doca_dpa_mem_alloc(r.dpa, data.size() * sizeof(data[0]), &out.addr), "mem_alloc(workload)");
    try {
        check(doca_dpa_h2d_memcpy(r.dpa, out.addr, data.data(), data.size() * sizeof(data[0])), "h2d(workload)");
    } catch (...) {
        (void)doca_dpa_mem_free(r.dpa, out.addr);
        out.addr = 0;
        out.mask = 0;
        throw;
    }
    return out;
}

void destroy_workload_buffer(const Resources &r, DpaBuffer &buf)
{
    if (buf.addr != 0) {
        (void)doca_dpa_mem_free(r.dpa, buf.addr);
        buf.addr = 0;
        buf.mask = 0;
    }
}

Result run_set(const Resources &r, const Workload &w, const std::vector<uint32_t> &eus,
               uint64_t M, uint64_t seed)
{
    const size_t n = eus.size();
    std::vector<SyncEv> ec(n), et(n), ei(n), es(n);
    std::vector<PinnedThread> pts(n);
    DpaBuffer data;
    Result out;
    out.cyc.resize(n, 0);
    out.time.resize(n, 0);
    out.inst.resize(n, 0);

    auto cleanup = [&]() {
        for (size_t i = 0; i < n; ++i) {
            destroy_pinned(r, pts[i]);
        }
        for (size_t i = 0; i < n; ++i) {
            destroy_ev(ec[i]);
            destroy_ev(et[i]);
            destroy_ev(ei[i]);
            destroy_ev(es[i]);
        }
        destroy_workload_buffer(r, data);
    };

    try {
        data = create_workload_buffer(r, w);
        for (size_t i = 0; i < n; ++i) {
            create_ev(r, ec[i]);
            create_ev(r, et[i]);
            create_ev(r, ei[i]);
            create_ev(r, es[i]);
        }
        for (size_t i = 0; i < n; ++i) {
            dpa_aff_cfg cfg;
            cfg.dpa_handle = static_cast<uint64_t>(r.dpa_handle);
            cfg.M = M;
            cfg.seed = seed ^ (static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL);
            cfg.data_addr = static_cast<uint64_t>(data.addr);
            cfg.data_mask = data.mask;
            cfg.data_stride = w.stride;
            cfg.start_index = (static_cast<uint64_t>(i) * 4099ULL + cfg.seed) & data.mask;
            cfg.se_cyc = ec[i].dpa_handle;
            cfg.se_time = et[i].dpa_handle;
            cfg.se_inst = ei[i].dpa_handle;
            cfg.se_sink = es[i].dpa_handle;
            create_pinned(r, eus[i], cfg, pts[i], w.fn);
        }

        kickoff(r, pts);

        for (size_t i = 0; i < n; ++i) {
            for (int p = 0; p < 5000000; ++p) {
                if (read_ev(ec[i]) > 0) {
                    break;
                }
            }
            out.cyc[i] = read_ev(ec[i]);
            out.time[i] = read_ev(et[i]);
            out.inst[i] = read_ev(ei[i]);
            (void)read_ev(es[i]);
        }
    } catch (...) {
        cleanup();
        throw;
    }

    cleanup();
    return out;
}

// total IPC sums instret / cycles for each thread
double total_ipc(const Result &r)
{
    double sum = 0.0;
    for (size_t i = 0; i < r.cyc.size(); ++i) {
        if (r.cyc[i] > 0) {
            sum += static_cast<double>(r.inst[i]) / static_cast<double>(r.cyc[i]);
        }
    }
    return sum;
}

// single-thread baseline
void measure_single_thread(const Resources &r, uint64_t M)
{
    std::cout << "\n# single-thread baseline on EU0\n";
    std::cout << "#    workload                instret   cycles    cyc/inst   IPC    cyc/tick(clock)\n";
    std::cout << std::fixed << std::setprecision(3);
    for (const auto &w : kWorkloads) {
        const Result s = run_set(r, w, {0}, M, kSeed);
        const double cpi = (s.inst[0] > 0) ? static_cast<double>(s.cyc[0]) / s.inst[0] : 0.0;
        const double ipc = (s.cyc[0] > 0) ? static_cast<double>(s.inst[0]) / s.cyc[0] : 0.0;
        const double clk = (s.time[0] > 0) ? static_cast<double>(s.cyc[0]) / s.time[0] : 0.0;
        std::cout << "#    " << std::left << std::setw(24) << w.label << std::right
                  << std::setw(9) << s.inst[0] << "  " << std::setw(8) << s.cyc[0]
                  << "   " << std::setw(7) << cpi << "  " << std::setw(5) << ipc
                  << "   " << std::setw(6) << clk << "\n";
        std::cout << std::unitbuf;
    }
}

// sweep threads across consecutive EUs and sum their IPC
void measure_scaling(Resources &r, uint64_t M, uint32_t eus_per_core, uint32_t maxK)
{
    const size_t nW = sizeof(kWorkloads) / sizeof(kWorkloads[0]);

    std::vector<std::vector<double>> ipc(nW, std::vector<double>(maxK, 0.0));

    for (size_t wi = 0; wi < nW; ++wi) {
        std::cout << "\n# scaling [" << kWorkloads[wi].label << "]: K=1.." << maxK
                  << " (sequential EU binding, core 0 first)\n";
        int budget = 200; // pinned-thread lifecycles per DPA session
        std::cout << std::fixed << std::setprecision(3);
        for (uint32_t K = 1; K <= maxK; ++K) {
            if (static_cast<int>(K) + 4 > budget) {
                reset_dpa(r);
                budget = 200;
            }
            std::vector<uint32_t> eus(K);
            for (uint32_t i = 0; i < K; ++i) {
                eus[i] = i;
            }
            const uint32_t cores = (K + eus_per_core - 1) / eus_per_core;
            try {
                const Result s = run_set(r, kWorkloads[wi], eus, M, kSeed);
                ipc[wi][K - 1] = total_ipc(s);
            } catch (const std::exception &e) {
                std::cout << "#    K=" << std::setw(2) << K << "  FAILED (" << e.what()
                          << ") — resetting, recording NaN\n";
                reset_dpa(r);
                budget = 200;
                ipc[wi][K - 1] = std::numeric_limits<double>::quiet_NaN();
            }
            budget -= static_cast<int>(K);
            std::cout << "#    K=" << std::setw(2) << K << "  cores=" << cores
                      << "  total_IPC=" << std::setw(7) << ipc[wi][K - 1] << "\n"
                      << std::unitbuf;
        }
    }

    std::cout << "\n# === plot: total IPC vs EU count ===\n";
    std::cout << "# EU\tcores";
    for (const auto &w : kWorkloads) {
        std::cout << "\t" << w.key << "_IPC";
    }
    std::cout << "\n";
    std::cout << std::fixed << std::setprecision(4);
    for (uint32_t K = 1; K <= maxK; ++K) {
        const uint32_t cores = (K + eus_per_core - 1) / eus_per_core;
        std::cout << K << "\t" << cores;
        for (size_t wi = 0; wi < nW; ++wi) {
            std::cout << "\t" << ipc[wi][K - 1];
        }
        std::cout << "\n";
    }
    std::cout << "# === end plot ===\n";
    std::cout << std::unitbuf;
}

// compare same-core and cross-core placement
void measure_cross_core(const Resources &r, uint64_t M)
{
    const Workload &w = kWorkloads[2];
    const Result alone = run_set(r, w, {0}, M, kSeed);
    const double base = static_cast<double>(alone.cyc[0]);

    std::cout << "\n# same-core and cross-core placement\n";
    std::cout << std::fixed << std::setprecision(3);

    const Result same2 = run_set(r, w, {0, 1}, M, kSeed);    // 2 threads, same core
    const Result cross2 = run_set(r, w, {0, 16}, M, kSeed);  // 2 threads, 2 cores
    std::cout << "#    alone (EU0)           : cyc=" << alone.cyc[0] << "\n";
    std::cout << "#    same-core  (EU0,EU1)  : cyc=" << same2.cyc[0] << " / " << same2.cyc[1]
              << "   (" << (static_cast<double>(same2.cyc[0]) / base) << "x baseline cycles)\n";
    std::cout << "#    cross-core (EU0,EU16) : cyc=" << cross2.cyc[0] << " / " << cross2.cyc[1]
              << "   (" << (static_cast<double>(cross2.cyc[0]) / base) << "x baseline cycles)\n";
    std::cout << std::unitbuf;
}

Config parse_args(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *n) -> std::string {
            if (i + 1 >= argc) {
                die(std::string("missing value for ") + n);
            }
            return std::string(argv[++i]);
        };
        if (a == "--device" || a == "--device-name") {
            cfg.device_name = need("--device-name");
        } else if (a == "--M") {
            cfg.M = std::stoull(need("--M"));
        } else if (a == "--max-eus") {
            cfg.max_eus = static_cast<uint32_t>(std::stoul(need("--max-eus")));
        } else if (a == "-h" || a == "--help") {
            std::cout << "dpa_arch_probe options:\n"
                      << "  --device-name <ibdev>   (default mlx5_0)\n"
                      << "  --M <loop iters>        (default 30000)\n"
                      << "  --max-eus <N>           (scaling sweep max EU count, default 64)\n";
            std::exit(EXIT_SUCCESS);
        } else {
            die("unknown argument: " + a);
        }
    }
    return cfg;
}

void run(const Config &cfg)
{
    Resources r;
    try {
        open_device(cfg.device_name, &r.dev);
        check(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
        check(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
        check(doca_dpa_start(r.dpa), "doca_dpa_start");
        check(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");

        unsigned int cores = 0, eus = 0, total = 0;
        (void)doca_dpa_get_core_num(r.dpa, &cores);
        (void)doca_dpa_get_num_eus_per_core(r.dpa, &eus);
        (void)doca_dpa_get_total_num_eus_available(r.dpa, &total);
        std::cout << "# topology: cores=" << cores << " eus_per_core=" << eus
                  << " total_eus=" << total << "  (M=" << cfg.M << ")\n";

        measure_single_thread(r, cfg.M);
        measure_scaling(r, cfg.M, eus, cfg.max_eus);
        reset_dpa(r); // start a fresh session for the cross-core run
        try {
            measure_cross_core(r, cfg.M);
        } catch (const std::exception &e) {
            // keep the scaling results if the cross-core run fails
            std::cout << "# (cross-core skipped: " << e.what() << ")\n";
        }

        (void)doca_dpa_stop(r.dpa);
        (void)doca_dpa_destroy(r.dpa);
        (void)doca_dev_close(r.dev);
    } catch (...) {
        if (r.dpa) {
            (void)doca_dpa_stop(r.dpa);
            (void)doca_dpa_destroy(r.dpa);
        }
        if (r.dev) {
            (void)doca_dev_close(r.dev);
        }
        throw;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    try {
        init_log();
        const Config cfg = parse_args(argc, argv);
        run(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
