#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
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

#include "host/dpa_compute_tput_samecore_common.h"

extern "C" {
#define DECL_K(name) extern doca_dpa_func_t name;
DECL_K(k_tput_samecore_u64_add) DECL_K(k_tput_samecore_u64_mul) DECL_K(k_tput_samecore_u64_div)
DECL_K(k_tput_samecore_u64_min) DECL_K(k_tput_samecore_u64_max) DECL_K(k_tput_samecore_u64_xor) DECL_K(k_tput_samecore_u64_and)
DECL_K(k_tput_samecore_u32_add) DECL_K(k_tput_samecore_u32_mul) DECL_K(k_tput_samecore_u32_div)
DECL_K(k_tput_samecore_u32_min) DECL_K(k_tput_samecore_u32_max) DECL_K(k_tput_samecore_u32_xor) DECL_K(k_tput_samecore_u32_and)
DECL_K(k_tput_samecore_u8_add)  DECL_K(k_tput_samecore_u8_mul)  DECL_K(k_tput_samecore_u8_div)
DECL_K(k_tput_samecore_u8_min)  DECL_K(k_tput_samecore_u8_max)  DECL_K(k_tput_samecore_u8_xor)  DECL_K(k_tput_samecore_u8_and)
DECL_K(k_tput_samecore_start)
#undef DECL_K
}

extern struct doca_dpa_app *DPA_APP_NAME;
DOCA_LOG_REGISTER(DPA_TPUT_SAMECORE_BENCH);

