/* ---------------------------------------------------------------------------
 * The shared counter page. See avian_metrics.h for why it exists.
 * ------------------------------------------------------------------------- */
#define _GNU_SOURCE

#include "avian_metrics.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>

#define AV_CACHE_LINE 64

static const uint64_t av_metric_bucket_us[AV_METRIC_BUCKETS] = {
    500, 1000, 2500, 5000, 10000, 25000, 50000,
    100000, 250000, 500000, 1000000, 2500000, 5000000, 10000000
};

uint64_t av_metric_bucket_edge(int i) {
    if (i < 0 || i >= AV_METRIC_BUCKETS) return 0;
    return av_metric_bucket_us[i];
}

/* Rounded up to whole cache lines so two workers never share one. */
static size_t slot_stride(void) {
    size_t bytes = (size_t)AV_METRIC_COUNT * sizeof(uint64_t);
    return (bytes + AV_CACHE_LINE - 1) / AV_CACHE_LINE * AV_CACHE_LINE;
}

static unsigned char *g_page = NULL;
static int g_slots = 0;
static size_t g_stride = 0;

int av_metrics_init(int slots) {
    if (g_page) return 0;
    if (slots < 1) slots = 1;
    g_stride = slot_stride();
    size_t bytes = g_stride * (size_t)slots;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    memset(p, 0, bytes);
    g_page = (unsigned char *)p;
    g_slots = slots;
    return 0;
}

int av_metrics_enabled(void) { return g_page != NULL; }
int av_metrics_slots(void) { return g_slots; }

static _Atomic uint64_t *cell(int slot, int index) {
    if (!g_page || slot < 0 || slot >= g_slots) return NULL;
    if (index < 0 || index >= AV_METRIC_COUNT) return NULL;
    return (_Atomic uint64_t *)(g_page + g_stride * (size_t)slot
                                + sizeof(uint64_t) * (size_t)index);
}

/* One writer per slot, so this is a load, an add and a store rather than a
 * locked read-modify-write. */
void av_metrics_add(int slot, int index, uint64_t n) {
    _Atomic uint64_t *c = cell(slot, index);
    if (!c) return;
    uint64_t v = atomic_load_explicit(c, memory_order_relaxed);
    atomic_store_explicit(c, v + n, memory_order_relaxed);
}

void av_metrics_set(int slot, int index, uint64_t v) {
    _Atomic uint64_t *c = cell(slot, index);
    if (!c) return;
    atomic_store_explicit(c, v, memory_order_relaxed);
}

/* initial-exec for the reason given at g_current_worker in avian_extra.c. */
static _Thread_local int g_local_slot __attribute__((tls_model("initial-exec"))) = 0;

void av_metrics_bind(int slot) { g_local_slot = slot; }

void av_metrics_add_local(int index, uint64_t n) {
    av_metrics_add(g_local_slot, index, n);
}

void av_metrics_set_local(int index, uint64_t v) {
    av_metrics_set(g_local_slot, index, v);
}

uint64_t av_metrics_sum(int index) {
    if (!g_page || index < 0 || index >= AV_METRIC_COUNT) return 0;
    uint64_t total = 0;
    for (int i = 0; i < g_slots; i++) {
        _Atomic uint64_t *c = cell(i, index);
        if (c) total += atomic_load_explicit(c, memory_order_relaxed);
    }
    return total;
}

int av_metrics_bucket(uint64_t micros) {
    for (int i = 0; i < AV_METRIC_BUCKETS; i++) {
        if (micros <= av_metric_bucket_us[i]) return i;
    }
    return AV_METRIC_BUCKETS;
}
