/* The broadcast bus across real processes: writers racing each other while a
 * reader checks every byte it is given, writers killed part-way through a
 * message, and a wake that crosses from one process to another. See
 * scripts/bus-unit-test.sh. */
#define _GNU_SOURCE

#include "avian_bus.h"

#include <errno.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

#define WRITERS 4
#define PER_WRITER 20000
#define MAX_PAYLOAD 3000

static uint8_t filler(uint32_t writer, uint32_t counter, uint32_t i) {
    return (uint8_t)((writer * 2654435761u) ^ (counter * 40503u) ^ (i * 2246822519u) ^ (i >> 3));
}

static uint32_t payload_length(uint32_t writer, uint32_t counter) {
    return 8 + (counter * 37 + writer * 11) % (MAX_PAYLOAD - 8);
}

static uint64_t publish_from(uint32_t writer, uint32_t counter) {
    uint8_t data[MAX_PAYLOAD];
    uint32_t n = payload_length(writer, counter);
    memcpy(data, &writer, 4);
    memcpy(data + 4, &counter, 4);
    for (uint32_t i = 8; i < n; i++) data[i] = filler(writer, counter, i);
    char topic[16];
    int topic_len = snprintf(topic, sizeof topic, "t%u", writer);
    return av_bus_publish((const uint8_t *)topic, (uint32_t)topic_len, (const uint8_t *)"e", 1,
                          data, n, 1);
}

/* Whether a message read back is exactly one some writer published. */
static int intact(const uint8_t *buffer, const struct av_bus_message *m, uint32_t *writer, uint32_t *counter) {
    const uint8_t *data = buffer + m->topic_len + m->event_len;
    if (m->event_len != 1 || buffer[m->topic_len] != 'e' || m->data_len < 8) return 0;
    memcpy(writer, data, 4);
    memcpy(counter, data + 4, 4);
    if (*writer >= WRITERS || m->data_len != payload_length(*writer, *counter)) return 0;
    char topic[16];
    int topic_len = snprintf(topic, sizeof topic, "t%u", *writer);
    if ((int)m->topic_len != topic_len || memcmp(buffer, topic, (size_t)topic_len) != 0) return 0;
    for (uint32_t i = 8; i < m->data_len; i++) {
        if (data[i] != filler(*writer, *counter, i)) return 0;
    }
    return 1;
}

static void racing_writers(void) {
    uint64_t first = av_bus_next();
    pid_t pids[WRITERS];
    for (uint32_t w = 0; w < WRITERS; w++) {
        pids[w] = fork();
        if (pids[w] == 0) {
            for (uint32_t c = 0; c < PER_WRITER; c++) {
                if (publish_from(w, c) == 0) _exit(2);
            }
            _exit(0);
        }
    }

    uint8_t *buffer = malloc(av_bus_max_message());
    int64_t last[WRITERS] = { -1, -1, -1, -1 };
    uint64_t cursor = first;
    unsigned read_ok = 0, gone = 0, corrupt = 0, disorder = 0, running = WRITERS;
    while (running > 0 || cursor < av_bus_next()) {
        struct av_bus_message m;
        int rc = av_bus_read(cursor, buffer, av_bus_max_message(), &m);
        if (rc == 1) {
            uint32_t writer, counter;
            if (!intact(buffer, &m, &writer, &counter) || m.sequence != cursor) {
                corrupt++;
            } else {
                if ((int64_t)counter <= last[writer]) disorder++;
                last[writer] = counter;
                read_ok++;
            }
            cursor++;
        } else if (rc == -1) {
            gone++;
            uint64_t oldest = av_bus_oldest();
            cursor = oldest > cursor + 1 ? oldest : cursor + 1;
        } else if (rc == 0) {
            int status;
            pid_t done = waitpid(-1, &status, WNOHANG);
            if (done > 0) {
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a writer failed to publish");
                running--;
            } else {
                sched_yield();
            }
        } else {
            corrupt++;
            cursor++;
        }
    }
    CHECK(av_bus_next() - first == WRITERS * PER_WRITER, "%llu published, not %d",
          (unsigned long long)(av_bus_next() - first), WRITERS * PER_WRITER);
    CHECK(corrupt == 0, "%u messages read back were not what was published", corrupt);
    CHECK(disorder == 0, "%u messages came out of a writer's order", disorder);
    CHECK(read_ok > 1000, "only %u messages were read while the writers raced", read_ok);
    CHECK(read_ok + gone <= WRITERS * PER_WRITER, "read %u and lost %u of %d", read_ok, gone,
          WRITERS * PER_WRITER);

    /* Once they are done, everything still held is whole. */
    unsigned held = 0;
    for (uint64_t s = av_bus_oldest(); s < av_bus_next(); s++) {
        struct av_bus_message m;
        uint32_t writer, counter;
        int rc = av_bus_read(s, buffer, av_bus_max_message(), &m);
        CHECK(rc == 1, "message %llu, at or after the oldest, is not readable (%d)",
              (unsigned long long)s, rc);
        if (rc == 1) {
            CHECK(intact(buffer, &m, &writer, &counter), "message %llu is not intact", (unsigned long long)s);
            held++;
        }
    }
    CHECK(held > 100, "only %u messages held after the race", held);
    printf("racing writers: %u read while they ran, %u written over first, %u held after\n",
           read_ok, gone, held);
    free(buffer);
}