namespace {

constexpr uint64_t kSyncEventMask = UINT64_MAX;
constexpr uint32_t kClockMhz = 1800;
constexpr uint32_t kLanes = 10;
constexpr uint32_t kRepeats = 64;
constexpr uint32_t kFactor = kLanes * kRepeats;

enum Dtype : uint32_t {
	DTYPE_U8 = 8,
	DTYPE_U32 = 32,
	DTYPE_U64 = 64,
};

enum Op : uint32_t {
	OP_ADD = 0,
	OP_MUL = 1,
	OP_DIV = 2,
	OP_MIN = 3,
	OP_MAX = 4,
	OP_XOR = 5,
	OP_AND = 6,
};

struct NameId {
	const char *name;
	uint32_t id;
};

constexpr NameId kDtypes[] = {
	{"u8", DTYPE_U8},
	{"u32", DTYPE_U32},
	{"u64", DTYPE_U64},
};

constexpr NameId kOps[] = {
	{"add", OP_ADD},
	{"mul", OP_MUL},
	{"div", OP_DIV},
	{"min", OP_MIN},
	{"max", OP_MAX},
	{"xor", OP_XOR},
	{"and", OP_AND},
};

struct KernelCase {
	uint32_t dtype;
	uint32_t op;
	doca_dpa_func_t *fn;
	double instret_per_logical_op;
};

#define K(DTYPE_ID, OP_ID, FN, INSTRET_PER_OP) {DTYPE_ID, OP_ID, &FN, INSTRET_PER_OP}
constexpr KernelCase kKernelCases[] = {
	K(DTYPE_U8,  OP_ADD, k_tput_samecore_u8_add,  2.0),
	K(DTYPE_U8,  OP_MUL, k_tput_samecore_u8_mul,  2.0),
	K(DTYPE_U8,  OP_DIV, k_tput_samecore_u8_div,  2.0),
	K(DTYPE_U8,  OP_MIN, k_tput_samecore_u8_min,  1.0),
	K(DTYPE_U8,  OP_MAX, k_tput_samecore_u8_max,  1.0),
	K(DTYPE_U8,  OP_XOR, k_tput_samecore_u8_xor,  1.0),
	K(DTYPE_U8,  OP_AND, k_tput_samecore_u8_and,  1.0),
	K(DTYPE_U32, OP_ADD, k_tput_samecore_u32_add, 1.0),
	K(DTYPE_U32, OP_MUL, k_tput_samecore_u32_mul, 1.0),
	K(DTYPE_U32, OP_DIV, k_tput_samecore_u32_div, 1.0),
	K(DTYPE_U32, OP_MIN, k_tput_samecore_u32_min, 1.0),
	K(DTYPE_U32, OP_MAX, k_tput_samecore_u32_max, 1.0),
	K(DTYPE_U32, OP_XOR, k_tput_samecore_u32_xor, 1.0),
	K(DTYPE_U32, OP_AND, k_tput_samecore_u32_and, 1.0),
	K(DTYPE_U64, OP_ADD, k_tput_samecore_u64_add, 1.0),
	K(DTYPE_U64, OP_MUL, k_tput_samecore_u64_mul, 1.0),
	K(DTYPE_U64, OP_DIV, k_tput_samecore_u64_div, 1.0),
	K(DTYPE_U64, OP_MIN, k_tput_samecore_u64_min, 1.0),
	K(DTYPE_U64, OP_MAX, k_tput_samecore_u64_max, 1.0),
	K(DTYPE_U64, OP_XOR, k_tput_samecore_u64_xor, 1.0),
	K(DTYPE_U64, OP_AND, k_tput_samecore_u64_and, 1.0),
};
#undef K

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
			throw std::runtime_error(std::string("Unknown ") + kind_name + " token: " + token);
		}
	}
	if (ids.empty()) {
		throw std::runtime_error(std::string("Empty ") + kind_name + " set");
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

const KernelCase *find_kernel_case(uint32_t dtype, uint32_t op)
{
	for (const auto &c : kKernelCases) {
		if (c.dtype == dtype && c.op == op) {
			return &c;
		}
	}
	return nullptr;
}

std::vector<uint32_t> parse_thread_counts(const std::string &csv)
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
			values.clear();
			for (uint32_t i = 1; i <= DPA_TPUT_SAMECORE_MAX_THREADS; ++i) {
				values.push_back(i);
			}
			return values;
		}
		const uint32_t value = static_cast<uint32_t>(std::stoul(token));
		if (value == 0 || value > DPA_TPUT_SAMECORE_MAX_THREADS) {
			throw std::runtime_error("--threads values must be in [1,16]");
		}
		values.push_back(value);
	}
	if (values.empty()) {
		throw std::runtime_error("Empty thread-count set");
	}
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	return values;
}

