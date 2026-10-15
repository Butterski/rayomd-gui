/* SPDX-License-Identifier: Apache-2.0 */
#include "platform/linux/loop.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint64_t id; /* unique per add, so a stale poll result never reaches a newer watch */
    int fd;
    uint32_t events;
    rgui_loop_fd_callback callback;
    void* user;
    int removed;
} Watch;

typedef struct {
    uint64_t id;
    uint64_t deadline_ns;
    uint64_t interval_ns;
    rgui_loop_timer_callback callback;
    void* user;
    int cancelled;
} Timer;

typedef struct {
    rgui_loop_callback callback;
    void* user;
} Hook;

enum { kMaxPrepare = 8 };

static struct {
    int initialized;
    int timer_fd;
    int event_fd;
    int signal_fd;
    Watch* watches;
    size_t watch_count, watch_capacity;
    uint64_t next_watch_id;
    Timer* timers;
    size_t timer_count, timer_capacity;
    uint64_t next_timer_id;
    uint64_t armed_ns; /* the deadline the timerfd holds, 0 when disarmed */
    int dispatching_timers;
    int dispatching_fds;
    struct pollfd* pollfds;
    uint64_t* poll_ids;
    size_t poll_capacity;
    Hook prepare[kMaxPrepare];
    int prepare_count;
    Hook wakeup;
    Hook signal;
    int quit;
} L = {.timer_fd = -1, .event_fd = -1, .signal_fd = -1};

static uint64_t NowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

uint64_t rgui_loop_now_ms(void) { return NowNs() / 1000000u; }

static int Grow(void** array, size_t* capacity, size_t need, size_t element) {
    if (need <= *capacity) return 0;
    size_t next = *capacity ? *capacity * 2 : 16;
    while (next < need) next *= 2;
    void* p = realloc(*array, next * element);
    if (!p) {
        errno = ENOMEM;
        return -1;
    }
    *array = p;
    *capacity = next;
    return 0;
}

static void CompactWatches(void) {
    if (L.dispatching_fds) return;
    size_t out = 0;
    for (size_t i = 0; i < L.watch_count; ++i) {
        if (!L.watches[i].removed) L.watches[out++] = L.watches[i];
    }
    L.watch_count = out;
}

static void CompactTimers(void) {
    if (L.dispatching_timers) return;
    size_t out = 0;
    for (size_t i = 0; i < L.timer_count; ++i) {
        if (!L.timers[i].cancelled) L.timers[out++] = L.timers[i];
    }
    L.timer_count = out;
}

static void ArmTimerFd(void) {
    uint64_t earliest = 0;
    for (size_t i = 0; i < L.timer_count; ++i) {
        const Timer* t = &L.timers[i];
        if (!t->cancelled && (earliest == 0 || t->deadline_ns < earliest)) earliest = t->deadline_ns;
    }
    if (earliest == L.armed_ns) return;
    struct itimerspec spec;
    memset(&spec, 0, sizeof spec);
    if (earliest != 0) {
        /* 0 would disarm: a deadline at the epoch of the clock is never in the future anyway. */
        spec.it_value.tv_sec = (time_t)(earliest / 1000000000u);
        spec.it_value.tv_nsec = (long)(earliest % 1000000000u);
        if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) spec.it_value.tv_nsec = 1;
    }
    if (timerfd_settime(L.timer_fd, TFD_TIMER_ABSTIME, &spec, NULL) == 0) L.armed_ns = earliest;
}

static void OnTimerFd(int fd, uint32_t revents, void* user) {
    (void)revents;
    (void)user;
    uint64_t expirations;
    while (read(fd, &expirations, sizeof expirations) < 0 && errno == EINTR) {
    }
    L.armed_ns = 0; /* expired: the timerfd holds nothing now */
    const uint64_t now = NowNs();
    const size_t count = L.timer_count; /* timers added by callbacks wait for a later round */
    L.dispatching_timers++;
    for (size_t i = 0; i < count; ++i) {
        Timer* t = &L.timers[i];
        if (t->cancelled || t->deadline_ns > now) continue;
        const uint64_t id = t->id;
        rgui_loop_timer_callback callback = t->callback;
        void* callback_user = t->user;
        if (t->interval_ns) {
            t->deadline_ns += t->interval_ns;
            if (t->deadline_ns <= now) t->deadline_ns = now + t->interval_ns;
        } else {
            t->cancelled = 1;
        }
        callback(id, callback_user); /* may add timers: L.timers may move, t is stale after this */
    }
    L.dispatching_timers--;
    CompactTimers();
}

static void OnEventFd(int fd, uint32_t revents, void* user) {
    (void)revents;
    (void)user;
    uint64_t value;
    while (read(fd, &value, sizeof value) < 0 && errno == EINTR) {
    }
    if (L.wakeup.callback) L.wakeup.callback(L.wakeup.user);
}

static void OnSignalFd(int fd, uint32_t revents, void* user) {
    (void)revents;
    (void)user;
    struct signalfd_siginfo info;
    ssize_t n;
    do {
        n = read(fd, &info, sizeof info);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof info) return;
    if (L.signal.callback) {
        L.signal.callback(L.signal.user);
    } else {
        L.quit = 1;
    }
}

int rgui_loop_init(void) {
    if (L.initialized) return 0;
    struct sigaction ignore;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);
    struct sigaction child;
    memset(&child, 0, sizeof child);
    child.sa_handler = SIG_DFL;
    child.sa_flags = SA_NOCLDWAIT;
    sigemptyset(&child.sa_mask);
    sigaction(SIGCHLD, &child, NULL);

    L.timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (L.timer_fd < 0) return -1;
    L.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (L.event_fd < 0) {
        close(L.timer_fd);
        L.timer_fd = -1;
        return -1;
    }
    L.initialized = 1;
    if (rgui_loop_add_fd(L.timer_fd, POLLIN, OnTimerFd, NULL) != 0 ||
        rgui_loop_add_fd(L.event_fd, POLLIN, OnEventFd, NULL) != 0) {
        return -1;
    }
    return 0;
}

