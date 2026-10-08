#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int number(const char *s, uint64_t *out)
{
    if (!*s || *s == '-' || *s == '+') return -1;
    char *end;
    errno = 0;
    *out = strtoull(s, &end, 10);
    return errno || *end ? -1 : 0;
}

static void usage(const char *name)
{
    printf("Usage: %s --topology-file topology.csv --eu-list A,B --layout same-line|separate-line [options]\n", name);
    puts("  --device-name mlx5_0  --period-cycles 1024  --reads 1000000");
    puts("  --warmup 5  --samples 30  --timeout-ms 1000");
    puts("stdout: one CSV row per batch, warmups are labelled");
}

static int parse(int argc, char **argv, struct config *c)
{
    const char *eu_list = "0,1", *layout = NULL;
    *c = (struct config){.device = "mlx5_0", .reads = 1000000, .period = 1024,
        .warmup = 5, .samples = 30, .timeout_ms = 1000};
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) { usage(argv[0]); return 1; }
        if (i + 1 == argc) return -1;
        const char *v = argv[++i];
        uint64_t n = 0;
        if (!strcmp(key, "--device-name")) c->device = v;
        else if (!strcmp(key, "--topology-file")) c->topology = v;
        else if (!strcmp(key, "--eu-list")) eu_list = v;
        else if (!strcmp(key, "--layout")) layout = v;
        else if (!strcmp(key, "--timeout-ms")) {
            char *end;
            double ms = strtod(v, &end);
            if (*end || !isfinite(ms) || ms <= 0 || ms > 5000) return -1;
            c->timeout_ms = ms;
        } else {
            if (number(v, &n)) return -1;
            if (!strcmp(key, "--period-cycles")) c->period = n;
            else if (!strcmp(key, "--reads")) c->reads = n;
            else if (!strcmp(key, "--warmup") && n <= 100000) c->warmup = (uint32_t)n;
            else if (!strcmp(key, "--samples") && n <= 100000) c->samples = (uint32_t)n;
            else return -1;
        }
    }
    if (!c->topology || !layout || !c->samples || !c->reads || c->reads > (UINT64_C(1) << 40) ||
        c->period < 64 || c->period > 1000000000 || c->warmup > UINT32_MAX - c->samples) return -1;
    if (!strcmp(layout, "same-line")) c->y_offset = 8;
    else if (!strcmp(layout, "separate-line")) c->y_offset = SLOT_BYTES;
    else return -1;
    const char *comma = strchr(eu_list, ',');
    if (!comma || comma == eu_list || !comma[1] || strchr(comma + 1, ',')) return -1;
    char first[16];
    size_t len = (size_t)(comma - eu_list);
    if (len >= sizeof(first)) return -1;
    memcpy(first, eu_list, len); first[len] = '\0';
    uint64_t a, b;
    if (number(first, &a) || number(comma + 1, &b) || a >= 256 || b >= 256 || a == b) return -1;
    c->eus[0] = (uint32_t)a; c->eus[1] = (uint32_t)b;
    FILE *f = fopen(c->topology, "r");
    if (!f) { perror(c->topology); return -1; }
    char line[256];
    bool mapped[256] = {0}, found[2] = {0};
    if (!fgets(line, sizeof(line), f) || strcmp(line, "eu_id,core_id,slot\n")) { fclose(f); return -1; }
    while (fgets(line, sizeof(line), f)) {
        unsigned eu, core, slot; char extra;
        if (sscanf(line, "%u,%u,%u %c", &eu, &core, &slot, &extra) != 3 || eu >= 256 || mapped[eu]) {
            fclose(f); return -1;
        }
        mapped[eu] = true;
        for (unsigned i = 0; i < 2; ++i)
            if (c->eus[i] == eu) { c->cores[i] = core; c->slots[i] = slot; found[i] = true; }
    }
    fclose(f);
    if (!found[0] || !found[1]) return -1;
    return 0;
}

int main(int argc, char **argv)
{
    struct config c;
    int parsed = parse(argc, argv, &c);
    if (parsed == 1) return 0;
    if (parsed) { fprintf(stderr, "Invalid arguments; use --help\n"); return 2; }
    struct runtime rt = {0};
    struct result r[2];
    int rc = 1;
    if (runtime_open(&c, &rt)) { rt.failed = true; goto cleanup; }
    bool same_line = (rt.data.ptr + c.y_offset) / LINE_BYTES == rt.data.ptr / LINE_BYTES;
    if (rt.data.ptr % LINE_BYTES || same_line != (c.y_offset == 8)) {
        fprintf(stderr, "Incorrect X/Y cache-line placement\n");
        rc = 2; goto cleanup;
    }
    const char *layout = c.y_offset == 8 ? "same-line" : "separate-line";
    const char *placement = c.cores[0] == c.cores[1] ? "same-core" : "cross-core";
    puts("sample,phase,status,layout,placement,eu_a,eu_b,core_a,core_b,x_address,y_address,period_cycles,reads,cycles,cycles_per_read,checksum,writes,missed,first_write,last_write,final_y,writes_per_1000_cycles");
    for (uint32_t sample = 0; sample < c.warmup + c.samples; ++sample) {
        if (runtime_sample(&c, &rt, r)) { rt.failed = true; goto cleanup; }
        bool good = r[0].status == STATUS_OK && r[1].status == STATUS_OK &&
            r[0].reads == c.reads && r[0].checksum == c.reads * X_VALUE &&
            r[1].final_y == r[1].writes && r[1].writes > 0 &&
            r[1].last_write >= r[1].first_write && r[0].cycles > 0;
        const char *status = good ? "ok" :
            (r[0].status == STATUS_TIMEOUT || r[1].status == STATUS_TIMEOUT) ? "timeout" : "bad_result";
        printf("%u,%s,%s,%s,%s,%u,%u,%u,%u,0x%" PRIx64 ",0x%" PRIx64
               ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.9f,%" PRIu64
               ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
               sample, sample < c.warmup ? "warmup" : "measure", status, layout, placement,
               c.eus[0], c.eus[1], c.cores[0], c.cores[1], (uint64_t)rt.data.ptr,
               (uint64_t)rt.data.ptr + c.y_offset, c.period, c.reads,
               r[0].cycles, (double)r[0].cycles / c.reads, r[0].checksum,
               r[1].writes, r[1].missed, r[1].first_write, r[1].last_write,
               r[1].final_y, 1000.0 * r[1].writes / r[0].cycles);
        fflush(stdout);
        if (!good) { fprintf(stderr, "Sample %u failed: %s\n", sample, status); rc = 3; goto cleanup; }
    }
    rc = 0;
cleanup:
    runtime_close(&rt);
    return rc;
}
