#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <doca_dev.h>
#include <doca_dpa.h>
#include <doca_error.h>
#include <doca_log.h>
#include <doca_sync_event.h>

#include "host/dpa_compute_tput_samecore_common.h"

extern "C" {
#define DECL_K(name) extern doca_dpa_func_t name;
DECL_K(k_tput_samecore_u64_add) DECL_K(k_tput_samecore_u64_mul) DECL_K(k_tput_multicore_u64_div)
DECL_K(k_tput_samecore_u64_max)
DECL_K(k_tput_samecore_u32_add) DECL_K(k_tput_samecore_u32_mul) DECL_K(k_tput_samecore_u32_div)
DECL_K(k_tput_samecore_u32_max)
DECL_K(k_tput_samecore_u8_add) DECL_K(k_tput_samecore_u8_mul) DECL_K(k_tput_samecore_u8_div)
DECL_K(k_tput_samecore_u8_max)
DECL_K(k_tput_multicore_start)
#undef DECL_K
}

extern struct doca_dpa_app *DPA_APP_NAME;
DOCA_LOG_REGISTER(DPA_TPUT_MULTICORE_BENCH);

namespace {

constexpr uint32_t kClockMhz = 1800;
constexpr uint32_t kLanes = 10;
constexpr uint32_t kRepeats = 64;
constexpr uint32_t kFactor = kLanes * kRepeats;

struct KernelCase {
	const char *dtype;
	uint32_t bits;
	const char *op;
	doca_dpa_func_t *fn;
};

constexpr KernelCase kKernelCases[] = {
	{"u8", 8, "add", &k_tput_samecore_u8_add},
	{"u8", 8, "mul", &k_tput_samecore_u8_mul},
	{"u8", 8, "div", &k_tput_samecore_u8_div},
	{"u8", 8, "max", &k_tput_samecore_u8_max},
	{"u32", 32, "add", &k_tput_samecore_u32_add},
	{"u32", 32, "mul", &k_tput_samecore_u32_mul},
	{"u32", 32, "div", &k_tput_samecore_u32_div},
	{"u32", 32, "max", &k_tput_samecore_u32_max},
	{"u64", 64, "add", &k_tput_samecore_u64_add},
	{"u64", 64, "mul", &k_tput_samecore_u64_mul},
	{"u64", 64, "div", &k_tput_multicore_u64_div},
	{"u64", 64, "max", &k_tput_samecore_u64_max},
};

const std::set<std::string> kTypes = {"u8", "u32", "u64"};
const std::set<std::string> kOps = {"add", "mul", "div", "max"};

std::string to_lower_ascii(std::string s)
{
	for (char &c : s) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return s;
}

std::set<std::string> parse_selection(const std::string &csv, const std::set<std::string> &allowed,
				      const char *name)
{
	if (csv == "all") {
		return allowed;
	}
	std::set<std::string> selected;
	std::stringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ',')) {
		token = to_lower_ascii(token);
		if (allowed.count(token) == 0) {
			throw std::runtime_error(std::string("Invalid ") + name + ": " + token);
		}
		selected.insert(token);
	}
	if (selected.empty()) {
		throw std::runtime_error(std::string("Empty ") + name + " set");
	}
	return selected;
}

std::string format_selection(const std::set<std::string> &values)
{
	std::ostringstream os;
	bool first = true;
	for (const auto &value : values) {
		if (!first) os << "|";
		os << value;
		first = false;
	}
	return os.str();
}

uint64_t parse_u64(const std::string &value, const char *name)
{
	if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); })) {
		throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
	}
	size_t end = 0;
	const uint64_t parsed = std::stoull(value, &end);
	if (end != value.size()) {
		throw std::runtime_error(std::string("Invalid ") + name + ": " + value);
	}
	return parsed;
}

uint32_t parse_u32(const std::string &value, const char *name)
{
	const uint64_t parsed = parse_u64(value, name);
	if (parsed > UINT32_MAX) {
		throw std::runtime_error(std::string("Out of range ") + name + ": " + value);
	}
	return static_cast<uint32_t>(parsed);
}

std::vector<uint32_t> parse_counts(const std::string &csv, uint32_t limit, const char *name)
{
	std::vector<uint32_t> values;
	std::stringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ',')) {
		if (token.empty()) {
			continue;
		}
		token = to_lower_ascii(token);
		if (token == "all") {
			values.push_back(0);
			continue;
		}
		const uint32_t value = parse_u32(token, name);
		if (value == 0 || value > limit) {
			throw std::runtime_error(std::string(name) + " values must be in [1," + std::to_string(limit) + "]");
		}
		values.push_back(value);
	}
	if (values.empty()) {
		throw std::runtime_error(std::string("Empty ") + name + " set");
	}
	std::sort(values.begin(), values.end(), [](uint32_t a, uint32_t b) {
		return (a == 0 ? 17 : a) < (b == 0 ? 17 : b);
	});
	values.erase(std::unique(values.begin(), values.end()), values.end());
	return values;
}