int rgui_loop_watch_signals(rgui_loop_callback callback, void* user) {
    if (rgui_loop_init() != 0) return -1;
    L.signal.callback = callback;
    L.signal.user = user;
    if (L.signal_fd >= 0) return 0;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL) != 0) return -1;
    L.signal_fd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
    if (L.signal_fd < 0) return -1;
    return rgui_loop_add_fd(L.signal_fd, POLLIN, OnSignalFd, NULL);
}

int rgui_loop_add_fd(int fd, uint32_t events, rgui_loop_fd_callback callback, void* user) {
    if (fd < 0 || !callback) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < L.watch_count; ++i) {
        if (L.watches[i].fd == fd && !L.watches[i].removed) L.watches[i].removed = 1;
    }
    CompactWatches();
    if (Grow((void**)&L.watches, &L.watch_capacity, L.watch_count + 1, sizeof(Watch)) != 0) return -1;
    Watch* w = &L.watches[L.watch_count++];
    w->id = ++L.next_watch_id;
    w->fd = fd;
    w->events = events;
    w->callback = callback;
    w->user = user;
    w->removed = 0;
    return 0;
}

int rgui_loop_set_fd_events(int fd, uint32_t events) {
    for (size_t i = 0; i < L.watch_count; ++i) {
        if (L.watches[i].fd == fd && !L.watches[i].removed) {
            L.watches[i].events = events;
            return 0;
        }
    }
    return -1;
}

void rgui_loop_remove_fd(int fd) {
    for (size_t i = 0; i < L.watch_count; ++i) {
        if (L.watches[i].fd == fd) L.watches[i].removed = 1;
    }
    CompactWatches();
}

uint64_t rgui_loop_add_timer(uint32_t ms, uint32_t repeat_ms, rgui_loop_timer_callback callback, void* user) {
    if (!callback || !L.initialized) return 0;
    if (Grow((void**)&L.timers, &L.timer_capacity, L.timer_count + 1, sizeof(Timer)) != 0) return 0;
    Timer* t = &L.timers[L.timer_count++];
    t->id = ++L.next_timer_id;
    t->deadline_ns = NowNs() + (uint64_t)ms * 1000000u;
    t->interval_ns = (uint64_t)repeat_ms * 1000000u;
    t->callback = callback;
    t->user = user;
    t->cancelled = 0;
    return t->id;
}

void rgui_loop_cancel_timer(uint64_t id) {
    if (id == 0) return;
    for (size_t i = 0; i < L.timer_count; ++i) {
        if (L.timers[i].id == id) L.timers[i].cancelled = 1;
    }
    CompactTimers();
}

void rgui_loop_set_wakeup_handler(rgui_loop_callback callback, void* user) {
    L.wakeup.callback = callback;
    L.wakeup.user = user;
}

void rgui_loop_wakeup(void) {
    const int fd = L.event_fd;
    if (fd < 0) return;
    const uint64_t one = 1;
    while (write(fd, &one, sizeof one) < 0 && errno == EINTR) {
    }
}

int rgui_loop_add_prepare(rgui_loop_callback callback, void* user) {
    if (!callback || L.prepare_count >= kMaxPrepare) return -1;
    L.prepare[L.prepare_count].callback = callback;
    L.prepare[L.prepare_count].user = user;
    L.prepare_count++;
    return 0;
}

void rgui_loop_remove_prepare(rgui_loop_callback callback, void* user) {
    int out = 0;
    for (int i = 0; i < L.prepare_count; ++i) {
        if (L.prepare[i].callback != callback || L.prepare[i].user != user) L.prepare[out++] = L.prepare[i];
    }
    L.prepare_count = out;
}

void rgui_loop_quit(void) { L.quit = 1; }

int rgui_loop_run(void) {
    if (rgui_loop_init() != 0) return -1;
    L.quit = 0;
    while (!L.quit) {
        for (int i = 0; i < L.prepare_count && !L.quit; ++i) L.prepare[i].callback(L.prepare[i].user);
        if (L.quit) break;
        ArmTimerFd();
        CompactWatches();
        const size_t count = L.watch_count;
        const size_t capacity = L.poll_capacity;
        if (Grow((void**)&L.pollfds, &L.poll_capacity, count, sizeof(struct pollfd)) != 0) return -1;
        if (!L.poll_ids || capacity != L.poll_capacity) {
            uint64_t* ids = realloc(L.poll_ids, L.poll_capacity * sizeof(uint64_t));
            if (!ids) return -1;
            L.poll_ids = ids;
        }
        for (size_t i = 0; i < count; ++i) {
            L.pollfds[i].fd = L.watches[i].fd;
            L.pollfds[i].events = (short)L.watches[i].events;
            L.pollfds[i].revents = 0;
            L.poll_ids[i] = L.watches[i].id;
        }
        const int ready = poll(L.pollfds, (nfds_t)count, -1);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        L.dispatching_fds++;
        for (size_t i = 0; i < count && !L.quit; ++i) {
            const uint32_t revents = (uint32_t)(unsigned short)L.pollfds[i].revents;
            if (!revents) continue;
            for (size_t k = 0; k < L.watch_count; ++k) {
                Watch* w = &L.watches[k];
                if (w->id != L.poll_ids[i]) continue;
                if (!w->removed) w->callback(w->fd, revents, w->user);
                break;
            }
        }
        L.dispatching_fds--;
        CompactWatches();
    }
    return 0;
}
