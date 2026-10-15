// SPDX-License-Identifier: Apache-2.0
// Clipboard and drag-and-drop transfers (see data_transfer.h).
#include "platform/linux/data_transfer.h"

#include "platform/linux/loop.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <utility>
#include <vector>

namespace rgui::wl {

Transfers::Transfers(int idleTimeoutMs, size_t maxIncomingBytes)
    : idleTimeoutMs_(std::max(idleTimeoutMs, 1)), maxIncomingBytes_(maxIncomingBytes) {}

Transfers::~Transfers() {
    if (tick_) rgui_loop_cancel_timer(tick_);
    for (const auto& t : transfers_) {
        rgui_loop_remove_fd(t.first);
        close(t.first);
    }
}

size_t Transfers::Outgoing() const {
    return static_cast<size_t>(
        std::count_if(transfers_.begin(), transfers_.end(), [](const auto& t) { return t.second.writing; }));
}

size_t Transfers::Incoming() const { return transfers_.size() - Outgoing(); }

bool Transfers::Send(int fd, std::shared_ptr<const std::string> text) {
    struct stat st;
    if (!text || Outgoing() >= kMaxOutgoing || fstat(fd, &st) != 0 || !(S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))) {
        close(fd);
        return false;
    }
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return false;
    }
    Transfer t;
    t.writing = true;
    t.text = std::move(text);
    return Add(fd, std::move(t), POLLOUT);
}

void Transfers::Receive(int fd, std::function<void(std::string)> done) {
    Transfer t;
    t.done = std::move(done);
    Add(fd, std::move(t), POLLIN);
}

void Transfers::ReceivePaste(int fd, std::function<void(std::string)> done) {
    for (const auto& t : transfers_) {
        if (t.second.paste) {
            Finish(t.first, false);  // one paste at a time: the newest wins
            break;
        }
    }
    Transfer t;
    t.paste = true;
    t.done = std::move(done);
    Add(fd, std::move(t), POLLIN);
}

bool Transfers::Add(int fd, Transfer transfer, uint32_t events) {
    transfer.lastActivity = rgui_loop_now_ms();
    transfers_[fd] = std::move(transfer);
    if (rgui_loop_add_fd(fd, events, events == POLLOUT ? OnWritable : OnReadable, this) != 0) {
        Finish(fd, false);
        return false;
    }
    if (!tick_) {
        // Checked a few times per timeout: a stalled transfer ends within 1.25 timeouts.
        const uint32_t period = static_cast<uint32_t>(std::max(1, std::min(1000, idleTimeoutMs_ / 4)));
        tick_ = rgui_loop_add_timer(period, period, OnTick, this);
    }
    return true;
}

void Transfers::OnReadable(int fd, uint32_t revents, void* user) {
    auto* self = static_cast<Transfers*>(user);
    auto it = self->transfers_.find(fd);
    if (it == self->transfers_.end()) return;
    Transfer& t = it->second;
    char buffer[65536];
    for (;;) {
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n > 0) {
            if (t.data.size() + static_cast<size_t>(n) > self->maxIncomingBytes_) {
                self->Finish(fd, false);
                return;
            }
            t.data.append(buffer, static_cast<size_t>(n));
            t.lastActivity = rgui_loop_now_ms();
            continue;
        }
        if (n == 0) {
            self->Finish(fd, true);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (revents & (POLLERR | POLLNVAL)) self->Finish(fd, false);
            return;
        }
        self->Finish(fd, false);
        return;
    }
}

void Transfers::OnWritable(int fd, uint32_t, void* user) {
    auto* self = static_cast<Transfers*>(user);
    auto it = self->transfers_.find(fd);
    if (it == self->transfers_.end()) return;
    Transfer& t = it->second;
    while (t.offset < t.text->size()) {
        const ssize_t n = write(fd, t.text->data() + t.offset, t.text->size() - t.offset);
        if (n > 0) {
            t.offset += static_cast<size_t>(n);
            t.lastActivity = rgui_loop_now_ms();
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  // wait for the reader
        break;  // EPIPE: the reader went away
    }
    self->Finish(fd, true);
}

void Transfers::OnTick(uint64_t, void* user) {
    auto* self = static_cast<Transfers*>(user);
    const uint64_t now = rgui_loop_now_ms();
    std::vector<int> idle;
    for (const auto& t : self->transfers_) {
        if (now - t.second.lastActivity >= static_cast<uint64_t>(self->idleTimeoutMs_)) idle.push_back(t.first);
    }
    for (int fd : idle) self->Finish(fd, false);
}

void Transfers::Finish(int fd, bool ok) {
    auto it = transfers_.find(fd);
    if (it == transfers_.end()) return;
    Transfer t = std::move(it->second);
    transfers_.erase(it);
    rgui_loop_remove_fd(fd);
    close(fd);
    if (transfers_.empty() && tick_) {
        rgui_loop_cancel_timer(tick_);
        tick_ = 0;
    }
    if (!t.writing && t.done) t.done(ok ? std::move(t.data) : std::string());
}

} // namespace rgui::wl