std::vector<uint32_t> parse_core_ids(const std::string &csv)
{
	std::vector<uint32_t> values;
	std::stringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ',')) {
		if (!token.empty()) {
			values.push_back(parse_u32(token, "core ID"));
		}
	}
	if (values.empty()) {
		throw std::runtime_error("Empty core ID list");
	}
	if (values.size() > 16) {
		throw std::runtime_error("--core-ids supports at most 16 cores");
	}
	std::vector<uint32_t> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
		throw std::runtime_error("--core-ids must not contain duplicate IDs");
	}
	return values;
}

std::string format_eu_list(const std::vector<uint32_t> &eus)
{
	std::ostringstream os;
	for (size_t i = 0; i < eus.size(); ++i) {
		if (i != 0) {
			os << "|";
		}
		os << eus[i];
	}
	return os.str();
}

struct Config {
	std::string device_name = "mlx5_0";
	std::string topology_file;
	uint32_t warmup = 10;
	uint32_t iters = 50;
	uint64_t op_iterations = 500;
	bool print_header = true;
	std::vector<uint32_t> core_ids;
	std::vector<uint32_t> core_counts = {1, 2, 4, 8, 0};
	std::vector<uint32_t> eus_per_core = {1, 2, 4, 8, 16};
	uint32_t control_eu = 0;
	std::set<std::string> types = kTypes;
	std::set<std::string> ops = kOps;
};

using CoreMap = std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>>;

[[noreturn]] void throw_runtime(const std::string &msg);

