/* SPDX-License-Identifier: Apache-2.0 */
/* The UI thread's event loop: file descriptors and timers through poll(2), one timerfd for every
 * timer, an eventfd other threads use to wake the loop, and a signalfd for SIGINT and SIGTERM.
 * Everything here runs on the UI thread, except rgui_loop_wakeup, which any thread may call. */
#ifndef RGUI_PLATFORM_LINUX_LOOP_H
#define RGUI_PLATFORM_LINUX_LOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*rgui_loop_fd_callback)(int fd, uint32_t revents, void* user);
typedef void (*rgui_loop_timer_callback)(uint64_t id, void* user);
typedef void (*rgui_loop_callback)(void* user);

/* Creates the loop's timerfd and eventfd, ignores SIGPIPE and sets SIGCHLD to SA_NOCLDWAIT
 * (children spawned without waiting never become zombies). Idempotent. 0, or -1 with errno. */
int rgui_loop_init(void);

/* Blocks SIGINT and SIGTERM in the calling thread and reads them from a signalfd instead; a
 * signal then calls `callback` (which saves what it must and calls rgui_loop_quit), or quits the
 * loop when `callback` is NULL. Call it from the main thread before any other thread starts:
 * threads inherit the blocked mask, so the signals reach the signalfd only. 0, or -1 with errno. */
int rgui_loop_watch_signals(rgui_loop_callback callback, void* user);

/* Watches `fd` for `events` (POLLIN, POLLOUT, ...); the callback gets poll's revents. A second
 * add of the same fd replaces its events and callback. 0, or -1 with errno. */
int rgui_loop_add_fd(int fd, uint32_t events, rgui_loop_fd_callback callback, void* user);
/* Changes the events of a watched fd. 0, or -1 when the fd is not watched. */
int rgui_loop_set_fd_events(int fd, uint32_t events);
/* Stops watching `fd` (it stays open). Safe inside any callback, the fd's own included. */
void rgui_loop_remove_fd(int fd);

/* A timer firing `ms` milliseconds from now, then every `repeat_ms` milliseconds when that is not
 * 0. The id is never 0. A timer started inside a callback first fires on a later iteration. */
uint64_t rgui_loop_add_timer(uint32_t ms, uint32_t repeat_ms, rgui_loop_timer_callback callback, void* user);
/* Stops a timer; unknown or finished ids are ignored. Safe inside any callback. */
void rgui_loop_cancel_timer(uint64_t id);

/* Called on the UI thread, once per wakeup batch, after rgui_loop_wakeup (from any thread). */
void rgui_loop_set_wakeup_handler(rgui_loop_callback callback, void* user);
void rgui_loop_wakeup(void);

/* Called before every poll (flushing buffered output, starting frames). At most 8. 0 or -1. */
int rgui_loop_add_prepare(rgui_loop_callback callback, void* user);
void rgui_loop_remove_prepare(rgui_loop_callback callback, void* user);

/* Runs until rgui_loop_quit. 0, or -1 when poll fails for a reason other than EINTR. */
int rgui_loop_run(void);
void rgui_loop_quit(void);

/* Milliseconds of CLOCK_MONOTONIC. */
uint64_t rgui_loop_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif
