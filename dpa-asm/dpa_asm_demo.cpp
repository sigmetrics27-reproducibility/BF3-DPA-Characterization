// run the asm demo and read back the sum and cycle counter
// try ./build/dpa_asm_demo --device-name mlx5_0
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_sync_event.h>

#include "host/dpa_asm_demo_common.h"

extern "C" {
extern doca_dpa_func_t k_demo;
}

extern struct doca_dpa_app *DPA_APP_NAME;
DOCA_LOG_REGISTER(DPA_ASM_DEMO);

namespace {

constexpr uint64_t kSyncEventMask = UINT64_MAX;

[[noreturn]] void die(const std::string &m) { throw std::runtime_error(m); }
void check(doca_error_t st, const char *what)
{
    if (st != DOCA_SUCCESS) {
        die(std::string(what) + std::string(" failed: ") + doca_error_get_descr(st));
    }
}

struct SyncEv {
    doca_sync_event *event = nullptr;
    doca_dpa_dev_sync_event_t dpa_handle = 0;
};

void open_device(const std::string &name, doca_dev **dev)
{
    struct doca_devinfo **list = nullptr;
    uint32_t n = 0;
    check(doca_devinfo_create_list(&list, &n), "devinfo_create_list");
    char ibdev[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
    bool found = false;
    for (uint32_t i = 0; i < n; ++i) {
        if (doca_devinfo_get_ibdev_name(list[i], ibdev, DOCA_DEVINFO_IBDEV_NAME_SIZE) == DOCA_SUCCESS &&
            name == ibdev) {
            check(doca_dev_open(list[i], dev), "dev_open");
            found = true;
            break;
        }
    }
    check(doca_devinfo_destroy_list(list), "devinfo_destroy_list");
    if (!found || *dev == nullptr) {
        die("IB device not found: " + name);
    }
}

// device writes the result, host reads it
void create_ev(doca_dpa *dpa, doca_dev *dev, SyncEv &se)
{
    check(doca_sync_event_create(&se.event), "sync_event_create");
    check(doca_sync_event_add_publisher_location_dpa(se.event, dpa), "add_publisher_location_dpa");
    check(doca_sync_event_add_subscriber_location_cpu(se.event, dev), "add_subscriber_location_cpu");
    check(doca_sync_event_start(se.event), "sync_event_start");
    check(doca_sync_event_get_dpa_handle(se.event, dpa, &se.dpa_handle), "get_dpa_handle");
}

void destroy_ev(SyncEv &se)
{
    if (se.event) {
        (void)doca_sync_event_stop(se.event);
        (void)doca_sync_event_destroy(se.event);
        se.event = nullptr;
    }
}

uint64_t read_ev(const SyncEv &se)
{
    uint64_t v = 0;
    check(doca_sync_event_get(se.event, &v), "sync_event_get");
    return v;
}

struct Config {
    std::string device_name = "mlx5_0";
    uint64_t a = 10, b = 20, c = 30;
};

Config parse_args(int argc, char **argv)
{
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&]() {
            if (i + 1 >= argc) {
                die(std::string("missing value for ") + a);
            }
            return std::string(argv[++i]);
        };
        if (a == "--device" || a == "--device-name") {
            cfg.device_name = need();
        } else if (a == "-h" || a == "--help") {
            std::cout << "dpa_asm_demo options:\n"
                      << "  --device-name <ibdev>   (default mlx5_0)\n";
            std::exit(EXIT_SUCCESS);
        } else {
            die("unknown argument: " + a);
        }
    }
    return cfg;
}

void run(const Config &cfg)
{
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    try {
        open_device(cfg.device_name, &dev);
        check(doca_dpa_create(dev, &dpa), "doca_dpa_create");
        check(doca_dpa_set_app(dpa, DPA_APP_NAME), "doca_dpa_set_app");
        check(doca_dpa_start(dpa), "doca_dpa_start");

        // two results and one completion event
        SyncEv se_sum, se_cyc, comp;
        create_ev(dpa, dev, se_sum);
        create_ev(dpa, dev, se_cyc);
        create_ev(dpa, dev, comp);

        // put the kernel args on the DPA heap
        dpa_asm_demo_cfg cfgd{};
        cfgd.a = cfg.a;
        cfgd.b = cfg.b;
        cfgd.c = cfg.c;
        cfgd.se_sum = static_cast<uint64_t>(se_sum.dpa_handle);
        cfgd.se_cyc = static_cast<uint64_t>(se_cyc.dpa_handle);
        doca_dpa_dev_uintptr_t cfg_dev = 0;
        check(doca_dpa_mem_alloc(dpa, sizeof(cfgd), &cfg_dev), "mem_alloc(cfg)");
        check(doca_dpa_h2d_memcpy(dpa, cfg_dev, &cfgd, sizeof(cfgd)), "h2d(cfg)");

        // run the asm demo on one thread
        check(doca_dpa_kernel_launch_update_set(dpa, nullptr, 0,
                                                comp.event, 1,
                                                1, &k_demo,
                                                static_cast<uint64_t>(cfg_dev)),
             "kernel_launch(k_demo)");
        check(doca_sync_event_wait_gt(comp.event, 0, kSyncEventMask), "wait_gt(comp)");

        const uint64_t sum = read_ev(se_sum);
        const uint64_t cyc = read_ev(se_cyc);
        std::cout << "asm_sum3(" << cfg.a << "," << cfg.b << "," << cfg.c << ") = " << sum
                  << "   (expect " << (cfg.a + cfg.b + cfg.c) << ")\n"
                  << "asm_rdcycle() = " << cyc << "   (live cycle counter via hand-written csrr)\n"
                  << (sum == cfg.a + cfg.b + cfg.c
                          ? "=> hand-written ASM executed correctly on the DPA\n"
                          : "=> MISMATCH\n");

        check(doca_dpa_mem_free(dpa, cfg_dev), "mem_free(cfg)");
        destroy_ev(comp);
        destroy_ev(se_cyc);
        destroy_ev(se_sum);
        check(doca_dpa_stop(dpa), "doca_dpa_stop");
        check(doca_dpa_destroy(dpa), "doca_dpa_destroy");
        check(doca_dev_close(dev), "doca_dev_close");
    } catch (...) {
        if (dpa) {
            (void)doca_dpa_stop(dpa);
            (void)doca_dpa_destroy(dpa);
        }
        if (dev) {
            (void)doca_dev_close(dev);
        }
        throw;
    }
}

}  // namespace

int main(int argc, char **argv)
{
    try {
        check(doca_log_backend_create_standard(), "log_backend_create_standard");
        const Config cfg = parse_args(argc, argv);
        run(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
