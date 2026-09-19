/* ---------------------------------------------------------------------------
 * The shared load page. See avian_load.h for why it exists.
 * ------------------------------------------------------------------------- */
#define _GNU_SOURCE

#include "avian_load.h"

#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define AV_CACHE_LINE 64

typedef struct {
    _Atomic uint32_t state;
    _Atomic int32_t channel;
    _Atomic uint32_t busy;
    _Atomic uint32_t conns;
    _Atomic uint64_t waiting_since;
    _Atomic int32_t pid;
    _Atomic uint32_t accepting;
    _Atomic uint32_t wait_us;
    unsigned char pad[AV_CACHE_LINE - 36];
} load_slot;

_Static_assert(sizeof(load_slot) == AV_CACHE_LINE, "a slot is one cache line");

static load_slot *g_slots = NULL;
static int g_count = 0;

int av_load_init(int slots) {
    if (g_slots) return 0;
    if (slots < 1) slots = 1;
    size_t bytes = sizeof(load_slot) * (size_t)slots;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    memset(p, 0, bytes);
    g_slots = (load_slot *)p;
    g_count = slots;
    return 0;
}

int av_load_enabled(void) { return g_slots != NULL; }
int av_load_slots(void) { return g_count; }

static load_slot *at(int slot) {
    if (!g_slots || slot < 0 || slot >= g_count) return NULL;
    return &g_slots[slot];
}

void av_load_join(int slot, int channel) {
    load_slot *s = at(slot);
    if (!s) return;
    atomic_store_explicit(&s->busy, 0, memory_order_relaxed);
    atomic_store_explicit(&s->conns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->waiting_since, 0, memory_order_relaxed);
    atomic_store_explicit(&s->channel, channel, memory_order_relaxed);
    atomic_store_explicit(&s->pid, (int32_t)getpid(), memory_order_relaxed);
    atomic_store_explicit(&s->accepting, 0, memory_order_relaxed);
    atomic_store_explicit(&s->wait_us, 0, memory_order_relaxed);
    /* Last, so a reader that sees the slot active sees the rest of it. */
    atomic_store_explicit(&s->state, AV_LOAD_ACTIVE, memory_order_release);
}

void av_load_draining(int slot) {
    load_slot *s = at(slot);
    if (s) atomic_store_explicit(&s->state, AV_LOAD_DRAINING, memory_order_relaxed);
}

void av_load_leave(int slot) {
    load_slot *s = at(slot);
    if (s) atomic_store_explicit(&s->state, AV_LOAD_ABSENT, memory_order_relaxed);
}

void av_load_reap(int pid) {
    if (!g_slots || pid <= 0) return;
    for (int i = 0; i < g_count; i++) {
        load_slot *s = &g_slots[i];
        if (atomic_load_explicit(&s->pid, memory_order_relaxed) != pid) continue;
        atomic_store_explicit(&s->state, AV_LOAD_ABSENT, memory_order_relaxed);
        atomic_store_explicit(&s->pid, 0, memory_order_relaxed);
    }
}

void av_load_publish(int slot, uint32_t busy_permille, uint32_t conns) {
    load_slot *s = at(slot);
    if (!s) return;
    if (busy_permille > 1000) busy_permille = 1000;
    atomic_store_explicit(&s->busy, busy_permille, memory_order_relaxed);
    atomic_store_explicit(&s->conns, conns, memory_order_relaxed);
}

void av_load_publish_wait(int slot, uint32_t wait_us) {
    load_slot *s = at(slot);
    if (s) atomic_store_explicit(&s->wait_us, wait_us, memory_order_relaxed);
}

void av_load_accepting(int slot, int on) {
    load_slot *s = at(slot);
    if (s) atomic_store_explicit(&s->accepting, on ? 1u : 0u, memory_order_relaxed);
}

void av_load_waiting(int slot, uint64_t since_us) {
    load_slot *s = at(slot);
    if (s) atomic_store_explicit(&s->waiting_since, since_us, memory_order_relaxed);
}

int av_load_snapshot(av_load_view *out, int cap, uint64_t now_us) {
    if (!g_slots || !out || cap <= 0) return 0;
    int n = 0;
    for (int i = 0; i < g_count && n < cap; i++) {
        load_slot *s = &g_slots[i];
        if (atomic_load_explicit(&s->state, memory_order_acquire) != AV_LOAD_ACTIVE) continue;
        uint32_t busy = atomic_load_explicit(&s->busy, memory_order_relaxed);
        uint64_t since = atomic_load_explicit(&s->waiting_since, memory_order_relaxed);
        if (since > 0 && now_us > since) {
            uint64_t quiet = now_us - since;
            if (quiet >= AV_LOAD_QUIET_US) busy = 0;
            else busy = (uint32_t)((uint64_t)busy * (AV_LOAD_QUIET_US - quiet) / AV_LOAD_QUIET_US);
        }
        out[n].slot = i;
        out[n].channel = atomic_load_explicit(&s->channel, memory_order_relaxed);
        out[n].busy = busy;
        out[n].conns = atomic_load_explicit(&s->conns, memory_order_relaxed);
        out[n].accepting = atomic_load_explicit(&s->accepting, memory_order_relaxed);
        out[n].wait_us = atomic_load_explicit(&s->wait_us, memory_order_relaxed);
        n++;
    }
    return n;
}
