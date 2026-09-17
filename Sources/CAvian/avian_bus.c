/* The broadcast bus. See avian_bus.h. */
#define _GNU_SOURCE

#include "avian_bus.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/eventfd.h>
#endif

/* A message's description, in front of its bytes in the ring. */
struct record {
    uint64_t sequence;
    uint32_t topic_len;
    uint32_t event_len;
    uint32_t data_len;
    uint32_t unused;
};

struct ring {
    /* 0 when free; otherwise the claiming process's ID shifted left, and 1. */
    _Atomic uint64_t claim;
    /* The number the next message will be given. A message is readable once
     * this has passed it. */
    _Atomic uint64_t next;
    /* The highest byte position a writer has claimed: every byte below it may
     * have been written, and a byte at position p is gone once this passes
     * p + capacity. Never falls, not even when a writer that died is taken
     * over and its bytes written again. */
    _Atomic uint64_t high;
    /* The lowest number that may still be readable. */
    _Atomic uint64_t oldest;
    /* Where the next message goes. Only the claim holder reads or moves it. */
    uint64_t tail;
    uint64_t first;
    uint64_t capacity;
    uint64_t index_mask;
    uint32_t slot_count;
    uint32_t max_message;
};

/* Where a message starts, found by its number. */
struct index_entry {
    _Atomic uint64_t sequence;
    _Atomic uint64_t position;
};

struct wake_slot {
    _Atomic uint32_t armed;
    uint32_t unused;
};

static struct ring *g_ring = NULL;
static uint8_t *g_bytes = NULL;
static struct index_entry *g_index = NULL;
static struct wake_slot *g_slots = NULL;
/* Per slot: the descriptor a wake is written to and the one it is read from.
 * The same eventfd for both on Linux. Inherited across the fork. */
static int *g_write_fds = NULL;
static int *g_read_fds = NULL;
static int64_t g_self = -1;

#define RECORD_SIZE sizeof(struct record)
#define ALIGN(n) (((n) + 7) & ~(uint64_t)7)
/* Room in the index for a message of this many bytes on average. */
#define AVERAGE_RECORD 128

#ifndef __linux__
static void set_nonblocking_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int av_bus_init(uint64_t ring_bytes, uint32_t slots) {
    if (g_ring != NULL || slots == 0) return -1;
    uint64_t capacity = 1u << 16;
    while (capacity < ring_bytes && capacity < (UINT64_C(1) << 40)) capacity <<= 1;
    uint64_t entries = 1024;
    while (entries < capacity / AVERAGE_RECORD) entries <<= 1;

    size_t head = ALIGN(sizeof(struct ring));
    size_t index = entries * sizeof(struct index_entry);
    size_t wake = slots * sizeof(struct wake_slot);
    size_t total = head + index + wake + capacity;
    void *map = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map == MAP_FAILED) return -1;

    int *writes = calloc(slots, sizeof(int));
    int *reads = calloc(slots, sizeof(int));
    if (writes == NULL || reads == NULL) {
        free(writes);
        free(reads);
        munmap(map, total);
        return -1;
    }
    for (uint32_t i = 0; i < slots; i++) {
#ifdef __linux__
        int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd < 0) goto fail;
        writes[i] = reads[i] = fd;
#else
        int pair[2];
        if (pipe(pair) != 0) goto fail;
        set_nonblocking_cloexec(pair[0]);
        set_nonblocking_cloexec(pair[1]);
        reads[i] = pair[0];
        writes[i] = pair[1];
#endif
        continue;
    fail:
        for (uint32_t j = 0; j < i; j++) {
            close(reads[j]);
            if (writes[j] != reads[j]) close(writes[j]);
        }
        free(writes);
        free(reads);
        munmap(map, total);
        return -1;
    }

    struct ring *r = map;
    r->first = now_us();
    atomic_store(&r->next, r->first);
    atomic_store(&r->oldest, r->first);
    r->capacity = capacity;
    r->index_mask = entries - 1;
    r->slot_count = slots;
    /* A message may take up to a quarter of the ring, so a burst of the
     * largest ones still leaves the last few readable. */
    uint64_t max = capacity / 4 - RECORD_SIZE;
    r->max_message = max > (UINT64_C(1) << 30) ? (UINT32_C(1) << 30) : (uint32_t)max;

    g_ring = r;
    g_index = (struct index_entry *)((uint8_t *)map + head);
    g_slots = (struct wake_slot *)((uint8_t *)map + head + index);
    g_bytes = (uint8_t *)map + head + index + wake;
    g_write_fds = writes;
    g_read_fds = reads;
    return 0;
}

