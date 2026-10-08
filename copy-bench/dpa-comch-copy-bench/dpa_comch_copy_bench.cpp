#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <doca_comch_consumer.h>
#include <doca_comch_msgq.h>
#include <doca_comch_producer.h>
#include <doca_buf.h>
#include <doca_buf_inventory.h>
#include <doca_ctx.h>
#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_mmap.h>
#include <doca_pe.h>

#include "host/dpa_comch_copy_common.h"

extern "C" {
doca_dpa_func_t dpa_comch_copy_run_rpc;
}

extern struct doca_dpa_app *DPA_APP_NAME;

DOCA_LOG_REGISTER(DPA_COMCH_COPY_BENCH);

namespace {

constexpr uint64_t kDefaultMinPayload = 64;
constexpr uint64_t kDefaultMaxPayload = 1ULL << 20; // 1 MiB
constexpr double kDefaultSizeFactor = 2.0;
constexpr uint32_t kDefaultWarmup = 20;
constexpr uint32_t kDefaultIters = 200;
constexpr uint32_t kDefaultClockMHz = 1800;
constexpr uint32_t kDefaultSeed = 1;
constexpr uint32_t kDefaultQueueDepth = 0;
constexpr uint32_t kDefaultImmBytes = sizeof(struct dpa_comch_imm_payload);
constexpr uint32_t kDefaultOpBytes = 8 * 1024;
constexpr uint32_t kMaxDpaComchDmaCopyBytes = 8 * 1024;
constexpr uint32_t kMaxDpaComchDmaQueueDepth = 8 * 1024;
constexpr uint32_t kVerifyChunkBytes = 64 * 1024;
constexpr size_t kHostAlignment = 4096;
constexpr size_t kCopyAlignment = 64;
constexpr size_t kConsumerRecvSlotBytes = 64;

struct Config {
    std::string device_name = "mlx5_0";
    uint32_t direction = DPA_COMCH_COPY_DIR_H2D;

    uint64_t min_payload = kDefaultMinPayload;
    uint64_t max_payload = kDefaultMaxPayload;
    double size_factor = kDefaultSizeFactor;

    uint32_t warmup = kDefaultWarmup;
    uint32_t iters = kDefaultIters;
    uint32_t clock_mhz = kDefaultClockMHz;
    uint32_t queue_depth = kDefaultQueueDepth;
    uint32_t imm_bytes = kDefaultImmBytes;
    uint32_t op_bytes = kDefaultOpBytes;
    uint32_t seed = kDefaultSeed;
    bool verify = false;
};

struct ConsumerStats {
    std::atomic<uint64_t> completed{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> imm_len_errors{0};
    std::atomic<uint64_t> imm_checksum{0};
    uint32_t expected_imm_bytes = 0;
};

struct Resources {
    doca_dev *dev = nullptr;
    doca_dpa *dpa = nullptr;
    doca_dpa_dev_t dpa_handle = 0;

    doca_pe *pe = nullptr;
    doca_comch_msgq *msgq = nullptr;
    doca_comch_consumer *consumer = nullptr;
    doca_comch_producer *producer = nullptr;
    doca_ctx *consumer_ctx = nullptr;
    doca_ctx *producer_ctx = nullptr;

    void *consumer_recv_mem = nullptr;
    size_t consumer_recv_mem_bytes = 0;
    size_t consumer_recv_slot_bytes = 0;
    doca_mmap *consumer_recv_mmap = nullptr;
    doca_buf_inventory *consumer_recv_inventory = nullptr;

    doca_dpa_completion *producer_completion = nullptr;
    doca_dpa_dev_completion_t producer_completion_handle = 0;
    doca_dpa_dev_comch_producer_t dpa_producer_handle = 0;
    uint32_t consumer_id = 0;
    uint32_t queue_depth = 0;

    void *host_mem_base = nullptr;
    size_t host_mem_bytes = 0;
    size_t host_region_bytes = 0;
    uint8_t *host_src = nullptr;
    uint8_t *host_dst = nullptr;
    doca_mmap *host_mmap = nullptr;
    doca_dpa_dev_mmap_t host_dpa_mmap_handle = 0;

    doca_dpa_dev_uintptr_t local_dpa_buf = 0;
    size_t local_dpa_buf_bytes = 0;
    doca_mmap *local_dpa_mmap = nullptr;
    doca_dpa_dev_mmap_t local_dpa_mmap_handle = 0;

