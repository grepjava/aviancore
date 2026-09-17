/* ---------------------------------------------------------------------------
 * The broadcast bus: messages one worker publishes and every worker hears.
 *
 * Workers are processes, and a message published by the one a request landed
 * on has to reach subscribers held by all of them: the server-sent event
 * streams and WebSockets of a chat room are spread across workers by the
 * kernel, not by room. So messages go into a ring in memory mapped MAP_SHARED
 * before the fork, and each worker reads the ring from where it last stopped.
 *
 * The ring keeps what it has room for, not only what nobody has read yet. A
 * message stays readable, by its sequence number, until newer ones have
 * overwritten it, which is what lets a client that reconnects with the last
 * number it saw -- an event stream's Last-Event-ID -- be sent what it missed,
 * whichever worker it reconnects to.
 *
 * Sequence numbers rise by one per message and start, when the ring is
 * mapped, at the time in microseconds since the epoch. A ring mapped by a
 * restarted server so starts above every number the one before it gave out,
 * and a number from before the restart is simply older than anything kept.
 *
 * As with the other shared tables, no lock is held across processes. One
 * writer at a time claims the ring by a compare-and-swap that puts its process
 * ID in the claim word, holds it for the copy of one message, and gives it
 * back. A claim whose process no longer exists is taken over, and whatever
 * that writer left half-written is written over; readers never see it,
 * because a message is readable only once the writer has counted it.
 *
 * Readers take no claim. A reader copies a message out and then checks that
 * the writer has not since reached around the ring to the bytes it copied;
 * if it has, the message is gone, and the copy is thrown away.
 *
 * Each worker has a wake descriptor: an eventfd, or a pipe where there is
 * none. A worker with subscribers arms its slot before it sleeps, and a
 * publisher writes to the descriptor of every armed slot, disarming it as it
 * does, so a burst of messages costs one wake per worker rather than one per
 * message. A worker checks the ring once more after arming, so a message
 * published between its last read and the arming is not slept through.
 *
 * Slots are numbered as metrics slots are: twice as many as workers, so a
 * worker and the replacement overlapping it each have their own.
 * ------------------------------------------------------------------------- */
#ifndef AVIAN_BUS_H
#define AVIAN_BUS_H

#include <stddef.h>
#include <stdint.h>

/* Maps a ring of `ring_bytes`, rounded up to a power of two, and `slots` wake
 * slots. Call once, before any fork. Returns 0, or -1. */
int av_bus_init(uint64_t ring_bytes, uint32_t slots);

int av_bus_enabled(void);

/* The largest message the ring takes, topic, event name and data together. */
uint32_t av_bus_max_message(void);

/* Makes this process the holder of `slot`, and returns the descriptor that
 * becomes readable when the slot is woken. -1 when the slot does not exist. */
int av_bus_attach(uint32_t slot);

/* Publishes a message and returns its sequence number, or 0 when there is no
 * ring or the message is larger than av_bus_max_message. Every armed slot is
 * woken, except this process's own when `wake_self` is 0: a publisher on the
 * thread that reads the ring can read it next without a wake. */
uint64_t av_bus_publish(const uint8_t *topic, uint32_t topic_len,
                        const uint8_t *event, uint32_t event_len,
                        const uint8_t *data, uint32_t data_len,
                        int wake_self);

/* The number the next message will be given. Every message below it has been
 * published. */
uint64_t av_bus_next(void);

/* The lowest number that may still be readable. A message below it is gone;
 * one at or above it may be gone by the time it is read. */
uint64_t av_bus_oldest(void);

/* A message copied out of the ring. `buffer` holds the topic, then the event
 * name, then the data; the lengths say where each ends. */
struct av_bus_message {
    uint64_t sequence;
    uint32_t topic_len;
    uint32_t event_len;
    uint32_t data_len;
};

/* Copies message `sequence` into `buffer`, which holds `capacity` bytes.
 * Returns 1 with `out` filled in; 0 when it has not been published yet; -1
 * when it is gone; -2 when `capacity` is too small, with `out->data_len` set
 * to the room needed, topic and event name included. A buffer of
 * av_bus_max_message bytes is always large enough. */
int av_bus_read(uint64_t sequence, uint8_t *buffer, uint32_t capacity,
                struct av_bus_message *out);

/* Asks for a wake at the next publish. Returns 1 when a message at or above
 * `cursor` has already been published, which the caller should read now
 * rather than sleep on. */
int av_bus_arm(uint64_t cursor);

/* Empties the wake descriptor after it became readable. */
void av_bus_clear(void);

/* For tests: leaves the ring as a writer in process `pid` would that died
 * part-way through a message -- claimed, its bytes half written. */
void av_bus_test_abandon(int32_t pid);

#endif