int av_bus_enabled(void) {
    return g_ring != NULL;
}

uint32_t av_bus_max_message(void) {
    return g_ring ? g_ring->max_message : 0;
}

int av_bus_attach(uint32_t slot) {
    if (g_ring == NULL || slot >= g_ring->slot_count) return -1;
    g_self = slot;
    atomic_store(&g_slots[slot].armed, 0);
    av_bus_clear();
    return g_read_fds[slot];
}

static void copy_in(uint64_t position, const uint8_t *from, size_t n) {
    uint64_t mask = g_ring->capacity - 1;
    while (n > 0) {
        uint64_t at = position & mask;
        size_t run = g_ring->capacity - at;
        if (run > n) run = n;
        memcpy(g_bytes + at, from, run);
        from += run;
        position += run;
        n -= run;
    }
}

static void copy_out(uint64_t position, uint8_t *to, size_t n) {
    uint64_t mask = g_ring->capacity - 1;
    while (n > 0) {
        uint64_t at = position & mask;
        size_t run = g_ring->capacity - at;
        if (run > n) run = n;
        memcpy(to, g_bytes + at, run);
        to += run;
        position += run;
        n -= run;
    }
}

static int process_gone(uint64_t claim) {
    pid_t pid = (pid_t)(claim >> 1);
    return kill(pid, 0) != 0 && errno == ESRCH;
}

static void take_claim(void) {
    uint64_t mine = ((uint64_t)getpid() << 1) | 1;
    unsigned spins = 0;
    for (;;) {
        uint64_t seen = atomic_load(&g_ring->claim);
        if (seen == 0) {
            if (atomic_compare_exchange_weak(&g_ring->claim, &seen, mine)) return;
            continue;
        }
        /* Another writer, copying one message. Held for microseconds, unless
         * its process died holding it. Only a claim still held after a while
         * is worth the system call that asks. */
        if (++spins % 256 == 0 && seen != mine && process_gone(seen)) {
            if (atomic_compare_exchange_strong(&g_ring->claim, &seen, mine)) return;
        }
        if (spins % 16 == 0) sched_yield();
    }
}

static void give_claim(void) {
    atomic_store(&g_ring->claim, 0);
}

/* Whether the bytes starting at `position` have been written over. */
static int overwritten(uint64_t position) {
    return atomic_load(&g_ring->high) > position + g_ring->capacity;
}