CoreMap load_topology(const std::string &path)
{
	std::ifstream input(path);
	std::string line;
	if (!input || !std::getline(input, line)) {
		throw_runtime("Invalid topology CSV: " + path);
	}
	if (!line.empty() && line.back() == '\r') {
		line.pop_back();
	}
	if (line != "eu_id,core_id,slot") {
		throw_runtime("Invalid topology CSV: " + path);
	}
	CoreMap cores;
	std::set<uint32_t> seen_eus;
	while (std::getline(input, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		std::stringstream ss(line);
		std::string field;
		uint32_t values[3] = {};
		for (uint32_t &value : values) {
			if (!std::getline(ss, field, ',')) {
				throw_runtime("Invalid topology row: " + line);
			}
			value = parse_u32(field, "topology value");
		}
		if (std::getline(ss, field, ',') || values[1] >= 16 || values[2] >= 16 ||
		    !seen_eus.insert(values[0]).second) {
			throw_runtime("Invalid or duplicate topology row: " + line);
		}
		cores[values[1]].push_back({values[2], values[0]});
	}
	if (cores.empty()) {
		throw_runtime("Topology CSV has no EUs");
	}
	for (auto &[core, eus] : cores) {
		(void)core;
		std::sort(eus.begin(), eus.end());
		for (size_t i = 1; i < eus.size(); ++i) {
			if (eus[i - 1].first == eus[i].first) {
				throw_runtime("Duplicate EU slot in topology CSV");
			}
		}
	}
	return cores;
}

struct Resources {
	doca_dev *dev = nullptr;
	doca_dpa *dpa = nullptr;
	doca_dpa_dev_t dpa_handle = 0;
	unsigned int num_cores = 0;
	unsigned int eus_per_core = 0;
	unsigned int total_eus = 0;
};

struct SyncEventResource {
	doca_sync_event *event = nullptr;
	doca_dpa_dev_sync_event_t dpa_handle = 0;
};

struct ThreadSet {
	doca_dpa_dev_uintptr_t args_dev = 0;
	doca_dpa_dev_uintptr_t results_dev = 0;
	doca_dpa_dev_uintptr_t notify_handles_dev = 0;
	doca_dpa_dev_uintptr_t start_flag_dev = 0;
	std::vector<doca_dpa_eu_affinity *> affinities;
	std::vector<doca_dpa_thread *> threads;
	std::vector<doca_dpa_notification_completion *> notify_comps;
	std::vector<dpa_tput_samecore_thread_result> host_results;
};

struct RunSummary {
	uint64_t host_window_ns = 0;
	uint64_t max_duration_cycles = 0;
	uint64_t instret_sum = 0;
	uint64_t checksum_sum = 0;
};

template <typename T>
T percentile_sorted(const std::vector<T> &sorted, double p)
{
	if (sorted.empty()) {
		return T{};
	}
	const double idx = p * static_cast<double>(sorted.size() - 1);
	const size_t lo = static_cast<size_t>(idx);
	const size_t hi = std::min(lo + 1, sorted.size() - 1);
	const double frac = idx - static_cast<double>(lo);
	return static_cast<T>(static_cast<double>(sorted[lo]) * (1.0 - frac) +
			      static_cast<double>(sorted[hi]) * frac);
}

template <typename T>
double mean_of(const std::vector<T> &values)
{
	if (values.empty()) {
		return 0.0;
	}
	return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

[[noreturn]] void throw_runtime(const std::string &msg) { throw std::runtime_error(msg); }

void check_doca(doca_error_t st, const char *what)
{
	if (st != DOCA_SUCCESS) {
		throw_runtime(std::string(what) + " failed: " + doca_error_get_descr(st));
	}
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
		} else if (arg == "--topology-file") {
			cfg.topology_file = require_value("--topology-file");
		} else if (arg == "--warmup") {
			cfg.warmup = parse_u32(require_value("--warmup"), "--warmup");
		} else if (arg == "--iters") {
			cfg.iters = parse_u32(require_value("--iters"), "--iters");
		} else if (arg == "--op-iterations") {
			cfg.op_iterations = parse_u64(require_value("--op-iterations"), "--op-iterations");
		} else if (arg == "--core-counts") {
			cfg.core_counts = parse_counts(require_value("--core-counts"), 16, "--core-counts");
		} else if (arg == "--eus-per-core") {
			cfg.eus_per_core = parse_counts(require_value("--eus-per-core"), 16, "--eus-per-core");
		} else if (arg == "--core-ids") {
			cfg.core_ids = parse_core_ids(require_value("--core-ids"));
		} else if (arg == "--control-eu") {
			cfg.control_eu = parse_u32(require_value("--control-eu"), "--control-eu");
		} else if (arg == "--types") {
			cfg.types = parse_selection(require_value("--types"), kTypes, "--types");
		} else if (arg == "--ops") {
			cfg.ops = parse_selection(require_value("--ops"), kOps, "--ops");
		} else if (arg == "--no-header") {
			cfg.print_header = false;
		} else if (arg == "-h" || arg == "--help") {
			std::cout
				<< "dpa_compute_tput_multicore_bench options:\n"
				<< "  --device-name <ibdev>      Device name (default: mlx5_0)\n"
				<< "  --topology-file <csv>     EU map from scripts/topology.py (required)\n"
				<< "  --warmup <num>             Warmup runs per case (default: 10)\n"
				<< "  --iters <num>              Measured runs per case (default: 50)\n"
				<< "  --op-iterations <num>      Outer loop iters per DPA thread (default: 500)\n"
				<< "  --core-counts <csv|all>   Visible core counts (default: 1,2,4,8,all)\n"
				<< "  --eus-per-core <csv|all>  EUs from each core (default: 1,2,4,8,16)\n"
					<< "  --core-ids <csv>          Select and order visible cores (default: all visible)\n"
					<< "  --control-eu <id>         EU excluded from workers (default: 0; does not bind RPC)\n"
					<< "  --types <csv|all>         u8,u32,u64 (default: all)\n"
					<< "  --ops <csv|all>           add,mul,div,max (default: all)\n"
					<< "  --no-header                Suppress CSV header line\n";
			std::exit(EXIT_SUCCESS);
		} else {
			throw_runtime("Unknown argument: " + arg);
		}
	}
	if (cfg.iters == 0) {
		throw_runtime("--iters must be > 0");
	}
	if (cfg.op_iterations == 0) {
		throw_runtime("--op-iterations must be > 0");
	}
	if (cfg.op_iterations > UINT64_MAX / (kFactor * 2)) {
		throw_runtime("--op-iterations overflows the logical operation count");
	}
	if (cfg.topology_file.empty()) {
		throw_runtime("--topology-file is required");
	}
	return cfg;
}

void init_doca_log()
{
	check_doca(doca_log_backend_create_standard(), "doca_log_backend_create_standard");
	struct doca_log_backend *sdk_log = nullptr;
	check_doca(doca_log_backend_create_with_file_sdk(stderr, &sdk_log), "doca_log_backend_create_with_file_sdk");
	check_doca(doca_log_backend_set_sdk_level(sdk_log, DOCA_LOG_LEVEL_ERROR), "doca_log_backend_set_sdk_level");
}

