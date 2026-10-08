#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_verbs.h>
#include <doca_verbs_bridge.h>

#include <infiniband/verbs.h>

#include "host/dpa_verbs_copy_common.h"

extern "C" {
    doca_dpa_func_t dpa_copy_run_rpc;
}

extern struct doca_dpa_app *DPA_APP_NAME;

DOCA_LOG_REGISTER(DPA_VERBS_COPY_BENCH);

namespace {

constexpr uint32_t kDefaultGidIndex = 0;
constexpr uint64_t kDefaultMinPayload = 64;
constexpr uint64_t kDefaultMaxPayload = 1ULL << 20;  // 1 MiB
constexpr double kDefaultSizeFactor = 2.0;
constexpr uint32_t kDefaultWarmup = 200;
constexpr uint32_t kDefaultIters = 2000;
constexpr uint32_t kDefaultClockMHz = 1800;
constexpr uint32_t kDefaultQpDepth = 4096;
constexpr uint32_t kD2HVerifyChunk = 64 * 1024;

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t gid_index = kDefaultGidIndex;
    uint32_t mode = DPA_VERBS_COPY_MODE_READ;

    uint64_t min_payload = kDefaultMinPayload;
    uint64_t max_payload = kDefaultMaxPayload;
    double size_factor = kDefaultSizeFactor;

    uint32_t warmup = kDefaultWarmup;
    uint32_t iters = kDefaultIters;
    uint32_t clock_mhz = kDefaultClockMHz;
    uint32_t seed = 1;
    uint32_t qp_depth = kDefaultQpDepth;
    bool verify = false;
};

struct EndpointInfo {
    uint32_t qpn = 0;
    doca_verbs_gid gid{};
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_verbs_context *verbs_context = nullptr;
    doca_verbs_pd *verbs_pd = nullptr;
    doca_verbs_ah_attr *verbs_ah_attr = nullptr;

    doca_verbs_cq *host_cq = nullptr;
    doca_verbs_qp *host_qp = nullptr;

    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;
    doca_dpa_completion *dpa_completion = nullptr;
    doca_dpa_dev_completion_t dpa_completion_handle = 0;

    doca_verbs_qp *dpa_qp = nullptr;
    doca_dpa_dev_verbs_qp_t dpa_qp_handle = 0;

    void *host_buf = nullptr;
    size_t host_buf_bytes = 0;
    ibv_mr *host_mr = nullptr;
    doca_mmap *host_mmap = nullptr;
    doca_dpa_dev_mmap_t host_dpa_mmap_handle = 0;

    doca_dpa_dev_uintptr_t local_dpa_buf = 0;
    size_t local_dpa_buf_bytes = 0;
    doca_mmap *local_dpa_mmap = nullptr;
    doca_dpa_dev_mmap_t local_dpa_mmap_handle = 0;
};

[[noreturn]] void throw_runtime(const std::string &msg)
{
    throw std::runtime_error(msg);
}

void check_doca(doca_error_t status, const char *what)
{
    if (status != DOCA_SUCCESS) {
        throw_runtime(std::string(what) + " failed: " + doca_error_get_descr(status));
    }
}