    uint8_t *verify_buf = nullptr;
    size_t verify_buf_bytes = 0;

    ConsumerStats consumer_stats{};
    std::atomic<bool> progress_thread_run{false};
    std::thread progress_thread;
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

uint64_t align_up(uint64_t value, uint64_t align)
{
    if (align == 0) {
        return value;
    }
    const uint64_t rem = value % align;
    if (rem == 0) {
        return value;
    }
    return value + (align - rem);
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

uint32_t parse_direction(const std::string &s)
{
    if (s == "h2d") {
        return DPA_COMCH_COPY_DIR_H2D;
    }
    if (s == "d2h") {
        return DPA_COMCH_COPY_DIR_D2H;
    }
    throw_runtime("Unsupported direction: " + s + " (expected: h2d|d2h)");
}

const char *direction_to_string(uint32_t direction)
{
    switch (direction) {
    case DPA_COMCH_COPY_DIR_H2D:
        return "h2d";
    case DPA_COMCH_COPY_DIR_D2H:
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
        << "  --dir h2d|d2h                   Copy direction (default: h2d)\n"
        << "  --min-payload <bytes>           Min payload bytes (default: 64)\n"
        << "  --max-payload <bytes>           Max payload bytes (default: 1048576)\n"
        << "  --size-factor <float>           Payload growth factor (default: 2.0)\n"
        << "  --warmup <num>                  Warmup iterations per payload (default: 20)\n"
        << "  --iters <num>                   Measured iterations per payload (default: 200)\n"
        << "  --queue-depth <num>             Message queue depth (0 = max safe supported, default: 0)\n"
        << "  --imm-bytes <num>               Immediate payload bytes (1..32, default: 8)\n"
        << "  --op-bytes <num>                Max bytes per Comch DMA op (default: 8192)\n"
        << "  --clock-mhz <num>               DPA clock MHz for ns conversion (default: 1800)\n"
        << "  --seed <num>                    Seed for deterministic fill pattern (default: 1)\n"
        << "  --verify                        Verify payload content after each run\n";
}

Config parse_args(int argc, char **argv)
{
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw_runtime(std::string("Missing value for ") + name);
            }
            return std::string(argv[++i]);
        };

        if (arg == "--device") {
            cfg.device_name = next("--device");
        } else if (arg == "--dir") {
            cfg.direction = parse_direction(next("--dir"));
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
        } else if (arg == "--queue-depth") {
            cfg.queue_depth = static_cast<uint32_t>(parse_u64(next("--queue-depth"), "--queue-depth"));
        } else if (arg == "--imm-bytes") {
            cfg.imm_bytes = static_cast<uint32_t>(parse_u64(next("--imm-bytes"), "--imm-bytes"));
        } else if (arg == "--op-bytes") {
            cfg.op_bytes = static_cast<uint32_t>(parse_u64(next("--op-bytes"), "--op-bytes"));
        } else if (arg == "--clock-mhz") {
            cfg.clock_mhz = static_cast<uint32_t>(parse_u64(next("--clock-mhz"), "--clock-mhz"));
        } else if (arg == "--seed") {
            cfg.seed = static_cast<uint32_t>(parse_u64(next("--seed"), "--seed"));
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
    if (cfg.max_payload > std::numeric_limits<uint32_t>::max()) {
        throw_runtime("--max-payload must fit in uint32");
    }
    if (cfg.size_factor <= 1.0) {
        throw_runtime("--size-factor must be > 1.0");
    }
    if (cfg.clock_mhz == 0) {
        throw_runtime("--clock-mhz must be > 0");
    }
    if (cfg.imm_bytes == 0 || cfg.imm_bytes > 32) {
        throw_runtime("--imm-bytes must be in range [1, 32]");
    }
    if (cfg.op_bytes == 0) {
        throw_runtime("--op-bytes must be > 0");
    }
    if (cfg.op_bytes > kMaxDpaComchDmaCopyBytes) {
        throw_runtime("--op-bytes must be <= " + std::to_string(kMaxDpaComchDmaCopyBytes) +
                      " for DPA Comch producer DMA copy");
    }

    const uint64_t total_iters = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
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

void open_device_by_ibdev_name(const std::string &device_name, doca_dev **dev)
{
    struct doca_devinfo **devinfo_list = nullptr;
    uint32_t nb_devs = 0;
    check_doca(doca_devinfo_create_list(&devinfo_list, &nb_devs), "doca_devinfo_create_list");

    char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
    bool found = false;

    for (uint32_t i = 0; i < nb_devs; ++i) {
        const doca_error_t st = doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE);
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

void consumer_recv_task_done(struct doca_comch_consumer_task_post_recv *task,
                             union doca_data,
                             union doca_data ctx_user_data)
{
    auto *stats = static_cast<ConsumerStats *>(ctx_user_data.ptr);

    const uint32_t imm_len = doca_comch_consumer_task_post_recv_get_imm_data_len(task);
    if (imm_len != stats->expected_imm_bytes) {
        stats->imm_len_errors.fetch_add(1, std::memory_order_relaxed);
    }

    const uint8_t *imm = doca_comch_consumer_task_post_recv_get_imm_data(task);
    uint64_t sample = 0;
    if (imm != nullptr && imm_len > 0) {
        const uint32_t copy_bytes = std::min<uint32_t>(imm_len, sizeof(sample));
        std::memcpy(&sample, imm, copy_bytes);
    }
    stats->imm_checksum.fetch_xor(sample, std::memory_order_relaxed);
    stats->completed.fetch_add(1, std::memory_order_relaxed);

    struct doca_buf *buf = doca_comch_consumer_task_post_recv_get_buf(task);
    if (buf != nullptr) {
        (void)doca_buf_dec_refcount(buf, nullptr);
    }
    doca_task_free(doca_comch_consumer_task_post_recv_as_task(task));
}

void consumer_recv_task_error(struct doca_comch_consumer_task_post_recv *task,
                              union doca_data task_user_data,
                              union doca_data ctx_user_data)
{
    auto *stats = static_cast<ConsumerStats *>(ctx_user_data.ptr);
    stats->errors.fetch_add(1, std::memory_order_relaxed);
    consumer_recv_task_done(task, task_user_data, ctx_user_data);
}

void stop_ctx_with_progress(doca_ctx *ctx, doca_pe *pe)
{
    if (ctx == nullptr) {
        return;
    }

    const doca_error_t st = doca_ctx_stop(ctx);
    if (st == DOCA_SUCCESS || st == DOCA_ERROR_BAD_STATE) {
        return;
    }
    if (st != DOCA_ERROR_IN_PROGRESS) {
        throw_runtime(std::string("doca_ctx_stop failed: ") + doca_error_get_descr(st));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        enum doca_ctx_states state = DOCA_CTX_STATE_IDLE;
        if (doca_ctx_get_state(ctx, &state) == DOCA_SUCCESS && state == DOCA_CTX_STATE_IDLE) {
            return;
        }
        if (pe != nullptr) {
            (void)doca_pe_progress(pe);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    throw_runtime("Timed out waiting for context to stop");
}

void stop_progress_thread(Resources &r)
{
    r.progress_thread_run.store(false, std::memory_order_relaxed);
    if (r.progress_thread.joinable()) {
        r.progress_thread.join();
    }
}

void cleanup(Resources &r)
{
    stop_progress_thread(r);

    if (r.producer != nullptr) {
        stop_ctx_with_progress(r.producer_ctx, r.pe);
        (void)doca_comch_producer_destroy(r.producer);
        r.producer = nullptr;
        r.producer_ctx = nullptr;
    }

    if (r.consumer != nullptr) {
        stop_ctx_with_progress(r.consumer_ctx, r.pe);
        (void)doca_comch_consumer_destroy(r.consumer);
        r.consumer = nullptr;
        r.consumer_ctx = nullptr;
    }

    if (r.msgq != nullptr) {
        (void)doca_comch_msgq_stop(r.msgq);
        (void)doca_comch_msgq_destroy(r.msgq);
        r.msgq = nullptr;
    }

    if (r.consumer_recv_inventory != nullptr) {
        (void)doca_buf_inventory_destroy(r.consumer_recv_inventory);
        r.consumer_recv_inventory = nullptr;
    }

    if (r.consumer_recv_mmap != nullptr) {
        (void)doca_mmap_destroy(r.consumer_recv_mmap);
        r.consumer_recv_mmap = nullptr;
    }

    if (r.consumer_recv_mem != nullptr) {
        free(r.consumer_recv_mem);
        r.consumer_recv_mem = nullptr;
        r.consumer_recv_mem_bytes = 0;
        r.consumer_recv_slot_bytes = 0;
    }

    if (r.producer_completion != nullptr) {
        (void)doca_dpa_completion_stop(r.producer_completion);
        (void)doca_dpa_completion_destroy(r.producer_completion);
        r.producer_completion = nullptr;
    }

    if (r.local_dpa_mmap != nullptr) {
        (void)doca_mmap_destroy(r.local_dpa_mmap);
        r.local_dpa_mmap = nullptr;
    }

    if (r.local_dpa_buf != 0 && r.dpa != nullptr) {
        (void)doca_dpa_mem_free(r.dpa, r.local_dpa_buf);
        r.local_dpa_buf = 0;
    }

    if (r.host_mmap != nullptr) {
        (void)doca_mmap_destroy(r.host_mmap);
        r.host_mmap = nullptr;
    }

    if (r.verify_buf != nullptr) {
        free(r.verify_buf);
        r.verify_buf = nullptr;
    }

    if (r.host_mem_base != nullptr) {
        free(r.host_mem_base);
        r.host_mem_base = nullptr;
        r.host_src = nullptr;
        r.host_dst = nullptr;
    }

    if (r.pe != nullptr) {
        (void)doca_pe_destroy(r.pe);
        r.pe = nullptr;
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
}

void start_progress_thread(Resources &r)
{
    r.progress_thread_run.store(true, std::memory_order_relaxed);
    r.progress_thread = std::thread([&r]() {
        while (r.progress_thread_run.load(std::memory_order_relaxed)) {
            if (doca_pe_progress(r.pe) == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });
}

void init_resources(const Config &cfg, Resources &r)
{
    open_device_by_ibdev_name(cfg.device_name, &r.dev);

    const struct doca_devinfo *devinfo = doca_dev_as_devinfo(r.dev);
    if (devinfo == nullptr) {
        throw_runtime("doca_dev_as_devinfo returned null");
    }

    uint32_t producer_max_buf = 0;
    uint32_t consumer_max_buf = 0;
    uint32_t consumer_max_imm = 0;
    uint32_t producer_max_tasks = 0;
    uint32_t consumer_max_tasks = 0;
    check_doca(doca_comch_producer_cap_get_max_buf_size(devinfo, &producer_max_buf),
               "doca_comch_producer_cap_get_max_buf_size");
    check_doca(doca_comch_consumer_cap_get_max_buf_size(devinfo, &consumer_max_buf),
               "doca_comch_consumer_cap_get_max_buf_size");
    check_doca(doca_comch_consumer_cap_get_max_imm_data_len(devinfo, &consumer_max_imm),
               "doca_comch_consumer_cap_get_max_imm_data_len");
    check_doca(doca_comch_producer_cap_get_max_num_tasks(devinfo, &producer_max_tasks),
               "doca_comch_producer_cap_get_max_num_tasks");
    check_doca(doca_comch_consumer_cap_get_max_num_tasks(devinfo, &consumer_max_tasks),
               "doca_comch_consumer_cap_get_max_num_tasks");

    const uint32_t comch_max_payload = std::min(producer_max_buf, consumer_max_buf);
    if (cfg.op_bytes > comch_max_payload) {
        throw_runtime("--op-bytes exceeds Comch supported max payload per DMA op on this device: " +
                      std::to_string(comch_max_payload));
    }
    if (cfg.imm_bytes > consumer_max_imm) {
        throw_runtime("--imm-bytes exceeds Comch supported immediate length on this device: " +
                      std::to_string(consumer_max_imm));
    }
    const uint32_t max_supported_tasks = std::min(producer_max_tasks, consumer_max_tasks);
    const uint32_t max_safe_queue_depth = std::min(max_supported_tasks, kMaxDpaComchDmaQueueDepth);
    r.queue_depth = (cfg.queue_depth == 0) ? max_safe_queue_depth : cfg.queue_depth;
    if (r.queue_depth == 0) {
        throw_runtime("Comch reports zero supported tasks");
    }
    if (cfg.queue_depth > max_safe_queue_depth) {
        throw_runtime("--queue-depth exceeds the safe DPA Comch DMA producer queue depth on this device: " +
                      std::to_string(max_safe_queue_depth));
    }

    check_doca(doca_dpa_create(r.dev, &r.dpa), "doca_dpa_create");
    check_doca(doca_dpa_set_app(r.dpa, DPA_APP_NAME), "doca_dpa_set_app");
    check_doca(doca_dpa_start(r.dpa), "doca_dpa_start");
    check_doca(doca_dpa_get_dpa_handle(r.dpa, &r.dpa_handle), "doca_dpa_get_dpa_handle");

    check_doca(doca_pe_create(&r.pe), "doca_pe_create");

    check_doca(doca_comch_msgq_create(r.dev, &r.msgq), "doca_comch_msgq_create");
    check_doca(doca_comch_msgq_set_max_num_consumers(r.msgq, 1), "doca_comch_msgq_set_max_num_consumers");
    check_doca(doca_comch_msgq_set_max_num_producers(r.msgq, 1), "doca_comch_msgq_set_max_num_producers");
    check_doca(doca_comch_msgq_set_dpa_producer(r.msgq, r.dpa), "doca_comch_msgq_set_dpa_producer");
    check_doca(doca_comch_msgq_start(r.msgq), "doca_comch_msgq_start");

    check_doca(doca_comch_msgq_consumer_create(r.msgq, &r.consumer), "doca_comch_msgq_consumer_create");
    r.consumer_ctx = doca_comch_consumer_as_ctx(r.consumer);
    if (r.consumer_ctx == nullptr) {
        throw_runtime("doca_comch_consumer_as_ctx returned null");
    }

    r.consumer_stats.expected_imm_bytes = cfg.imm_bytes;
    union doca_data consumer_ud{};
    consumer_ud.ptr = &r.consumer_stats;

    check_doca(doca_comch_consumer_set_imm_data_len(r.consumer, cfg.imm_bytes), "doca_comch_consumer_set_imm_data_len");
    check_doca(doca_ctx_set_user_data(r.consumer_ctx, consumer_ud), "doca_ctx_set_user_data(consumer)");
    check_doca(doca_comch_consumer_task_post_recv_set_conf(r.consumer,
                                                            consumer_recv_task_done,
                                                            consumer_recv_task_error,
                                                            r.queue_depth),
               "doca_comch_consumer_task_post_recv_set_conf");
    check_doca(doca_pe_connect_ctx(r.pe, r.consumer_ctx), "doca_pe_connect_ctx(consumer)");
    check_doca(doca_ctx_start(r.consumer_ctx), "doca_ctx_start(consumer)");
    check_doca(doca_comch_consumer_get_id(r.consumer, &r.consumer_id), "doca_comch_consumer_get_id");

    r.consumer_recv_slot_bytes = align_up(std::max<size_t>(cfg.imm_bytes, kConsumerRecvSlotBytes), kCopyAlignment);
    r.consumer_recv_mem_bytes = r.consumer_recv_slot_bytes * r.queue_depth;
    void *consumer_recv_mem = nullptr;
    if (posix_memalign(&consumer_recv_mem, kHostAlignment, r.consumer_recv_mem_bytes) != 0 ||
        consumer_recv_mem == nullptr) {
        throw_runtime("posix_memalign failed for consumer receive memory");
    }
    r.consumer_recv_mem = consumer_recv_mem;
    std::memset(r.consumer_recv_mem, 0, r.consumer_recv_mem_bytes);

    check_doca(doca_buf_inventory_create(r.queue_depth, &r.consumer_recv_inventory),
               "doca_buf_inventory_create(consumer recv)");
    check_doca(doca_buf_inventory_start(r.consumer_recv_inventory),
               "doca_buf_inventory_start(consumer recv)");
    check_doca(doca_mmap_create(&r.consumer_recv_mmap), "doca_mmap_create(consumer recv)");
    check_doca(doca_mmap_add_dev(r.consumer_recv_mmap, r.dev), "doca_mmap_add_dev(consumer recv)");
    check_doca(doca_mmap_set_permissions(r.consumer_recv_mmap, DOCA_ACCESS_FLAG_PCI_READ_WRITE),
               "doca_mmap_set_permissions(consumer recv)");
    check_doca(doca_mmap_set_memrange(r.consumer_recv_mmap, r.consumer_recv_mem, r.consumer_recv_mem_bytes),
               "doca_mmap_set_memrange(consumer recv)");
    check_doca(doca_mmap_start(r.consumer_recv_mmap), "doca_mmap_start(consumer recv)");

    check_doca(doca_dpa_completion_create(r.dpa, r.queue_depth, &r.producer_completion), "doca_dpa_completion_create");
    check_doca(doca_dpa_completion_start(r.producer_completion), "doca_dpa_completion_start");
    check_doca(doca_dpa_completion_get_dpa_handle(r.producer_completion, &r.producer_completion_handle),
               "doca_dpa_completion_get_dpa_handle");

    check_doca(doca_comch_msgq_producer_create(r.msgq, &r.producer), "doca_comch_msgq_producer_create");
    r.producer_ctx = doca_comch_producer_as_ctx(r.producer);
    if (r.producer_ctx == nullptr) {
        throw_runtime("doca_comch_producer_as_ctx returned null");
    }
    check_doca(doca_ctx_set_datapath_on_dpa(r.producer_ctx, r.dpa), "doca_ctx_set_datapath_on_dpa(producer)");
    check_doca(doca_comch_producer_set_dev_max_num_send(r.producer, r.queue_depth),
               "doca_comch_producer_set_dev_max_num_send");
    check_doca(doca_comch_producer_dpa_completion_attach(r.producer, r.producer_completion),
               "doca_comch_producer_dpa_completion_attach");
    check_doca(doca_ctx_start(r.producer_ctx), "doca_ctx_start(producer)");
    check_doca(doca_comch_producer_get_dpa_handle(r.producer, &r.dpa_producer_handle), "doca_comch_producer_get_dpa_handle");

    r.host_region_bytes =
        static_cast<size_t>(align_up(std::max<uint64_t>(cfg.max_payload, kCopyAlignment), kCopyAlignment));
    r.host_mem_bytes = r.host_region_bytes * 2;

    void *host_mem = nullptr;
    if (posix_memalign(&host_mem, kHostAlignment, r.host_mem_bytes) != 0 || host_mem == nullptr) {
        throw_runtime("posix_memalign failed for host memory");
    }
    r.host_mem_base = host_mem;
    std::memset(r.host_mem_base, 0, r.host_mem_bytes);
    r.host_src = static_cast<uint8_t *>(r.host_mem_base);
    r.host_dst = r.host_src + r.host_region_bytes;

    check_doca(doca_mmap_create(&r.host_mmap), "doca_mmap_create(host)");
    check_doca(doca_mmap_set_permissions(r.host_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
               "doca_mmap_set_permissions(host)");
    check_doca(doca_mmap_set_memrange(r.host_mmap, r.host_mem_base, r.host_mem_bytes), "doca_mmap_set_memrange(host)");
    check_doca(doca_mmap_add_dev(r.host_mmap, r.dev), "doca_mmap_add_dev(host)");
    check_doca(doca_mmap_start(r.host_mmap), "doca_mmap_start(host)");
    check_doca(doca_mmap_dev_get_dpa_handle(r.host_mmap, r.dev, &r.host_dpa_mmap_handle),
               "doca_mmap_dev_get_dpa_handle(host)");

    r.local_dpa_buf_bytes = static_cast<size_t>(align_up(std::max<uint64_t>(cfg.max_payload, 8), 8));
    check_doca(doca_dpa_mem_alloc(r.dpa, r.local_dpa_buf_bytes, &r.local_dpa_buf), "doca_dpa_mem_alloc");
    check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, 0, r.local_dpa_buf_bytes), "doca_dpa_memset(local)");

    check_doca(doca_mmap_create(&r.local_dpa_mmap), "doca_mmap_create(local dpa)");
    check_doca(doca_mmap_set_permissions(r.local_dpa_mmap, DOCA_ACCESS_FLAG_LOCAL_READ_WRITE),
               "doca_mmap_set_permissions(local dpa)");
    check_doca(doca_mmap_set_dpa_memrange(r.local_dpa_mmap, r.dpa, r.local_dpa_buf, r.local_dpa_buf_bytes),
               "doca_mmap_set_dpa_memrange(local dpa)");
    check_doca(doca_mmap_start(r.local_dpa_mmap), "doca_mmap_start(local dpa)");
    check_doca(doca_mmap_dev_get_dpa_handle(r.local_dpa_mmap, r.dev, &r.local_dpa_mmap_handle),
               "doca_mmap_dev_get_dpa_handle(local dpa)");

    r.verify_buf_bytes = r.host_region_bytes;
    void *verify_mem = nullptr;
    if (posix_memalign(&verify_mem, kCopyAlignment, r.verify_buf_bytes) != 0 || verify_mem == nullptr) {
        throw_runtime("posix_memalign failed for verify buffer");
    }
    r.verify_buf = static_cast<uint8_t *>(verify_mem);

    start_progress_thread(r);
}

uint8_t fill_value_for_payload(const Config &cfg, uint32_t payload)
{
    return static_cast<uint8_t>((payload ^ (cfg.direction * 31U) ^ (cfg.seed * 17U)) & 0xFFU);
}

void prepare_payload_buffers(const Config &cfg, const Resources &r, uint32_t payload)
{
    const uint8_t fill = fill_value_for_payload(cfg, payload);

    if (cfg.direction == DPA_COMCH_COPY_DIR_H2D) {
        std::memset(r.host_src, fill, payload);
        check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, 0, payload), "doca_dpa_memset(h2d dst)");
    } else {
        std::memset(r.host_dst, 0, payload);
        check_doca(doca_dpa_memset(r.dpa, r.local_dpa_buf, fill, payload), "doca_dpa_memset(d2h src)");
    }
}

void verify_payload(const Config &cfg, const Resources &r, uint32_t payload)
{
    if (!cfg.verify) {
        return;
    }

    const uint8_t expected_fill = fill_value_for_payload(cfg, payload);

    if (cfg.direction == DPA_COMCH_COPY_DIR_H2D) {
        std::memset(r.verify_buf, 0, payload);
        uint32_t offset = 0;
        while (offset < payload) {
            const uint32_t chunk = std::min<uint32_t>(kVerifyChunkBytes, payload - offset);
            check_doca(doca_dpa_d2h_memcpy(r.dpa, r.verify_buf + offset, r.local_dpa_buf + offset, chunk),
                       "doca_dpa_d2h_memcpy(h2d verify)");
            offset += chunk;
        }
        for (uint32_t i = 0; i < payload; ++i) {
            if (r.verify_buf[i] != expected_fill) {
                throw_runtime("verify(h2d) failed at byte " + std::to_string(i));
            }
        }
        return;
    }

    for (uint32_t i = 0; i < payload; ++i) {
        if (r.host_dst[i] != expected_fill) {
            throw_runtime("verify(d2h) failed at byte " + std::to_string(i));
        }
    }
}

void reset_consumer_stats(ConsumerStats &stats)
{
    stats.completed.store(0, std::memory_order_relaxed);
    stats.errors.store(0, std::memory_order_relaxed);
    stats.imm_len_errors.store(0, std::memory_order_relaxed);
    stats.imm_checksum.store(0, std::memory_order_relaxed);
}

void post_consumer_receives(Resources &r, uint64_t count)
{
    for (uint64_t i = 0; i < count; ++i) {
        uint8_t *slot = static_cast<uint8_t *>(r.consumer_recv_mem) + (i * r.consumer_recv_slot_bytes);
        struct doca_buf *buf = nullptr;
        check_doca(doca_buf_inventory_buf_get_by_addr(r.consumer_recv_inventory,
                                                      r.consumer_recv_mmap,
                                                      slot,
                                                      r.consumer_recv_slot_bytes,
                                                      &buf),
                   "doca_buf_inventory_buf_get_by_addr(consumer recv)");

        struct doca_comch_consumer_task_post_recv *recv_task = nullptr;
        try {
            check_doca(doca_comch_consumer_task_post_recv_alloc_init(r.consumer, buf, &recv_task),
                   "doca_comch_consumer_task_post_recv_alloc_init");
        } catch (...) {
            (void)doca_buf_dec_refcount(buf, nullptr);
            throw;
        }
        struct doca_task *task = doca_comch_consumer_task_post_recv_as_task(recv_task);
        try {
            check_doca(doca_task_submit(task), "doca_task_submit(post_recv)");
        } catch (...) {
            (void)doca_buf_dec_refcount(buf, nullptr);
            doca_task_free(task);
            throw;
        }
    }
}

void wait_for_consumer_completions(Resources &r, uint64_t expected)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (r.consumer_stats.completed.load(std::memory_order_relaxed) < expected) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw_runtime("Timed out waiting for comch consumer completions");
        }
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    if (r.consumer_stats.errors.load(std::memory_order_relaxed) != 0) {
        throw_runtime("Comch consumer completion reported task errors");
    }
    if (r.consumer_stats.imm_len_errors.load(std::memory_order_relaxed) != 0) {
        throw_runtime("Comch consumer completion immediate-length mismatch");
    }
}

uint64_t run_copy_rpc(const Config &cfg,
                      const Resources &r,
                      uint32_t payload,
                      uint32_t warmup_iters,
                      uint32_t measure_iters)
{
    uint64_t cycles = 0;
    check_doca(doca_dpa_rpc(r.dpa,
                            &dpa_comch_copy_run_rpc,
                            &cycles,
                            r.dpa_handle,
                            r.dpa_producer_handle,
                            r.producer_completion_handle,
                            r.consumer_id,
                            r.host_dpa_mmap_handle,
                            reinterpret_cast<uint64_t>(r.host_src),
                            reinterpret_cast<uint64_t>(r.host_dst),
                            r.local_dpa_mmap_handle,
                            r.local_dpa_buf,
                            cfg.direction,
                            warmup_iters,
                            measure_iters,
                            payload,
                            cfg.imm_bytes,
                            cfg.op_bytes),
               "doca_dpa_rpc(dpa_comch_copy_run_rpc)");
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
    Resources r{};

    try {
        init_resources(cfg, r);

        const auto payloads = build_payload_list(cfg);

        std::cout << "mode=comch_dma_copy"
                  << " direction=" << direction_to_string(cfg.direction)
                  << " device=" << cfg.device_name
                  << " warmup=" << cfg.warmup
                  << " iters=" << cfg.iters
                  << " queue_depth=" << r.queue_depth
                  << " imm_bytes=" << cfg.imm_bytes
                  << " op_bytes=" << cfg.op_bytes
                  << " max_dpa_comch_dma_copy_bytes=" << kMaxDpaComchDmaCopyBytes
                  << " max_dpa_comch_dma_queue_depth=" << kMaxDpaComchDmaQueueDepth
                  << " verify=" << (cfg.verify ? 1 : 0)
                  << "\n";
        std::cout << "payload_bytes,avg_latency_ns,bandwidth_GBps,total_cycles\n";

        for (const uint64_t payload_u64 : payloads) {
            const uint32_t payload = static_cast<uint32_t>(payload_u64);
            const uint64_t msgs_per_iter = (payload + (uint64_t)cfg.op_bytes - 1ULL) / (uint64_t)cfg.op_bytes;
            if (msgs_per_iter > r.queue_depth) {
                throw_runtime("queue depth is too small for one payload iteration: payload=" +
                              std::to_string(payload) + " op_bytes=" + std::to_string(cfg.op_bytes) +
                              " requires " + std::to_string(msgs_per_iter) + " receive tasks");
            }
            const uint32_t max_iters_per_batch = static_cast<uint32_t>(r.queue_depth / msgs_per_iter);

            prepare_payload_buffers(cfg, r, payload);

            auto run_batches = [&](uint32_t iters) -> uint64_t {
                uint32_t remaining = iters;
                uint64_t phase_cycles = 0;

                while (remaining > 0) {
                    const uint32_t batch_iters = std::min<uint32_t>(remaining, max_iters_per_batch);
                    const uint64_t batch_msgs = msgs_per_iter * (uint64_t)batch_iters;

                    reset_consumer_stats(r.consumer_stats);
                    post_consumer_receives(r, batch_msgs);
                    phase_cycles += run_copy_rpc(cfg, r, payload, 0, batch_iters);
                    wait_for_consumer_completions(r, batch_msgs);

                    remaining -= batch_iters;
                }

                return phase_cycles;
            };

            (void)run_batches(cfg.warmup);
            const uint64_t cycles = run_batches(cfg.iters);
            verify_payload(cfg, r, payload);

            const double avg_ns = (cfg.iters == 0)
                                      ? 0.0
                                      : (static_cast<double>(cycles) * 1000.0 /
                                         static_cast<double>(cfg.clock_mhz) /
                                         static_cast<double>(cfg.iters));
            const double bandwidth_gbps = (avg_ns > 0.0) ? (static_cast<double>(payload) / avg_ns) : 0.0;

            std::cout << payload << "," << std::fixed << std::setprecision(2) << avg_ns << ","
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
        init_doca_log();
        const Config cfg = parse_args(argc, argv);
        run_benchmark(cfg);
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
}
