// SPDX-License-Identifier: Apache-2.0
// The event loop: one-shot and repeating timers in time order, cancelling (also from inside a
// callback), fds removed from their own callback, and tasks posted from another thread running
// on the loop's thread.
#include "platform/linux/loop.h"
#include "platform/linux/ui_tasks.h"
#include "runner.h"

#include <fcntl.h>
#include <poll.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct LoopState {
    std::vector<char> order;
    int repeats = 0;
    uint64_t repeatId = 0;
    int pipeReads = 0;
    bool taskOnLoopThread = false;
    std::thread::id loopThread;
};

LoopState* g_state = nullptr;

void OnShort(uint64_t, void*) { g_state->order.push_back('s'); }
void OnCancelled(uint64_t, void*) { g_state->order.push_back('x'); }
void OnRepeat(uint64_t id, void*) {
    g_state->order.push_back('r');
    if (++g_state->repeats == 3) rgui_loop_cancel_timer(id);
}
void OnEnd(uint64_t, void*) {
    g_state->order.push_back('e');
    rgui_loop_quit();
}
void OnPipe(int fd, uint32_t, void*) {
    char c;
    while (read(fd, &c, 1) == 1) {
    }
    g_state->pipeReads++;
    rgui_loop_remove_fd(fd);  // from its own callback
}

} // namespace

RGUI_TEST("linux.loop.timers_fds_and_tasks") {
    CHECK_EQ(rgui_loop_init(), 0);
    LoopState state;
    g_state = &state;
    state.loopThread = std::this_thread::get_id();
    rgui::wl::UiTaskQueue tasks;

    int fds[2];
    CHECK_EQ(pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
    CHECK_EQ(rgui_loop_add_fd(fds[0], POLLIN, OnPipe, nullptr), 0);

    const uint64_t start = rgui_loop_now_ms();
    CHECK(rgui_loop_add_timer(40, 0, OnShort, nullptr) != 0);
    const uint64_t cancelled = rgui_loop_add_timer(20, 0, OnCancelled, nullptr);
    state.repeatId = rgui_loop_add_timer(60, 15, OnRepeat, nullptr);
    CHECK(rgui_loop_add_timer(250, 0, OnEnd, nullptr) != 0);
    rgui_loop_cancel_timer(cancelled);

    std::thread worker([&] {
        tasks.Post([&] { state.taskOnLoopThread = std::this_thread::get_id() == state.loopThread; });
        const char c = 'p';
        CHECK_EQ(write(fds[1], &c, 1), 1);
        CHECK_EQ(write(fds[1], &c, 1), 1);
    });
    CHECK_EQ(rgui_loop_run(), 0);
    worker.join();
    const uint64_t elapsed = rgui_loop_now_ms() - start;

    CHECK(state.order == std::vector<char>({'s', 'r', 'r', 'r', 'e'}));
    CHECK_EQ(state.repeats, 3);
    CHECK(state.taskOnLoopThread);
    CHECK_EQ(state.pipeReads, 1);  // removed after the first read: the second byte stays unread
    CHECK(elapsed >= 250);
    close(fds[0]);
    close(fds[1]);
    g_state = nullptr;
}