void open_device_by_ibdev_name(const std::string &device_name, doca_dev **dev)
{
	struct doca_devinfo **devinfo_list = nullptr;
	uint32_t nb_devs = 0;
	check_doca(doca_devinfo_create_list(&devinfo_list, &nb_devs), "doca_devinfo_create_list");

	char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE + 1] = {0};
	bool found = false;
	for (uint32_t i = 0; i < nb_devs; ++i) {
		if (doca_devinfo_get_ibdev_name(devinfo_list[i], ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) !=
		    DOCA_SUCCESS) {
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
	check_doca(doca_dpa_get_core_num(r.dpa, &r.num_cores), "doca_dpa_get_core_num");
	check_doca(doca_dpa_get_num_eus_per_core(r.dpa, &r.eus_per_core), "doca_dpa_get_num_eus_per_core");
	check_doca(doca_dpa_get_total_num_eus_available(r.dpa, &r.total_eus),
		   "doca_dpa_get_total_num_eus_available");
	if (r.eus_per_core == 0 || r.num_cores == 0 || r.total_eus == 0) {
		throw_runtime("Device reports an empty DPA topology");
	}
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

void create_thread_sync_event(const Resources &r, SyncEventResource &se)
{
	check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
	// keep these locality hints for this RPC and event-thread path
	// the reverse pairing failed on BF3
	check_doca(doca_sync_event_add_publisher_location_cpu(se.event, r.dev), "add_publisher_location_cpu");
	check_doca(doca_sync_event_add_subscriber_location_dpa(se.event, r.dpa), "add_subscriber_location_dpa");
	check_doca(doca_sync_event_start(se.event), "sync_event_start");
	check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle),
		   "sync_event_get_dpa_handle");
}

void wait_for_count(const SyncEventResource &se, uint64_t target, const char *name)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	for (;;) {
		uint64_t value = 0;
		check_doca(doca_sync_event_get(se.event, &value), "doca_sync_event_get(wait)");
		if (value == target) {
			return;
		}
		if (value > target || std::chrono::steady_clock::now() >= deadline) {
			throw_runtime(std::string(name) + " count mismatch or timeout: got " +
				      std::to_string(value) + ", expected " + std::to_string(target));
		}
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}

void cleanup_thread_set(const Resources &r, ThreadSet &ts)
{
	for (auto *notify_comp : ts.notify_comps) {
		if (notify_comp != nullptr) {
			(void)doca_dpa_notification_completion_stop(notify_comp);
			(void)doca_dpa_notification_completion_destroy(notify_comp);
		}
	}
	ts.notify_comps.clear();
	for (auto *thread : ts.threads) {
		if (thread != nullptr) {
			(void)doca_dpa_thread_destroy(thread);
		}
	}
	ts.threads.clear();
	for (auto *affinity : ts.affinities) {
		if (affinity != nullptr) {
			(void)doca_dpa_eu_affinity_destroy(affinity);
		}
	}
	ts.affinities.clear();
	if (ts.notify_handles_dev != 0) {
		(void)doca_dpa_mem_free(r.dpa, ts.notify_handles_dev);
		ts.notify_handles_dev = 0;
	}
	if (ts.start_flag_dev != 0) {
		(void)doca_dpa_mem_free(r.dpa, ts.start_flag_dev);
		ts.start_flag_dev = 0;
	}
	if (ts.results_dev != 0) {
		(void)doca_dpa_mem_free(r.dpa, ts.results_dev);
		ts.results_dev = 0;
	}
	if (ts.args_dev != 0) {
		(void)doca_dpa_mem_free(r.dpa, ts.args_dev);
		ts.args_dev = 0;
	}
	ts.host_results.clear();
}