uint64_t av_bus_publish(const uint8_t *topic, uint32_t topic_len,
                        const uint8_t *event, uint32_t event_len,
                        const uint8_t *data, uint32_t data_len,
                        int wake_self) {
    if (g_ring == NULL) return 0;
    uint64_t body = (uint64_t)topic_len + event_len + data_len;
    if (body > g_ring->max_message) return 0;
    uint64_t size = ALIGN(RECORD_SIZE + body);

    take_claim();
    uint64_t sequence = atomic_load(&g_ring->next);
    uint64_t position = g_ring->tail;
    if (atomic_load(&g_ring->high) < position + size) atomic_store(&g_ring->high, position + size);

    /* Messages this one pushes out of reach, by their bytes or by their
     * place in the index. */
    uint64_t oldest = atomic_load(&g_ring->oldest);
    while (oldest < sequence) {
        struct index_entry *e = &g_index[oldest & g_ring->index_mask];
        if (atomic_load(&e->sequence) == oldest
            && oldest + g_ring->index_mask >= sequence
            && !overwritten(atomic_load(&e->position))) break;
        oldest++;
    }
    atomic_store(&g_ring->oldest, oldest);

    struct record rec = { sequence, topic_len, event_len, data_len, 0 };
    copy_in(position, (const uint8_t *)&rec, RECORD_SIZE);
    uint64_t at = position + RECORD_SIZE;
    if (topic_len) copy_in(at, topic, topic_len);
    at += topic_len;
    if (event_len) copy_in(at, event, event_len);
    at += event_len;
    if (data_len) copy_in(at, data, data_len);

    /* The bytes first, then the index -- position before number -- so a
     * reader that finds the number finds a message behind it. */
    struct index_entry *e = &g_index[sequence & g_ring->index_mask];
    atomic_store(&e->position, position);
    atomic_store(&e->sequence, sequence);
    g_ring->tail = position + size;
    atomic_store(&g_ring->next, sequence + 1);
    give_claim();

    for (uint32_t i = 0; i < g_ring->slot_count; i++) {
        if ((int64_t)i == g_self && !wake_self) continue;
        if (atomic_load_explicit(&g_slots[i].armed, memory_order_relaxed) == 0) continue;
        if (atomic_exchange(&g_slots[i].armed, 0) == 0) continue;
#ifdef __linux__
        uint64_t one = 1;
        ssize_t n = write(g_write_fds[i], &one, sizeof one);
#else
        uint8_t one = 1;
        ssize_t n = write(g_write_fds[i], &one, 1);
#endif
        (void)n;
    }
    return sequence;
}

uint64_t av_bus_next(void) {
    return g_ring ? atomic_load(&g_ring->next) : 0;
}

uint64_t av_bus_oldest(void) {
    return g_ring ? atomic_load(&g_ring->oldest) : 0;
}

int av_bus_read(uint64_t sequence, uint8_t *buffer, uint32_t capacity,
                struct av_bus_message *out) {
    if (g_ring == NULL) return -1;
    if (sequence >= atomic_load(&g_ring->next)) return 0;
    if (sequence < atomic_load(&g_ring->oldest)) return -1;

    struct index_entry *e = &g_index[sequence & g_ring->index_mask];
    uint64_t before = atomic_load(&e->sequence);
    uint64_t position = atomic_load(&e->position);
    if (before != sequence || atomic_load(&e->sequence) != sequence) return -1;

    struct record rec;
    copy_out(position, (uint8_t *)&rec, RECORD_SIZE);
    if (overwritten(position) || rec.sequence != sequence) return -1;

    uint64_t body = (uint64_t)rec.topic_len + rec.event_len + rec.data_len;
    if (body > g_ring->max_message) return -1;
    out->sequence = sequence;
    out->topic_len = rec.topic_len;
    out->event_len = rec.event_len;
    if (body > capacity) {
        out->data_len = (uint32_t)body;
        return -2;
    }
    out->data_len = rec.data_len;
    copy_out(position + RECORD_SIZE, buffer, body);
    /* Written over while it was copied: what was copied is not the message. */
    if (overwritten(position)) return -1;
    return 1;
}

int av_bus_arm(uint64_t cursor) {
    if (g_ring == NULL || g_self < 0) return 0;
    atomic_store(&g_slots[g_self].armed, 1);
    return atomic_load(&g_ring->next) > cursor;
}

void av_bus_clear(void) {
    if (g_ring == NULL || g_self < 0) return;
    int fd = g_read_fds[g_self];
    uint8_t scratch[64];
    while (read(fd, scratch, sizeof scratch) > 0) {}
}

void av_bus_test_abandon(int32_t pid) {
    if (g_ring == NULL) return;
    take_claim();
    uint64_t sequence = atomic_load(&g_ring->next);
    uint64_t size = ALIGN(RECORD_SIZE + 512);
    uint64_t position = g_ring->tail;
    if (atomic_load(&g_ring->high) < position + size) atomic_store(&g_ring->high, position + size);
    struct record rec = { sequence, 0, 0, 512, 0 };
    copy_in(position, (const uint8_t *)&rec, RECORD_SIZE);
    struct index_entry *e = &g_index[sequence & g_ring->index_mask];
    atomic_store(&e->position, position);
    atomic_store(&e->sequence, sequence);
    atomic_store(&g_ring->claim, ((uint64_t)(uint32_t)pid << 1) | 1);
}