std::vector<uint32_t> parse_eu_list(const std::string &csv)
{
	std::vector<uint32_t> values;
	std::stringstream ss(csv);
	std::string token;
	while (std::getline(ss, token, ',')) {
		if (!token.empty()) {
			values.push_back(static_cast<uint32_t>(std::stoul(token)));
		}
	}
	if (values.empty()) {
		throw std::runtime_error("Empty EU list");
	}
	if (values.size() > DPA_TPUT_SAMECORE_MAX_THREADS) {
		throw std::runtime_error("--eu-list supports at most 16 EUs");
	}
	std::vector<uint32_t> sorted = values;
	std::sort(sorted.begin(), sorted.end());
	if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
		throw std::runtime_error("--eu-list must not contain duplicate EU IDs");
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
	uint32_t warmup = 10;
	uint32_t iters = 50;
	uint64_t op_iterations = 500;
	uint32_t core_id = 1;
	uint32_t base_eu = 0;
	bool base_eu_override = false;
	bool print_header = true;
	std::vector<uint32_t> explicit_eus;
	std::vector<uint32_t> thread_counts = {1, 2, 4, 8, 16};
	std::vector<uint32_t> dtypes = {DTYPE_U8, DTYPE_U32, DTYPE_U64};
	std::vector<uint32_t> ops = {OP_ADD, OP_MUL, OP_DIV, OP_MIN, OP_MAX, OP_XOR, OP_AND};
};

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
	uint64_t global_window_cycles = 0;
	uint64_t max_duration_cycles = 0;
	uint64_t min_duration_cycles = 0;
	uint64_t start_skew_cycles = 0;
	uint64_t instret_sum = 0;
	uint64_t checksum_xor = 0;
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
		} else if (arg == "--warmup") {
			cfg.warmup = static_cast<uint32_t>(std::stoul(require_value("--warmup")));
		} else if (arg == "--iters") {
			cfg.iters = static_cast<uint32_t>(std::stoul(require_value("--iters")));
		} else if (arg == "--op-iterations") {
			cfg.op_iterations = std::stoull(require_value("--op-iterations"));
		} else if (arg == "--threads") {
			cfg.thread_counts = parse_thread_counts(require_value("--threads"));
		} else if (arg == "--core-id") {
			cfg.core_id = static_cast<uint32_t>(std::stoul(require_value("--core-id")));
		} else if (arg == "--base-eu" || arg == "--eu-id") {
			cfg.base_eu = static_cast<uint32_t>(std::stoul(require_value(arg.c_str())));
			cfg.base_eu_override = true;
		} else if (arg == "--eu-list") {
			cfg.explicit_eus = parse_eu_list(require_value("--eu-list"));
		} else if (arg == "--types") {
			cfg.dtypes = parse_name_list(require_value("--types"), kDtypes, "type");
		} else if (arg == "--ops") {
			cfg.ops = parse_name_list(require_value("--ops"), kOps, "op");
		} else if (arg == "--no-header") {
			cfg.print_header = false;
		} else if (arg == "-h" || arg == "--help") {
			std::cout
				<< "dpa_compute_tput_samecore_bench options:\n"
				<< "  --device-name <ibdev>      Device name (default: mlx5_0)\n"
				<< "  --warmup <num>             Warmup runs per case (default: 10)\n"
				<< "  --iters <num>              Measured runs per case (default: 50)\n"
				<< "  --op-iterations <num>      Outer loop iters per DPA thread (default: 500)\n"
				<< "  --threads <csv|all>        Thread counts in [1,16] on one core (default: 1,2,4,8,16)\n"
				<< "  --core-id <num>            DPA core ID for generated EU placement (default: 1)\n"
				<< "  --base-eu <num>            First EU ID; thread i uses base-eu+i\n"
				<< "  --eu-list <csv>            Explicit same-core EU IDs; overrides --threads\n"
				<< "  --types <csv>              u8,u32,u64,all (default: all integer types)\n"
				<< "  --ops <csv>                add,mul,div,min,max,xor,and,all (default: all)\n"
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
		throw_runtime("--op-iterations overflows the instruction count");
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
	if (r.eus_per_core == 0) {
		throw_runtime("Device reports zero EUs per core");
	}
	const auto validate_eus = [&](const std::vector<uint32_t> &eus) {
		if (eus.empty()) {
			throw_runtime("Empty EU placement");
		}
		const uint32_t core = eus[0] / r.eus_per_core;
		for (uint32_t eu : eus) {
			if (eu >= r.total_eus) {
				throw_runtime("EU ID outside the available EU range");
			}
			if ((eu / r.eus_per_core) != core) {
				throw_runtime("EU placement crosses a DPA core boundary");
			}
		}
	};
	if (!cfg.explicit_eus.empty()) {
		validate_eus(cfg.explicit_eus);
		return;
	}
	const uint32_t base_eu = cfg.base_eu_override ? cfg.base_eu : cfg.core_id * r.eus_per_core;
	const uint32_t max_threads = *std::max_element(cfg.thread_counts.begin(), cfg.thread_counts.end());
	if (cfg.core_id >= r.num_cores && !cfg.base_eu_override) {
		throw_runtime("--core-id is outside available DPA core range");
	}
	std::vector<uint32_t> max_eus(max_threads);
	for (uint32_t i = 0; i < max_threads; ++i) {
		max_eus[i] = base_eu + i;
	}
	validate_eus(max_eus);
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

void create_thread_done_sync_event(const Resources &r, SyncEventResource &se)
{
	check_doca(doca_sync_event_create(&se.event), "doca_sync_event_create");
	check_doca(doca_sync_event_add_publisher_location_cpu(se.event, r.dev), "add_publisher_location_cpu(done)");
	check_doca(doca_sync_event_add_subscriber_location_dpa(se.event, r.dpa), "add_subscriber_location_dpa(done)");
	check_doca(doca_sync_event_start(se.event), "sync_event_start(done)");
	check_doca(doca_sync_event_get_dpa_handle(se.event, r.dpa, &se.dpa_handle),
		   "sync_event_get_dpa_handle(done)");
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
		     uint64_t op_iterations, doca_dpa_dev_sync_event_t done_handle, ThreadSet &ts)
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
		args[i].ready_se = 0;
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

void launch_starter_and_wait(const Resources &r, const SyncEventResource &done, ThreadSet &ts, uint32_t thread_count)
{
	uint64_t retval = 0;
	check_doca(doca_sync_event_update_set(done.event, 0), "doca_sync_event_update_set(done)");
	check_doca(doca_dpa_rpc(r.dpa, &k_tput_samecore_start, &retval, ts.notify_handles_dev,
				static_cast<uint64_t>(thread_count), ts.start_flag_dev),
		   "doca_dpa_rpc(start)");
	check_doca(doca_sync_event_wait_gt(done.event, thread_count - 1, kSyncEventMask),
		   "doca_sync_event_wait_gt(done)");
	check_doca(doca_dpa_d2h_memcpy(r.dpa, ts.host_results.data(), ts.results_dev,
				       sizeof(dpa_tput_samecore_thread_result) * thread_count),
		   "doca_dpa_d2h_memcpy(results)");
}

RunSummary summarize_run(const std::vector<dpa_tput_samecore_thread_result> &results)
{
	RunSummary s{};
	uint64_t min_start = std::numeric_limits<uint64_t>::max();
	uint64_t max_start = 0;
	uint64_t max_end = 0;
	s.min_duration_cycles = std::numeric_limits<uint64_t>::max();
	for (const auto &r : results) {
		min_start = std::min(min_start, r.start_cycle);
		max_start = std::max(max_start, r.start_cycle);
		max_end = std::max(max_end, r.end_cycle);
		s.min_duration_cycles = std::min(s.min_duration_cycles, r.cycles);
		s.max_duration_cycles = std::max(s.max_duration_cycles, r.cycles);
		s.instret_sum += r.instret;
		s.checksum_xor ^= r.checksum;
	}
	s.global_window_cycles = max_end - min_start;
	s.start_skew_cycles = max_start - min_start;
	return s;
}

RunSummary run_once(const Resources &r, const SyncEventResource &done, doca_dpa_func_t *worker,
		    const std::vector<uint32_t> &eus, uint64_t op_iterations)
{
	ThreadSet ts{};
	try {
		init_thread_set(r, worker, eus, op_iterations, done.dpa_handle, ts);
		launch_starter_and_wait(r, done, ts, static_cast<uint32_t>(eus.size()));
		uint64_t dividend = 0;
		if (worker == &k_tput_samecore_u8_div) dividend = 0xffULL;
		if (worker == &k_tput_samecore_u32_div) dividend = 0xffffffffULL;
		if (worker == &k_tput_samecore_u64_div) dividend = 0x7fffffffffffffffULL;
		if (dividend != 0) {
			const uint64_t expected = (dividend / 3) * kLanes + 3ULL;
			const uint64_t instructions = op_iterations * kFactor *
				(worker == &k_tput_samecore_u8_div ? 2 : 1);
			for (const auto &result : ts.host_results) {
				if (result.checksum != expected || result.cycles == 0 ||
				    result.instret < instructions) {
					throw_runtime("Fixed-input integer div result validation failed");
				}
			}
		}
		const RunSummary summary = summarize_run(ts.host_results);
		cleanup_thread_set(r, ts);
		return summary;
	} catch (...) {
		cleanup_thread_set(r, ts);
		throw;
	}
}

std::vector<RunSummary> measure(const Config &cfg, const Resources &r, const SyncEventResource &done,
				doca_dpa_func_t *worker, const std::vector<uint32_t> &eus)
{
	std::vector<RunSummary> runs;
	const uint64_t total_runs = static_cast<uint64_t>(cfg.warmup) + static_cast<uint64_t>(cfg.iters);
	runs.reserve(cfg.iters);
	for (uint64_t i = 0; i < total_runs; ++i) {
		const RunSummary run = run_once(r, done, worker, eus, cfg.op_iterations);
		if (i >= cfg.warmup) {
			runs.push_back(run);
		}
	}
	return runs;
}

void print_header()
{
	std::cout
		<< "mode,dtype,op,status,placement,threads,eu_list,op_iterations,lanes,repeats,factor,total_ops,"
		<< "global_min_cycles,global_p50_cycles,global_mean_cycles,"
		<< "maxdur_min_cycles,maxdur_p50_cycles,maxdur_mean_cycles,"
		<< "start_skew_min_cycles,start_skew_p50_cycles,start_skew_max_cycles,"
		<< "global_min_ops_per_cycle,global_min_mops,"
		<< "global_p50_ops_per_cycle,global_p50_mops,"
		<< "maxdur_p50_ops_per_cycle,maxdur_p50_mops,"
		<< "expected_instret_per_logical_op,"
		<< "instret_per_logical_op_min,instret_per_logical_op_p50,"
		<< "instret_per_logical_op_max,checksum\n";
}

void emit_row(const KernelCase &c, const std::string &placement, const std::vector<uint32_t> &eus,
	      uint64_t op_iterations, const std::vector<RunSummary> &runs)
{
	std::vector<uint64_t> global_windows;
	std::vector<uint64_t> max_durations;
	std::vector<uint64_t> start_skews;
	std::vector<double> instret_per_op_values;
	uint64_t checksum = 0;
	const uint32_t threads = static_cast<uint32_t>(eus.size());
	const double total_ops = static_cast<double>(op_iterations) * static_cast<double>(kFactor) *
				 static_cast<double>(threads);
	const double expected_instret_per_logical_op = c.instret_per_logical_op + 2.0 / static_cast<double>(kFactor);

	for (const auto &run : runs) {
		global_windows.push_back(run.global_window_cycles);
		max_durations.push_back(run.max_duration_cycles);
		start_skews.push_back(run.start_skew_cycles);
		instret_per_op_values.push_back(static_cast<double>(run.instret_sum) / total_ops);
		checksum ^= run.checksum_xor;
	}
	std::sort(global_windows.begin(), global_windows.end());
	std::sort(max_durations.begin(), max_durations.end());
	std::sort(start_skews.begin(), start_skews.end());
	std::sort(instret_per_op_values.begin(), instret_per_op_values.end());

	const double global_min = static_cast<double>(global_windows.front());
	const double global_p50 = static_cast<double>(percentile_sorted(global_windows, 0.50));
	const double global_mean = mean_of(global_windows);
	const double maxdur_min = static_cast<double>(max_durations.front());
	const double maxdur_p50 = static_cast<double>(percentile_sorted(max_durations, 0.50));
	const double maxdur_mean = mean_of(max_durations);
	const double skew_min = static_cast<double>(start_skews.front());
	const double skew_p50 = static_cast<double>(percentile_sorted(start_skews, 0.50));
	const double skew_max = static_cast<double>(start_skews.back());
	const double instret_min = instret_per_op_values.front();
	const double instret_p50 = percentile_sorted(instret_per_op_values, 0.50);
	const double instret_max = instret_per_op_values.back();
	const bool instret_ok = std::abs(instret_p50 - expected_instret_per_logical_op) <= 0.08;
	const char *status = instret_ok ? "ok" : "bad_instret";

	const double global_min_ops_per_cycle = total_ops / global_min;
	const double global_p50_ops_per_cycle = total_ops / global_p50;
	const double maxdur_p50_ops_per_cycle = total_ops / maxdur_p50;

	std::cout << std::fixed << std::setprecision(3)
		  << "samecore_raw_tput,"
		  << name_of_id(c.dtype, kDtypes, "unknown_dtype") << ","
		  << name_of_id(c.op, kOps, "unknown_op") << ","
		  << status << ","
		  << placement << ","
		  << threads << ","
		  << format_eu_list(eus) << ","
		  << op_iterations << ","
		  << kLanes << ","
		  << kRepeats << ","
		  << kFactor << ","
		  << total_ops << ","
		  << global_min << ","
		  << global_p50 << ","
		  << global_mean << ","
		  << maxdur_min << ","
		  << maxdur_p50 << ","
		  << maxdur_mean << ","
		  << skew_min << ","
		  << skew_p50 << ","
		  << skew_max << ","
		  << global_min_ops_per_cycle << ","
		  << global_min_ops_per_cycle * static_cast<double>(kClockMhz) << ","
		  << global_p50_ops_per_cycle << ","
		  << global_p50_ops_per_cycle * static_cast<double>(kClockMhz) << ","
		  << maxdur_p50_ops_per_cycle << ","
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
	Resources r{};
	SyncEventResource done{};
	try {
		init_resources(cfg, r);
		create_thread_done_sync_event(r, done);

		const uint32_t base_eu = cfg.base_eu_override ? cfg.base_eu : cfg.core_id * r.eus_per_core;
		const uint32_t core_id = base_eu / r.eus_per_core;
		std::cout << "# device=" << cfg.device_name
			  << " warmup=" << cfg.warmup
			  << " iters=" << cfg.iters
			  << " op_iterations=" << cfg.op_iterations
			  << " clock_mhz=" << kClockMhz
			  << " lanes=" << kLanes
			  << " repeats=" << kRepeats
			  << " factor=" << kFactor
			  << " core_id=" << core_id
			  << " base_eu=" << base_eu
			  << " explicit_eu_list=" << (cfg.explicit_eus.empty() ? "none" : format_eu_list(cfg.explicit_eus))
			  << " div_inputs=u8:0xff,u32:0xffffffff,u64:0x7fffffffffffffff"
			  << " div_divisor=3"
			  << " dpa_cores=" << r.num_cores
			  << " eus_per_core=" << r.eus_per_core
			  << " total_eus=" << r.total_eus
			  << "\n";
		if (cfg.print_header) {
			print_header();
		}

		auto run_placement = [&](const std::string &placement, const std::vector<uint32_t> &eus) {
			for (uint32_t dtype : cfg.dtypes) {
				for (uint32_t op : cfg.ops) {
					const KernelCase *c = find_kernel_case(dtype, op);
					if (c == nullptr) {
						continue;
					}
					const std::vector<RunSummary> runs = measure(cfg, r, done, c->fn, eus);
					emit_row(*c, placement, eus, cfg.op_iterations, runs);
				}
			}
		};

		if (!cfg.explicit_eus.empty()) {
			run_placement("explicit", cfg.explicit_eus);
		} else {
			for (uint32_t threads : cfg.thread_counts) {
				std::vector<uint32_t> eus(threads);
				for (uint32_t i = 0; i < threads; ++i) {
					eus[i] = base_eu + i;
				}
				run_placement("same_core_consecutive", eus);
			}
		}

		destroy_sync_event(done);
		cleanup_resources(r);
	} catch (...) {
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