void init_thread_set(const Resources &r, doca_dpa_func_t *worker, const std::vector<uint32_t> &eus,
		     uint64_t op_iterations, doca_dpa_dev_sync_event_t done_handle,
		     doca_dpa_dev_sync_event_t ready_handle, ThreadSet &ts)
{
	const uint32_t thread_count = static_cast<uint32_t>(eus.size());
	const size_t args_size = sizeof(dpa_tput_samecore_thread_arg) * thread_count;
	const size_t results_size = sizeof(dpa_tput_samecore_thread_result) * thread_count;
	const size_t handles_size = sizeof(doca_dpa_dev_notification_completion_t) * thread_count;

	check_doca(doca_dpa_mem_alloc(r.dpa, args_size, &ts.args_dev), "doca_dpa_mem_alloc(args)");
	check_doca(doca_dpa_mem_alloc(r.dpa, results_size, &ts.results_dev), "doca_dpa_mem_alloc(results)");
	check_doca(doca_dpa_mem_alloc(r.dpa, handles_size, &ts.notify_handles_dev),
		   "doca_dpa_mem_alloc(notify_handles)");
	check_doca(doca_dpa_mem_alloc(r.dpa, sizeof(uint64_t), &ts.start_flag_dev), "doca_dpa_mem_alloc(start_flag)");
	check_doca(doca_dpa_memset(r.dpa, ts.results_dev, 0, results_size), "doca_dpa_memset(results)");
	check_doca(doca_dpa_memset(r.dpa, ts.start_flag_dev, 0, sizeof(uint64_t)), "doca_dpa_memset(start_flag)");

	std::vector<dpa_tput_samecore_thread_arg> args(thread_count);
	for (uint32_t i = 0; i < thread_count; ++i) {
		args[i].dpa_ctx = r.dpa_handle;
		args[i].done_se = done_handle;
		args[i].ready_se = ready_handle;
		args[i].results_dev_ptr = ts.results_dev;
		args[i].start_flag_dev_ptr = ts.start_flag_dev;
		args[i].op_iterations = op_iterations;
		args[i].thread_index = i;
		args[i].reserved = 0;
	}
	check_doca(doca_dpa_h2d_memcpy(r.dpa, ts.args_dev, args.data(), args_size), "doca_dpa_h2d_memcpy(args)");

	ts.threads.assign(thread_count, nullptr);
	ts.notify_comps.assign(thread_count, nullptr);
	ts.affinities.assign(thread_count, nullptr);
	std::vector<doca_dpa_dev_notification_completion_t> notify_handles(thread_count, 0);

	for (uint32_t i = 0; i < thread_count; ++i) {
		check_doca(doca_dpa_eu_affinity_create(r.dpa, &ts.affinities[i]),
			   "doca_dpa_eu_affinity_create");
		check_doca(doca_dpa_eu_affinity_set(ts.affinities[i], eus[i]), "doca_dpa_eu_affinity_set");
		check_doca(doca_dpa_thread_create(r.dpa, &ts.threads[i]), "doca_dpa_thread_create");
		check_doca(doca_dpa_thread_set_func_arg(ts.threads[i], worker, 0),
			   "doca_dpa_thread_set_func_arg");
		check_doca(doca_dpa_thread_set_local_storage(
				   ts.threads[i], ts.args_dev + i * sizeof(dpa_tput_samecore_thread_arg)),
			   "doca_dpa_thread_set_local_storage");
		check_doca(doca_dpa_thread_set_affinity(ts.threads[i], ts.affinities[i]),
			   "doca_dpa_thread_set_affinity");
		check_doca(doca_dpa_thread_start(ts.threads[i]), "doca_dpa_thread_start");
		check_doca(doca_dpa_notification_completion_create(r.dpa, ts.threads[i], &ts.notify_comps[i]),
			   "doca_dpa_notification_completion_create");
		check_doca(doca_dpa_notification_completion_start(ts.notify_comps[i]),
			   "doca_dpa_notification_completion_start");
		check_doca(doca_dpa_notification_completion_get_dpa_handle(ts.notify_comps[i], &notify_handles[i]),
			   "doca_dpa_notification_completion_get_dpa_handle");
		check_doca(doca_dpa_thread_run(ts.threads[i]), "doca_dpa_thread_run");
	}
	check_doca(doca_dpa_h2d_memcpy(r.dpa, ts.notify_handles_dev, notify_handles.data(), handles_size),
		   "doca_dpa_h2d_memcpy(notify_handles)");
	ts.host_results.assign(thread_count, {});
}

