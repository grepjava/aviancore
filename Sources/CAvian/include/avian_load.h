/* ---------------------------------------------------------------------------
 * How busy each worker is, where every worker can see it.
 *
 * Workers are separate processes, and the kernel hands out connections without
 * knowing which of them is behind. This page is how a worker finds out that it
 * is: each one publishes its own load, and reads the others' when it decides
 * whether to accept a connection or to hand one it holds to a sibling.
 *
 * The page is mapped MAP_SHARED before the first fork, like the metrics page
 * (avian_metrics.h), and has one slot per worker slot and its replacement,
 * padded to whole cache lines. A worker writes only its own slot, so the
 * atomics are relaxed loads and stores: a reading one turn behind is still a
 * reading.
 *
 * Load is two numbers. `busy` is the share of the last stretch of time the
 * worker's loop spent working rather than waiting, in thousandths. `conns` is
 * how many connections it holds. A worker that is waiting right now also says
 * since when, so a reader can see that a worker whose last reading was busy has
 * since gone quiet -- the reading itself is only refreshed when the loop turns,
 * and a worker with nothing to do does not turn.
 * ------------------------------------------------------------------------- */
#ifndef AVIAN_LOAD_H
#define AVIAN_LOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slot's state. Only an active slot is offered connections. */
#define AV_LOAD_ABSENT   0u
#define AV_LOAD_ACTIVE   1u
#define AV_LOAD_DRAINING 2u

/* One worker's load, as a reader sees it. `busy` has already been discounted
 * for however long the worker has been waiting. */
typedef struct {
    int32_t slot;
    /* The hand-off channel the worker in this slot receives on: its worker
     * index, which a replacement shares with the worker it replaces. */
    int32_t channel;
    uint32_t busy;
    uint32_t conns;
    /* Whether the worker is watching a listener it shares with the others,
     * and so will take a connection another leaves to it. */
    uint32_t accepting;
    /* How long a request arriving now would wait, in microseconds, as the
     * worker estimates it (av_load_publish_wait). */
    uint32_t wait_us;
    /* The worker has been working on one loop turn for AV_LOAD_STALL_US or
     * more -- a handler holding its loop -- so its readings are stale and it
     * is not taking anything; busy reads 1000. */
    uint32_t stalled;
    /* How many of its connections make requests that hold its loop for a
     * long time, as the worker counts them (av_load_publish_heavy). */
    uint32_t heavy;
} av_load_view;

/* Maps the page. Call once, before any fork. 0, or -1 with errno set. A second
 * call is a no-op that succeeds. */
int av_load_init(int slots);
int av_load_enabled(void);
int av_load_slots(void);

/* The calling worker has started in `slot` and receives hand-offs on
 * `channel`. */
void av_load_join(int slot, int channel);
/* Stopping: no longer offered anything, still counted as present. */
void av_load_draining(int slot);
/* Gone. Written by the worker as it exits. */
void av_load_leave(int slot);
/* The supervisor has reaped `pid`: whatever slot it held is gone, since a
 * worker that crashed wrote nothing on its way out. */
void av_load_reap(int pid);

void av_load_publish(int slot, uint32_t busy_permille, uint32_t conns);
/* How long a request arriving at this worker now would wait before it is
 * served, in microseconds. Busyness alone does not say: a worker half busy
 * with requests that take milliseconds keeps a quick request waiting far
 * longer than one nearly flat out with requests that take microseconds. */
void av_load_publish_wait(int slot, uint32_t wait_us);
/* How many of the worker's connections are heavy: their requests hold its
 * loop long enough that anything else on the worker waits behind them. What
 * counts as heavy is the worker's to decide. Workers that each hold one can
 * leave every quick request waiting; gathering them on fewer workers frees
 * the rest. */
void av_load_publish_heavy(int slot, uint32_t heavy);
/* The worker has started (1) or stopped (0) watching a listener it shares
 * with the others. A worker that means to leave a connection to another needs
 * to know that the other will be there to take it. */
void av_load_accepting(int slot, int on);

/* The worker is about to wait (`since_us` > 0, monotonic) or has stopped
 * waiting (0). */
void av_load_waiting(int slot, uint64_t since_us);
/* The worker has stopped waiting and begun a turn of work at `now_us`. What
 * av_load_waiting(slot, 0) says, and when, so that a reader can tell a worker
 * that has been busy on one turn for too long -- which publishes nothing
 * meanwhile -- from one that is merely busy. */
void av_load_awake(int slot, uint64_t now_us);
#define AV_LOAD_STALL_US 20000u

/* Every active slot, with busy discounted as of `now_us`: fully after the
 * worker has waited `AV_LOAD_QUIET_US`, in proportion before. Returns how many
 * were written, at most `cap`. */
#define AV_LOAD_QUIET_US 20000u
int av_load_snapshot(av_load_view *out, int cap, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* AVIAN_LOAD_H */
