// SPDX-License-Identifier: Apache-2.0
// Clipboard and drag-and-drop transfers: the pipes wl_data_source.send hands us to write the
// selection into, and the pipes we read a paste or a drop from. Every transfer runs on the event
// loop (loop.h, initialised before use) without blocking the UI thread, and is bounded:
//   - at most kMaxOutgoing sends at once; another client asking for more gets its fd closed;
//   - a send fd must be a pipe or a socket (a regular file would make write() block);
//   - a transfer that makes no progress for the idle timeout is abandoned, both ways;
//   - a paste replaces the paste still in flight, which ends with "";
//   - a receive stops at the byte limit.
// So no client can make us hold descriptors until RLIMIT_NOFILE, after which the compositor's
// next fd-carrying event would cost the connection.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace rgui::wl {

class Transfers {
public:
    static constexpr size_t kMaxOutgoing = 8;
    static constexpr int kIdleTimeoutMs = 5000;
    static constexpr size_t kMaxIncomingBytes = size_t{128} << 20;

    explicit Transfers(int idleTimeoutMs = kIdleTimeoutMs, size_t maxIncomingBytes = kMaxIncomingBytes);
    // Closes every descriptor; no callback runs.
    ~Transfers();
    Transfers(const Transfers&) = delete;
    Transfers& operator=(const Transfers&) = delete;

    // Writes `text` into `fd`, which is taken (closed when done). False, with `fd` closed at
    // once, when it is not a pipe or socket or kMaxOutgoing sends are in flight.
    bool Send(int fd, std::shared_ptr<const std::string> text);
    // Reads `fd` (taken) to its end; `done` gets the bytes, or "" on an error, more than the byte
    // limit or the idle timeout. `done` runs from the loop (inside this call only when the loop
    // cannot watch `fd`).
    void Receive(int fd, std::function<void(std::string)> done);
    // The same for a paste: a paste still in flight is cancelled first, its `done` getting ""
    // inside this call.
    void ReceivePaste(int fd, std::function<void(std::string)> done);

    size_t Outgoing() const;
    size_t Incoming() const;

private:
    struct Transfer {
        bool writing = false;
        bool paste = false;
        std::shared_ptr<const std::string> text;  // writing
        size_t offset = 0;
        std::string data;  // reading
        std::function<void(std::string)> done;
        uint64_t lastActivity = 0;
    };

    bool Add(int fd, Transfer transfer, uint32_t events);
    void Finish(int fd, bool ok);
    static void OnReadable(int fd, uint32_t revents, void* user);
    static void OnWritable(int fd, uint32_t revents, void* user);
    static void OnTick(uint64_t id, void* user);

    int idleTimeoutMs_;
    size_t maxIncomingBytes_;
    std::map<int, Transfer> transfers_;
    uint64_t tick_ = 0;
};

} // namespace rgui::wl