uint64_t align_up(uint64_t val, uint64_t align)
{
    if (align == 0) {
        return val;
    }
    uint64_t rem = val % align;
    if (rem == 0) {
        return val;
    }
    return val + (align - rem);
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

uint32_t parse_mode(const std::string &s)
{
    if (s == "read") {
        return DPA_VERBS_COPY_MODE_READ;
    }
    if (s == "write") {
        return DPA_VERBS_COPY_MODE_WRITE;
    }
    if (s == "send") {
        return DPA_VERBS_COPY_MODE_SEND;
    }
    throw_runtime("Unsupported mode: " + s);
}

std::string mode_to_string(uint32_t mode)
{
    switch (mode) {
    case DPA_VERBS_COPY_MODE_READ:
        return "read";
    case DPA_VERBS_COPY_MODE_WRITE:
        return "write";
    case DPA_VERBS_COPY_MODE_SEND:
        return "send";
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
        << "  --mode read|write|send          Benchmark mode (default: read)\n"
        << "  --gid-index <num>               GID index (default: 0)\n"
        << "  --min-payload <bytes>           Min payload bytes (default: 64)\n"
        << "  --max-payload <bytes>           Max payload bytes (default: 1048576)\n"
        << "  --size-factor <float>           Payload growth factor (default: 2.0)\n"
        << "  --warmup <num>                  Warmup iterations per payload (default: 200)\n"
        << "  --iters <num>                   Measured iterations per payload (default: 2000)\n"
        << "  --clock-mhz <num>               DPA clock MHz for ns conversion (default: 1800)\n"
        << "  --seed <num>                    Seed for deterministic fill pattern (default: 1)\n"
        << "  --qp-depth <num>                QP depth and completion queue scale (default: 4096)\n"
        << "  --verify                        Verify payload content after each run\n";
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
        } else if (arg == "--gid-index") {
            cfg.gid_index = static_cast<uint32_t>(parse_u64(next("--gid-index"), "--gid-index"));
        } else if (arg == "--min-payload") {
            cfg.min_payload = parse_u64(next("--min-payload"), "--min-payload");
        } else if (arg == "--max-payload") {
            cfg.max_payload = parse_u64(next("--max-payload"), "--max-payload");
        } else if (arg == "--size-factor") {
            cfg.size_factor = parse_double(next("--size-factor"), "--size-factor");
        } else if (arg == "--warmup") {
            cfg.warmup = static_cast<uint32_t>(parse_u64(next("--warmup"), "--warmup"));
        } else if (arg == "--iters") {
            cfg.iters = static_cast<uint32_t>(parse_u64(next("--iters"), "--iters"));
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = static_cast<uint32_t>(parse_u64(next("--clock-mhz"), "--clock-mhz"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<uint32_t>(parse_u64(next("--seed"), "--seed"));
        } else if (arg == "--qp-depth") {
            cfg.qp_depth = static_cast<uint32_t>(parse_u64(next("--qp-depth"), "--qp-depth"));
        } else if (arg == "--verify") {
            cfg.verify = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            throw_runtime("Unknown argument: " + arg);
        }
    }

    if (cfg.max_payload < cfg.min_payload) {
        throw_runtime("--max-payload must be >= --min-payload");
    }
    if (cfg.min_payload == 0) {
        throw_runtime("--min-payload must be > 0");
    }
    if (cfg.size_factor <= 1.0) {
        throw_runtime("--size-factor must be > 1.0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }
    if (cfg.qp_depth == 0) {
        throw_runtime("--qp-depth must be > 0");
    }
    if (cfg.max_payload > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("--max-payload must fit in uint32");
    }

    uint64_t total_ops = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
    if (total_ops > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("warmup + iters exceeds uint32 range");
    }
    if (cfg.mode == DPA_VERBS_COPY_MODE_SEND && total_ops > cfg.qp_depth) {
        throw_runtime("For send mode, --qp-depth must be >= warmup + iters");
    }

    return cfg;
}

std::vector<uint64_t> build_payload_list(const Config &cfg)
{
    std::vector<uint64_t> payloads;
    uint64_t cur = cfg.min_payload;

    while (cur <= cfg.max_payload) {
        payloads.push_back(cur);

        uint64_t next = static_cast<uint64_t>(static_cast<double>(cur) * cfg.size_factor);
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

void open_verbs_resources(const std::string &device_name,
                          doca_verbs_context **verbs_ctx,
                          doca_verbs_pd **verbs_pd,
                          doca_dev **dev)
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
            check_doca(doca_verbs_context_create(devinfo_list[i], DOCA_VERBS_CONTEXT_CREATE_FLAGS_NONE, verbs_ctx),
                       "doca_verbs_context_create");
            check_doca(doca_verbs_pd_create(*verbs_ctx, verbs_pd), "doca_verbs_pd_create");
            check_doca(doca_verbs_pd_as_doca_dev(*verbs_pd, dev), "doca_verbs_pd_as_doca_dev");
            found = true;
            break;
        }
    }

    check_doca(doca_devinfo_destroy_list(devinfo_list), "doca_devinfo_destroy_list");
    if (!found) {
        throw_runtime("Device not found: " + device_name);
    }
}

void create_verbs_ah_attr(doca_verbs_context *verbs_context,
                          uint32_t gid_index,
                          doca_verbs_ah_attr **verbs_ah_attr)
{
    check_doca(doca_verbs_ah_attr_create(verbs_context, verbs_ah_attr), "doca_verbs_ah_attr_create");
    check_doca(doca_verbs_ah_attr_set_addr_type(*verbs_ah_attr, DOCA_VERBS_ADDR_TYPE_IPv4),
               "doca_verbs_ah_attr_set_addr_type");
    check_doca(doca_verbs_ah_attr_set_sgid_index(*verbs_ah_attr, gid_index),
               "doca_verbs_ah_attr_set_sgid_index");
    check_doca(doca_verbs_ah_attr_set_hop_limit(*verbs_ah_attr, 255), "doca_verbs_ah_attr_set_hop_limit");
}

void create_verbs_cq(doca_verbs_context *verbs_context, uint32_t cq_size, doca_verbs_cq **verbs_cq)
{
    doca_verbs_cq_attr *cq_attr = nullptr;
    check_doca(doca_verbs_cq_attr_create(&cq_attr), "doca_verbs_cq_attr_create");
    try {
        check_doca(doca_verbs_cq_attr_set_cq_size(cq_attr, cq_size), "doca_verbs_cq_attr_set_cq_size");
        check_doca(doca_verbs_cq_attr_set_external_datapath_en(cq_attr, 0),
                   "doca_verbs_cq_attr_set_external_datapath_en");
        check_doca(doca_verbs_cq_attr_set_entry_size(cq_attr, DOCA_VERBS_CQ_ENTRY_SIZE_64),
                   "doca_verbs_cq_attr_set_entry_size");
        check_doca(doca_verbs_cq_create(verbs_context, cq_attr, verbs_cq), "doca_verbs_cq_create");
    } catch (...) {
        (void)doca_verbs_cq_attr_destroy(cq_attr);
        throw;
    }
    check_doca(doca_verbs_cq_attr_destroy(cq_attr), "doca_verbs_cq_attr_destroy");
}

void create_verbs_qp_host(doca_verbs_context *verbs_context,
                          doca_verbs_pd *verbs_pd,
                          doca_verbs_cq *verbs_cq,
                          uint32_t rq_wr,
                          uint32_t sq_wr,
                          doca_verbs_qp **verbs_qp)
{
    doca_verbs_qp_init_attr *qp_attr = nullptr;
    check_doca(doca_verbs_qp_init_attr_create(&qp_attr), "doca_verbs_qp_init_attr_create");
    try {
        check_doca(doca_verbs_qp_init_attr_set_pd(qp_attr, verbs_pd), "doca_verbs_qp_init_attr_set_pd");
        check_doca(doca_verbs_qp_init_attr_set_sq_wr(qp_attr, sq_wr), "doca_verbs_qp_init_attr_set_sq_wr");
        check_doca(doca_verbs_qp_init_attr_set_rq_wr(qp_attr, rq_wr), "doca_verbs_qp_init_attr_set_rq_wr");
        check_doca(doca_verbs_qp_init_attr_set_qp_type(qp_attr, DOCA_VERBS_QP_TYPE_RC),
                   "doca_verbs_qp_init_attr_set_qp_type");
        check_doca(doca_verbs_qp_init_attr_set_send_cq(qp_attr, verbs_cq),
                   "doca_verbs_qp_init_attr_set_send_cq");
        check_doca(doca_verbs_qp_init_attr_set_receive_cq(qp_attr, verbs_cq),
                   "doca_verbs_qp_init_attr_set_receive_cq");
        check_doca(doca_verbs_qp_init_attr_set_external_datapath_en(qp_attr, 0),
                   "doca_verbs_qp_init_attr_set_external_datapath_en");
        check_doca(doca_verbs_qp_init_attr_set_send_max_sges(qp_attr, 1),
                   "doca_verbs_qp_init_attr_set_send_max_sges");
        check_doca(doca_verbs_qp_init_attr_set_receive_max_sges(qp_attr, 1),
                   "doca_verbs_qp_init_attr_set_receive_max_sges");
        check_doca(doca_verbs_qp_create(verbs_context, qp_attr, verbs_qp), "doca_verbs_qp_create(host)");
    } catch (...) {
        (void)doca_verbs_qp_init_attr_destroy(qp_attr);
        throw;
    }
    check_doca(doca_verbs_qp_init_attr_destroy(qp_attr), "doca_verbs_qp_init_attr_destroy");
}

void create_verbs_qp_dpa(doca_verbs_context *verbs_context,
                         doca_dpa *dpa,
                         doca_verbs_pd *verbs_pd,
                         doca_dpa_completion *dpa_completion,
                         uint32_t sq_wr,
                         doca_verbs_qp **verbs_qp)
{
    doca_verbs_qp_init_attr *qp_attr = nullptr;
    check_doca(doca_verbs_qp_init_attr_create(&qp_attr), "doca_verbs_qp_init_attr_create");
    try {
        check_doca(doca_verbs_qp_init_attr_set_external_datapath_en(qp_attr, 1),
                   "doca_verbs_qp_init_attr_set_external_datapath_en");
        check_doca(doca_verbs_qp_init_attr_set_dpa(qp_attr, dpa), "doca_verbs_qp_init_attr_set_dpa");
        check_doca(doca_verbs_qp_init_attr_set_pd(qp_attr, verbs_pd), "doca_verbs_qp_init_attr_set_pd");
        check_doca(doca_verbs_qp_init_attr_set_sq_wr(qp_attr, sq_wr), "doca_verbs_qp_init_attr_set_sq_wr");
        check_doca(doca_verbs_qp_init_attr_set_rq_wr(qp_attr, 1), "doca_verbs_qp_init_attr_set_rq_wr");
        check_doca(doca_verbs_qp_init_attr_set_qp_type(qp_attr, DOCA_VERBS_QP_TYPE_RC),
                   "doca_verbs_qp_init_attr_set_qp_type");
        check_doca(doca_verbs_qp_init_attr_set_send_dpa_completion(qp_attr, dpa_completion),
                   "doca_verbs_qp_init_attr_set_send_dpa_completion");
        check_doca(doca_verbs_qp_init_attr_set_receive_dpa_completion(qp_attr, dpa_completion),
                   "doca_verbs_qp_init_attr_set_receive_dpa_completion");
        check_doca(doca_verbs_qp_init_attr_set_send_max_sges(qp_attr, 1),
                   "doca_verbs_qp_init_attr_set_send_max_sges");
        check_doca(doca_verbs_qp_init_attr_set_receive_max_sges(qp_attr, 1),
                   "doca_verbs_qp_init_attr_set_receive_max_sges");
        check_doca(doca_verbs_qp_create(verbs_context, qp_attr, verbs_qp), "doca_verbs_qp_create(dpa)");
    } catch (...) {
        (void)doca_verbs_qp_init_attr_destroy(qp_attr);
        throw;
    }
    check_doca(doca_verbs_qp_init_attr_destroy(qp_attr), "doca_verbs_qp_init_attr_destroy");
}

EndpointInfo query_local_endpoint(doca_verbs_pd *verbs_pd, doca_verbs_qp *verbs_qp, uint32_t gid_index)
{
    EndpointInfo ep{};
    ep.qpn = doca_verbs_qp_get_qpn(verbs_qp);

    ibv_pd *ibvpd = doca_verbs_bridge_verbs_pd_get_ibv_pd(verbs_pd);
    if (ibvpd == nullptr) {
        throw_runtime("doca_verbs_bridge_verbs_pd_get_ibv_pd returned null");
    }

    union ibv_gid gid{};
    if (ibv_query_gid(ibvpd->context, 1, gid_index, &gid) != 0) {
        throw_runtime("ibv_query_gid failed");
    }
    std::memcpy(ep.gid.raw, gid.raw, DOCA_GID_BYTE_LENGTH);
    return ep;
}

void connect_verbs_qp(doca_verbs_qp *verbs_qp,
                      doca_verbs_ah_attr *verbs_ah_attr,
                      const EndpointInfo &remote_ep)
{
    check_doca(doca_verbs_ah_attr_set_gid(verbs_ah_attr, remote_ep.gid), "doca_verbs_ah_attr_set_gid");

    doca_verbs_qp_attr *qp_attr = nullptr;
    check_doca(doca_verbs_qp_attr_create(&qp_attr), "doca_verbs_qp_attr_create");

    try {
        check_doca(doca_verbs_qp_attr_set_next_state(qp_attr, DOCA_VERBS_QP_STATE_INIT),
                   "doca_verbs_qp_attr_set_next_state(INIT)");
        check_doca(doca_verbs_qp_attr_set_allow_remote_write(qp_attr, 1), "doca_verbs_qp_attr_set_allow_remote_write");
        check_doca(doca_verbs_qp_attr_set_allow_remote_read(qp_attr, 1), "doca_verbs_qp_attr_set_allow_remote_read");
        check_doca(doca_verbs_qp_attr_set_port_num(qp_attr, 1), "doca_verbs_qp_attr_set_port_num");
        check_doca(doca_verbs_qp_modify(verbs_qp,
                                        qp_attr,
                                        DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_WRITE |
                                            DOCA_VERBS_QP_ATTR_ALLOW_REMOTE_READ | DOCA_VERBS_QP_ATTR_PKEY_INDEX |
                                            DOCA_VERBS_QP_ATTR_PORT_NUM),
                   "doca_verbs_qp_modify(RST->INIT)");

        check_doca(doca_verbs_qp_attr_set_next_state(qp_attr, DOCA_VERBS_QP_STATE_RTR),
                   "doca_verbs_qp_attr_set_next_state(RTR)");
        check_doca(doca_verbs_qp_attr_set_rq_psn(qp_attr, 0), "doca_verbs_qp_attr_set_rq_psn");
        check_doca(doca_verbs_qp_attr_set_dest_qp_num(qp_attr, remote_ep.qpn), "doca_verbs_qp_attr_set_dest_qp_num");
        check_doca(doca_verbs_qp_attr_set_min_rnr_timer(qp_attr, 1), "doca_verbs_qp_attr_set_min_rnr_timer");
        check_doca(doca_verbs_qp_attr_set_path_mtu(qp_attr, DOCA_MTU_SIZE_1K_BYTES), "doca_verbs_qp_attr_set_path_mtu");
        check_doca(doca_verbs_qp_attr_set_ah_attr(qp_attr, verbs_ah_attr), "doca_verbs_qp_attr_set_ah_attr");
        check_doca(doca_verbs_qp_modify(verbs_qp,
                                        qp_attr,
                                        DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_RQ_PSN |
                                            DOCA_VERBS_QP_ATTR_DEST_QP_NUM | DOCA_VERBS_QP_ATTR_MIN_RNR_TIMER |
                                            DOCA_VERBS_QP_ATTR_PATH_MTU | DOCA_VERBS_QP_ATTR_AH_ATTR),
                   "doca_verbs_qp_modify(INIT->RTR)");

        check_doca(doca_verbs_qp_attr_set_next_state(qp_attr, DOCA_VERBS_QP_STATE_RTS),
                   "doca_verbs_qp_attr_set_next_state(RTS)");
        check_doca(doca_verbs_qp_attr_set_sq_psn(qp_attr, 0), "doca_verbs_qp_attr_set_sq_psn");
        check_doca(doca_verbs_qp_attr_set_ack_timeout(qp_attr, 14), "doca_verbs_qp_attr_set_ack_timeout");
        check_doca(doca_verbs_qp_attr_set_retry_cnt(qp_attr, 7), "doca_verbs_qp_attr_set_retry_cnt");
        check_doca(doca_verbs_qp_attr_set_rnr_retry(qp_attr, 1), "doca_verbs_qp_attr_set_rnr_retry");
        check_doca(doca_verbs_qp_modify(verbs_qp,
                                        qp_attr,
                                        DOCA_VERBS_QP_ATTR_NEXT_STATE | DOCA_VERBS_QP_ATTR_SQ_PSN |
                                            DOCA_VERBS_QP_ATTR_ACK_TIMEOUT | DOCA_VERBS_QP_ATTR_RETRY_CNT |
                                            DOCA_VERBS_QP_ATTR_RNR_RETRY),
                   "doca_verbs_qp_modify(RTR->RTS)");
    } catch (...) {
        (void)doca_verbs_qp_attr_destroy(qp_attr);
        throw;
    }

    check_doca(doca_verbs_qp_attr_destroy(qp_attr), "doca_verbs_qp_attr_destroy");
}

void post_host_receives(Resources &res, uint32_t payload, uint64_t total_ops)
{
    auto *base = static_cast<uint8_t *>(res.host_buf);

    for (uint64_t i = 0; i < total_ops; ++i) {
        ibv_sge sge{};
        sge.addr = reinterpret_cast<uint64_t>(base);
        sge.length = payload;
        sge.lkey = res.host_mr->lkey;

        ibv_recv_wr wr{};
        wr.wr_id = i;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        ibv_recv_wr *bad = nullptr;
        int rc = doca_verbs_bridge_post_recv(res.host_qp, &wr, &bad);
        if (rc != 0) {
            throw_runtime("doca_verbs_bridge_post_recv failed with rc=" + std::to_string(rc));
        }
    }
}

void poll_host_recv_completions(Resources &res, uint64_t expected)
{
    uint64_t completed = 0;
    std::vector<ibv_wc> wc(64);

    while (completed < expected) {
        int n = doca_verbs_bridge_poll_cq(res.host_cq, static_cast<int>(wc.size()), wc.data());
        if (n < 0) {
            throw_runtime("doca_verbs_bridge_poll_cq failed");
        }
        if (n == 0) {
            continue;
        }

        for (int i = 0; i < n; ++i) {
            if (wc[i].status != IBV_WC_SUCCESS) {
                throw_runtime("CQ completion error status=" + std::to_string(wc[i].status));
            }
            ++completed;
        }
    }
}

uint8_t fill_value_for_payload(const Config &cfg, uint32_t payload)
{
    return static_cast<uint8_t>((payload ^ (cfg.mode * 31U) ^ (cfg.seed * 17U)) & 0xFFU);
}

void prepare_payload_buffers(const Config &cfg, const Resources &res, uint32_t payload)
{
    const uint8_t fill = fill_value_for_payload(cfg, payload);
    std::memset(res.host_buf, 0, payload);

    if (cfg.mode == DPA_VERBS_COPY_MODE_READ) {
        std::memset(res.host_buf, fill, payload);
        check_doca(doca_dpa_memset(res.dpa, res.local_dpa_buf, 0, payload), "doca_dpa_memset(local read dst)");
    } else {
        check_doca(doca_dpa_memset(res.dpa, res.local_dpa_buf, fill, payload), "doca_dpa_memset(local write src)");
    }
}

void verify_payload_result(const Config &cfg, const Resources &res, uint32_t payload)
{
    if (!cfg.verify) {
        return;
    }

    const uint8_t expected_fill = fill_value_for_payload(cfg, payload);

    if (cfg.mode == DPA_VERBS_COPY_MODE_READ) {
        auto *local = static_cast<uint8_t *>(res.host_buf);
        std::memset(local, 0, payload);
        uint32_t offset = 0;
        while (offset < payload) {
            uint32_t chunk = std::min<uint32_t>(kD2HVerifyChunk, payload - offset);
            check_doca(doca_dpa_d2h_memcpy(res.dpa, local + offset, res.local_dpa_buf + offset, chunk),
                       "doca_dpa_d2h_memcpy(local_dpa_buf chunk)");
            offset += chunk;
        }
        for (uint32_t i = 0; i < payload; ++i) {
            if (local[i] != expected_fill) {
                throw_runtime("verify(read) failed: local DPA buffer content mismatch at byte " +
                              std::to_string(i));
            }
        }
        return;
    }

    const auto *host = static_cast<const uint8_t *>(res.host_buf);
    for (uint32_t i = 0; i < payload; ++i) {
        if (host[i] != expected_fill) {
            throw_runtime("verify(" + mode_to_string(cfg.mode) + ") failed at byte " + std::to_string(i));
        }
    }
}

void cleanup_resources(Resources &r)
{
    if (r.local_dpa_buf != 0 && r.dpa != nullptr) {
        if (r.local_dpa_mmap != nullptr) {
            (void)doca_mmap_destroy(r.local_dpa_mmap);
            r.local_dpa_mmap = nullptr;
        }
        (void)doca_dpa_mem_free(r.dpa, r.local_dpa_buf);
        r.local_dpa_buf = 0;
    }

    if (r.host_mmap != nullptr) {
        (void)doca_mmap_destroy(r.host_mmap);
        r.host_mmap = nullptr;
    }

    if (r.host_mr != nullptr) {
        (void)ibv_dereg_mr(r.host_mr);
        r.host_mr = nullptr;
    }

    if (r.host_buf != nullptr) {
        free(r.host_buf);
        r.host_buf = nullptr;
    }

    if (r.dpa_qp != nullptr) {
        (void)doca_verbs_qp_destroy(r.dpa_qp);
        r.dpa_qp = nullptr;
    }

    if (r.host_qp != nullptr) {
        (void)doca_verbs_qp_destroy(r.host_qp);
        r.host_qp = nullptr;
    }

    if (r.dpa_completion != nullptr) {
        (void)doca_dpa_completion_stop(r.dpa_completion);
        (void)doca_dpa_completion_destroy(r.dpa_completion);
        r.dpa_completion = nullptr;
    }

    if (r.host_cq != nullptr) {
        (void)doca_verbs_cq_destroy(r.host_cq);
        r.host_cq = nullptr;
    }

    if (r.verbs_ah_attr != nullptr) {
        (void)doca_verbs_ah_attr_destroy(r.verbs_ah_attr);
        r.verbs_ah_attr = nullptr;
    }

    if (r.dpa != nullptr) {
        (void)doca_dpa_stop(r.dpa);
        (void)doca_dpa_destroy(r.dpa);
        r.dpa = nullptr;
    }

    if (r.dev != nullptr) {
        (void)doca_dev_close(r.dev);
        r.dev = nullptr;
    }

    if (r.verbs_pd != nullptr) {
        (void)doca_verbs_pd_destroy(r.verbs_pd);
        r.verbs_pd = nullptr;
    }

    if (r.verbs_context != nullptr) {
        (void)doca_verbs_context_destroy(r.verbs_context);
        r.verbs_context = nullptr;
    }
}

void init_resources(const Config &cfg, Resources &r)
{
    open_verbs_resources(cfg.device_name, &r.verbs_context, &r.verbs_pd, &r.dev);
    create_verbs_ah_attr(r.verbs_context, cfg.gid_index, &r.verbs_ah_attr);

    uint32_t cq_depth = std::max<uint32_t>(cfg.qp_depth * 2, 1024);
    create_verbs_cq(r.verbs_context, cq_depth, &r.host_cq);

    create_verbs_qp_host(r.verbs_context, r.verbs_pd, r.host_cq, cfg.qp_depth, 32, &r.host_qp);

    check_doca(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
    check_doca(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
    check_doca(doca_dpa_start(r.dpa), "doca_dpa_start");
    check_doca(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");

    check_doca(doca_dpa_completion_create(r.dpa, cfg.qp_depth * 2, &r.dpa_completion),
               "doca_dpa_completion_create");
    check_doca(doca_dpa_completion_start(r.dpa_completion), "doca_dpa_completion_start");
    check_doca(doca_dpa_completion_get_dpa_handle(r.dpa_completion, &r.dpa_completion_handle),
               "doca_dpa_completion_get_dpa_handle");

    create_verbs_qp_dpa(r.verbs_context, r.dpa, r.verbs_pd, r.dpa_completion, cfg.qp_depth, &r.dpa_qp);
    check_doca(doca_verbs_qp_get_dpa_handle(r.dpa_qp, r.dpa, &r.dpa_qp_handle), "doca_verbs_qp_get_dpa_handle");

    EndpointInfo host_ep = query_local_endpoint(r.verbs_pd, r.host_qp, cfg.gid_index);
    EndpointInfo dpa_ep = query_local_endpoint(r.verbs_pd, r.dpa_qp, cfg.gid_index);

    connect_verbs_qp(r.host_qp, r.verbs_ah_attr, dpa_ep);
    connect_verbs_qp(r.dpa_qp, r.verbs_ah_attr, host_ep);

    r.host_buf_bytes = static_cast<size_t>(std::max<uint64_t>(8, cfg.max_payload));
    void *host_buf = nullptr;
    if (posix_memalign(&host_buf, 4096, r.host_buf_bytes) != 0 || host_buf == nullptr) {
        throw_runtime("posix_memalign failed for host buffer");
    }
    r.host_buf = host_buf;
    std::memset(r.host_buf, 0, r.host_buf_bytes);

    ibv_pd *ibvpd = doca_verbs_bridge_verbs_pd_get_ibv_pd(r.verbs_pd);
    if (ibvpd == nullptr) {
        throw_runtime("doca_verbs_bridge_verbs_pd_get_ibv_pd returned null");
    }

    r.host_mr = ibv_reg_mr(ibvpd,
                           r.host_buf,
                           r.host_buf_bytes,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (r.host_mr == nullptr) {
        throw_runtime("ibv_reg_mr failed for host buffer");
    }

    check_doca(doca_mmap_create(&r.host_mmap), "doca_mmap_create(host)");
    check_doca(doca_mmap_set_permissions(r.host_mmap,
                                         DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
                                             DOCA_ACCESS_FLAG_RDMA_WRITE),
               "doca_mmap_set_permissions(host)");
    check_doca(doca_mmap_set_memrange(r.host_mmap, r.host_buf, r.host_buf_bytes), "doca_mmap_set_memrange(host)");
    check_doca(doca_mmap_add_dev(r.host_mmap, r.dev), "doca_mmap_add_dev(host)");
    check_doca(doca_mmap_start(r.host_mmap), "doca_mmap_start(host)");
    check_doca(doca_mmap_dev_get_dpa_handle(r.host_mmap, r.dev, &r.host_dpa_mmap_handle),
               "doca_mmap_dev_get_dpa_handle(host)");

    r.local_dpa_buf_bytes = static_cast<size_t>(align_up(std::max<uint64_t>(8, cfg.max_payload), 8));
    check_doca(doca_dpa_mem_alloc(r.dpa, r.local_dpa_buf_bytes, &r.local_dpa_buf), "doca_dpa_mem_alloc");
    check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, 0, r.local_dpa_buf_bytes), "doca_dpa_memset");

    check_doca(doca_mmap_create(&r.local_dpa_mmap), "doca_mmap_create(dpa)");
    check_doca(doca_mmap_set_permissions(r.local_dpa_mmap,
                                         DOCA_ACCESS_FLAG_LOCAL_READ_WRITE | DOCA_ACCESS_FLAG_RDMA_READ |
                                             DOCA_ACCESS_FLAG_RDMA_WRITE),
               "doca_mmap_set_permissions(dpa)");
    check_doca(doca_mmap_set_dpa_memrange(r.local_dpa_mmap, r.dpa, r.local_dpa_buf, r.local_dpa_buf_bytes),
               "doca_mmap_set_dpa_memrange(dpa)");
    check_doca(doca_mmap_start(r.local_dpa_mmap), "doca_mmap_start(dpa)");
    check_doca(doca_mmap_dev_get_dpa_handle(r.local_dpa_mmap, r.dev, &r.local_dpa_mmap_handle),
               "doca_mmap_dev_get_dpa_handle(dpa)");
}

uint64_t run_copy_rpc(const Config &cfg, const Resources &r, uint32_t payload)
{
    uint64_t cycles = 0;
    check_doca(doca_dpa_rpc(r.dpa,
                            &dpa_copy_run_rpc,
                            &cycles,
                            r.dpa_handle,
                            r.dpa_qp_handle,
                            r.dpa_completion_handle,
                            r.local_dpa_buf,
                            r.local_dpa_mmap_handle,
                            reinterpret_cast<uint64_t>(r.host_buf),
                            r.host_dpa_mmap_handle,
                            cfg.mode,
                            cfg.warmup,
                            cfg.iters,
                            payload),
               "doca_dpa_rpc(dpa_copy_run_rpc)");
    return cycles;
}

void init_doca_log()
{
    check_doca(doca_log_backend_create_standard(), "doca_log_backend_create_standard");

    struct doca_log_backend *sdk_log = nullptr;
    check_doca(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
    check_doca(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_WARNING), "doca_log_backend_set_sdk_level");
}

void run_benchmark(const Config &cfg)
{
    Resources res{};

    try {
        init_resources(cfg, res);

        const auto payload_list = build_payload_list(cfg);
        const uint64_t total_ops = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);

        std::cout << "mode=" << mode_to_string(cfg.mode)
                  << " device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " qp_depth=" << cfg.qp_depth
                  << " verify=" << (cfg.verify ? 1 : 0)
                  << "\n";
        std::cout << "payload_bytes,avg_latency_ns,bandwidth_GBps,total_cycles\n";

        for (uint64_t payload_u64 : payload_list) {
            uint32_t payload = static_cast<uint32_t>(payload_u64);
            prepare_payload_buffers(cfg, res, payload);

            if (cfg.mode == DPA_VERBS_COPY_MODE_SEND) {
                post_host_receives(res, payload, total_ops);
            }

            uint64_t cycles = run_copy_rpc(cfg, res, payload);

            if (cfg.mode == DPA_VERBS_COPY_MODE_SEND) {
                poll_host_recv_completions(res, total_ops);
            }

            verify_payload_result(cfg, res, payload);

            double avg_ns = (cfg.iters == 0)
                                ? 0.0
                                : (static_cast<double>(cycles) * 1000.0 /
                                   static_cast<double>(cfg.clock_mhz) /
                                   static_cast<double>(cfg.iters));
            double bandwidth_gbps = (avg_ns > 0.0)
                                        ? (static_cast<double>(payload) / avg_ns)
                                        : 0.0;

            std::cout << payload << "," << std::fixed << std::setprecision(2) << avg_ns << ","
                      << std::fixed << std::setprecision(3) << bandwidth_gbps << "," << cycles << "\n";
        }

        cleanup_resources(res);
    } catch (...) {
        cleanup_resources(res);
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