static void killed_writers(void) {
    pid_t pids[3];
    for (uint32_t w = 0; w < 3; w++) {
        pids[w] = fork();
        if (pids[w] == 0) {
            for (uint32_t c = 0;; c++) publish_from(w, c);
        }
    }
    usleep(200 * 1000);
    for (int w = 0; w < 3; w++) kill(pids[w], SIGKILL);
    for (int w = 0; w < 3; w++) waitpid(pids[w], NULL, 0);

    /* One of them may have died holding the ring. A publish must not hang. */
    alarm(20);
    uint8_t *buffer = malloc(av_bus_max_message());
    unsigned bad = 0;
    for (uint32_t c = 0; c < 1000; c++) {
        uint64_t s = publish_from(3, c);
        struct av_bus_message m;
        uint32_t writer, counter;
        if (s == 0 || av_bus_read(s, buffer, av_bus_max_message(), &m) != 1
            || !intact(buffer, &m, &writer, &counter) || writer != 3 || counter != c) bad++;
    }
    alarm(0);
    CHECK(bad == 0, "%u of 1000 messages after the killed writers were not read back whole", bad);

    /* A claim held by a process that died is taken over. */
    pid_t holder = fork();
    if (holder == 0) {
        av_bus_test_abandon(getpid());
        _exit(0);
    }
    waitpid(holder, NULL, 0);
    alarm(20);
    uint64_t s = publish_from(3, 5000);
    alarm(0);
    struct av_bus_message m;
    CHECK(s != 0 && av_bus_read(s, buffer, av_bus_max_message(), &m) == 1,
          "the ring was not taken over from a writer that died holding it");
    printf("killed writers: the ring carried on\n");
    free(buffer);
}

static void wake_across_processes(void) {
    int pipefd[2];
    if (pipe(pipefd) != 0) abort();
    pid_t child = fork();
    if (child == 0) {
        close(pipefd[0]);
        int fd = av_bus_attach(1);
        uint64_t cursor = av_bus_next();
        if (av_bus_arm(cursor) != 0) _exit(3);
        char ready = 1;
        if (write(pipefd[1], &ready, 1) != 1) _exit(4);
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 5000) != 1) _exit(1);
        av_bus_clear();
        uint8_t buffer[64];
        struct av_bus_message m;
        if (av_bus_read(cursor, buffer, sizeof buffer, &m) != 1) _exit(2);
        if (m.data_len != 5 || memcmp(buffer + m.topic_len + m.event_len, "hello", 5) != 0) _exit(2);
        _exit(0);
    }
    close(pipefd[1]);
    char ready;
    if (read(pipefd[0], &ready, 1) != 1) abort();
    av_bus_publish((const uint8_t *)"w", 1, NULL, 0, (const uint8_t *)"hello", 5, 0);
    int status;
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the other process was not woken for the message (%d)", WEXITSTATUS(status));

    /* Not armed, not woken. */
    child = fork();
    if (child == 0) {
        int fd = av_bus_attach(1);
        struct pollfd p = { fd, POLLIN, 0 };
        _exit(poll(&p, 1, 300) == 0 ? 0 : 1);
    }
    usleep(50 * 1000);
    av_bus_publish((const uint8_t *)"w", 1, NULL, 0, (const uint8_t *)"quiet", 5, 1);
    waitpid(child, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a process that did not arm was woken");
    printf("wakes: an armed process is woken, one not armed is not\n");
}

int main(void) {
    if (av_bus_init(1 << 20, 4) != 0 || av_bus_attach(0) < 0) {
        fprintf(stderr, "cannot map the ring\n");
        return 1;
    }
    racing_writers();
    killed_writers();
    wake_across_processes();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