uint64_t launch_workers_and_wait(const Resources &r, const SyncEventResource &ready,
				 const SyncEventResource &done, ThreadSet &ts,
				 uint32_t thread_count)
{
	uint64_t retval = 0;
	uint64_t ready_before = 0;
	uint64_t completed_before = 0;
	check_doca(doca_sync_event_get(ready.event, &ready_before), "doca_sync_event_get(ready)");
	check_doca(doca_sync_event_get(done.event, &completed_before), "doca_sync_event_get(done)");
	if (ready_before > UINT64_MAX - thread_count || completed_before > UINT64_MAX - thread_count) {
		throw_runtime("Sync event counter overflow");
	}
	const uint64_t ready_target = ready_before + thread_count;
	// host timing includes notification, the ready delay and completion polling
	const auto start = std::chrono::steady_clock::now();
	check_doca(doca_dpa_rpc(r.dpa, &k_tput_multicore_start, &retval, ts.notify_handles_dev,
				static_cast<uint64_t>(thread_count), ts.start_flag_dev,
				ready.dpa_handle, ready_target), "doca_dpa_rpc(start)");
	wait_for_count(done, completed_before + thread_count, "Completion");
	const auto end = std::chrono::steady_clock::now();
	if (retval != ready_target) {
		throw_runtime("Workers not ready after startup delay: ready counter " +
			      std::to_string(retval) + ", expected " + std::to_string(ready_target));
	}
	check_doca(doca_dpa_d2h_memcpy(r.dpa, ts.host_results.data(), ts.results_dev,
				       sizeof(dpa_tput_samecore_thread_result) * thread_count),
		   "doca_dpa_d2h_memcpy(results)");
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

RunSummary summarize_run(const std::vector<dpa_tput_samecore_thread_result> &results, uint64_t host_ns)
{
	RunSummary s{};
	s.host_window_ns = host_ns;
	for (const auto &r : results) {
		s.max_duration_cycles = std::max(s.max_duration_cycles, r.cycles);
		s.instret_sum += r.instret;
		s.checksum_sum += r.checksum;
	}
	return s;
}

uint64_t expected_checksum(const KernelCase &c, uint64_t op_iterations)
{
	const uint64_t steps = op_iterations * kRepeats;
	if (std::string(c.op) == "div") {
		const uint64_t dividend = c.bits == 8 ? 0xffULL :
			c.bits == 32 ? 0xffffffffULL : 0x7fffffffffffffffULL;
		return (dividend / 3) * kLanes + 3ULL;
	}
	uint64_t mul_factor = 1;
	if (std::string(c.op) == "mul") {
		uint64_t base = 3;
		uint64_t exponent = steps;
		while (exponent != 0) {
			if (exponent & 1) {
				mul_factor *= base;
			}
			base *= base;
			exponent >>= 1;
		}
	}
	uint64_t checksum = 3;
	for (uint64_t initial = 0x21; initial <= 0x2a; ++initial) {
		uint64_t value = initial;
		if (std::string(c.op) == "add") {
			value += steps * 3;
		} else if (std::string(c.op) == "mul") {
			value *= mul_factor;
		} else if (std::string(c.op) != "max") {
			throw_runtime("Unknown operation in checksum validation");
		}
		if (c.bits == 8) {
			value &= 0xff;
		} else if (c.bits == 32) {
			value &= 0xffffffffULL;
			if (value & 0x80000000ULL) value |= 0xffffffff00000000ULL;
		}
		checksum ^= value;
	}
	return checksum;
}

void validate_results(const KernelCase &c, const std::vector<dpa_tput_samecore_thread_result> &results,
		      uint64_t op_iterations)
{
	const uint64_t expected = expected_checksum(c, op_iterations);
	const uint64_t logical_ops = op_iterations * kFactor;
	const uint64_t instructions_per_op = c.bits == 8 && std::string(c.op) != "max" ? 2 : 1;
	for (size_t i = 0; i < results.size(); ++i) {
		const auto &result = results[i];
		if (result.checksum != expected || result.cycles == 0 ||
		    result.instret < logical_ops * instructions_per_op ||
		    result.cycles != result.end_cycle - result.start_cycle) {
			throw_runtime("Invalid " + std::string(c.dtype) + " " + c.op +
				      " result from worker " + std::to_string(i) +
				      ": checksum=" + std::to_string(result.checksum) +
				      " expected=" + std::to_string(expected));
		}
	}
}

RunSummary run_once(const Resources &r, const SyncEventResource &ready, const SyncEventResource &done,
		    const KernelCase &c,
		    const std::vector<uint32_t> &eus, uint64_t op_iterations)
{
	ThreadSet ts{};
	try {
		init_thread_set(r, c.fn, eus, op_iterations, done.dpa_handle, ready.dpa_handle, ts);
		const uint64_t host_ns = launch_workers_and_wait(r, ready, done, ts, static_cast<uint32_t>(eus.size()));
		validate_results(c, ts.host_results, op_iterations);
		const RunSummary summary = summarize_run(ts.host_results, host_ns);
		cleanup_thread_set(r, ts);
		return summary;
	} catch (...) {
		cleanup_thread_set(r, ts);
		throw;
	}
}

std::vector<RunSummary> measure(const Config &cfg, const Resources &r, const SyncEventResource &ready,
				const SyncEventResource &done,
				const KernelCase &c, const std::vector<uint32_t> &eus)
{
	std::vector<RunSummary> runs;
	const uint64_t total_runs = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
	runs.reserve(cfg.iters);
	for (uint64_t i = 0; i < total_runs; ++i) {
		const RunSummary run = run_once(r, ready, done, c, eus, cfg.op_iterations);
		if (i >= cfg.warmup) {
			runs.push_back(run);
		}
	}
	return runs;
}

void print_header()
{
	std::cout
		<< "mode,dtype,op,status,cores,core_ids,eus_per_core,control_eu,threads,eu_list,op_iterations,lanes,repeats,factor,total_ops,"
		<< "host_min_ns,host_p50_ns,host_mean_ns,host_p50_mops,"
		<< "maxdur_min_cycles,maxdur_p50_cycles,maxdur_mean_cycles,"
		<< "maxdur_p50_mops,"
		<< "expected_instret_per_logical_op,"
		<< "instret_per_logical_op_min,instret_per_logical_op_p50,"
		<< "instret_per_logical_op_max,checksum\n";
}

void emit_row(const KernelCase &c, const std::vector<uint32_t> &core_ids, uint32_t eus_per_core, uint32_t control_eu,
	      const std::vector<uint32_t> &eus, uint64_t op_iterations, const std::vector<RunSummary> &runs)
{
	std::vector<uint64_t> host_windows;
	std::vector<uint64_t> max_durations;
	std::vector<double> instret_per_op_values;
	const uint64_t checksum = runs.front().checksum_sum;
	const uint32_t threads = static_cast<uint32_t>(eus.size());
	const double total_ops = static_cast<double>(op_iterations) * static_cast<double>(kFactor) *
				 static_cast<double>(threads);
	const double instructions_per_op = c.bits == 8 && std::string(c.op) != "max" ? 2.0 : 1.0;
	const double expected_instret_per_logical_op = instructions_per_op + 2.0 / static_cast<double>(kFactor);

	for (const auto &run : runs) {
		host_windows.push_back(run.host_window_ns);
		max_durations.push_back(run.max_duration_cycles);
		instret_per_op_values.push_back(static_cast<double>(run.instret_sum) / total_ops);
	}
	std::sort(host_windows.begin(), host_windows.end());
	std::sort(max_durations.begin(), max_durations.end());
	std::sort(instret_per_op_values.begin(), instret_per_op_values.end());

	const double host_min = static_cast<double>(host_windows.front());
	const double host_p50 = static_cast<double>(percentile_sorted(host_windows, 0.50));
	const double host_mean = mean_of(host_windows);
	const double maxdur_min = static_cast<double>(max_durations.front());
	const double maxdur_p50 = static_cast<double>(percentile_sorted(max_durations, 0.50));
	const double maxdur_mean = mean_of(max_durations);
	const double instret_min = instret_per_op_values.front();
	const double instret_p50 = percentile_sorted(instret_per_op_values, 0.50);
	const double instret_max = instret_per_op_values.back();
	const bool instret_ok = std::all_of(instret_per_op_values.begin(), instret_per_op_values.end(),
					    [&](double value) {
						    return std::abs(value - expected_instret_per_logical_op) <= 0.08;
					    });
	const char *status = instret_ok ? "ok" : "bad_instret";

	const double maxdur_p50_ops_per_cycle = total_ops / maxdur_p50;

	std::cout << std::fixed << std::setprecision(3)
		  << "multicore_raw_tput,"
		  << c.dtype << "," << c.op << ","
		  << status << ","
		  << core_ids.size() << ","
		  << format_eu_list(core_ids) << ","
		  << eus_per_core << ","
		  << control_eu << ","
		  << threads << ","
		  << format_eu_list(eus) << ","
		  << op_iterations << ","
		  << kLanes << ","
		  << kRepeats << ","
		  << kFactor << ","
		  << total_ops << ","
		  << host_min << ","
		  << host_p50 << ","
		  << host_mean << ","
		  << total_ops * 1000.0 / host_p50 << ","
		  << maxdur_min << ","
		  << maxdur_p50 << ","
		  << maxdur_mean << ","
		  << maxdur_p50_ops_per_cycle * static_cast<double>(kClockMhz) << ","
		  << expected_instret_per_logical_op << ","
		  << instret_min << ","
		  << instret_p50 << ","
		  << instret_max << ","
		  << checksum << "\n";
	std::cout << std::unitbuf;
}

void run_benchmark(const Config &cfg)
{
	const CoreMap topology = load_topology(cfg.topology_file);
	Resources r{};
	SyncEventResource ready{};
	SyncEventResource done{};
	try {
		init_resources(cfg, r);
		const uint32_t control_eu = cfg.control_eu;
		bool control_eu_found = false;
		for (const auto &[core, mapped] : topology) {
			(void)core;
			for (const auto &[slot, eu] : mapped) {
				(void)slot;
				control_eu_found |= eu == control_eu;
			}
		}
		if (!control_eu_found) {
			throw_runtime("Control EU is not in the topology: " + std::to_string(control_eu));
		}
		std::vector<uint32_t> core_pool = cfg.core_ids;
		if (core_pool.empty()) {
			for (const auto &[core, eus] : topology) {
				(void)eus;
				core_pool.push_back(core);
			}
		}
		for (uint32_t core : core_pool) {
			const auto found = topology.find(core);
			if (found == topology.end() || core >= r.num_cores) {
				throw_runtime("Core is not available in the topology: " + std::to_string(core));
			}
			for (const auto &[slot, eu] : found->second) {
				if (slot >= r.eus_per_core || eu >= r.total_eus) {
					throw_runtime("Topology CSV does not match the DPA SDK topology");
				}
				doca_dpa_eu_affinity *affinity = nullptr;
				check_doca(doca_dpa_eu_affinity_create(r.dpa, &affinity), "doca_dpa_eu_affinity_create");
				const doca_error_t status = doca_dpa_eu_affinity_set(affinity, eu);
				(void)doca_dpa_eu_affinity_destroy(affinity);
				check_doca(status, "doca_dpa_eu_affinity_set");
			}
		}
		create_thread_sync_event(r, ready);
		create_thread_sync_event(r, done);
		size_t visible_eus = 0;
		for (uint32_t core : core_pool) {
			visible_eus += topology.at(core).size();
		}

		std::cout << "# device=" << cfg.device_name
			  << " warmup=" << cfg.warmup
			  << " iters=" << cfg.iters
			  << " op_iterations=" << cfg.op_iterations
			  << " types=" << format_selection(cfg.types)
			  << " ops=" << format_selection(cfg.ops)
			  << " clock_mhz=" << kClockMhz
			  << " lanes=" << kLanes
			  << " repeats=" << kRepeats
			  << " factor=" << kFactor
			  << " topology_file=" << cfg.topology_file
			  << " control_eu=" << control_eu
			  << " div_inputs=u8:0xff,u32:0xffffffff,u64:0x7fffffffffffffff"
			  << " div_divisor=3"
			  << " visible_cores=" << core_pool.size()
			  << " visible_eus=" << visible_eus
			  << " dpa_cores=" << r.num_cores
			  << " eus_per_core=" << r.eus_per_core
			  << " total_eus=" << r.total_eus
			  << "\n";
		if (cfg.print_header) {
			print_header();
		}

		auto run_placement = [&](const std::vector<uint32_t> &selected_cores, uint32_t per_core,
						 const std::vector<uint32_t> &eus) {
			for (const auto &c : kKernelCases) {
				if (cfg.types.count(c.dtype) == 0 || cfg.ops.count(c.op) == 0) {
					continue;
				}
				const std::vector<RunSummary> runs = measure(cfg, r, ready, done, c, eus);
				emit_row(c, selected_cores, per_core, control_eu, eus, cfg.op_iterations, runs);
			}
		};

		bool any_placement = false;
		for (uint32_t requested_cores : cfg.core_counts) {
			const uint32_t count = requested_cores == 0 ? static_cast<uint32_t>(core_pool.size()) : requested_cores;
			if (count > core_pool.size()) {
				continue;
			}
			const std::vector<uint32_t> selected_cores(core_pool.begin(), core_pool.begin() + count);
			for (uint32_t requested_eus : cfg.eus_per_core) {
				const uint32_t per_core = requested_eus == 0 ? r.eus_per_core : requested_eus;
				std::vector<uint32_t> eus;
				for (uint32_t core : selected_cores) {
					const auto &mapped = topology.at(core);
					uint32_t selected = 0;
					for (const auto &[slot, eu] : mapped) {
						(void)slot;
						if (eu != control_eu && selected < per_core) {
							eus.push_back(eu);
							++selected;
						}
					}
				}
				if (eus.empty()) {
					continue;
				}
				any_placement = true;
				run_placement(selected_cores, per_core, eus);
			}
		}
		if (!any_placement) {
			throw_runtime("No requested core count is available in the topology");
		}

		destroy_sync_event(ready);
		destroy_sync_event(done);
		cleanup_resources(r);
	} catch (...) {
		destroy_sync_event(ready);
		destroy_sync_event(done);
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
