// SPDX-License-Identifier: Apache-2.0
// The tests' Platform: no window, no system. Posted tasks and timers run only when the test
// pumps; time is a fake clock the test advances, so timer order is deterministic. Clipboard,
// file dialogs and "open externally" live in memory; the test scripts the dialog answers and
// reads back what the app asked for. Directories lie under the build directory.
#pragma once

#include "app/platform.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rgui::test {

class HeadlessPlatform final : public Platform {
public:
    HeadlessPlatform();
    ~HeadlessPlatform() override = default;

    // --- Driving the platform (test thread = the UI thread) ---

    // Runs every posted task, also those posted while it runs, then every timer due at the
    // current fake time, in deadline order (equal deadlines in start order), until nothing is
    // left to run. Returns how many tasks and timers ran.
    int Pump();
    // Advances the fake clock by `ms`, firing the timers due on the way in deadline order (the
    // clock stands at each timer's deadline while it runs), then pumps.
    int Advance(int ms);
    // Pumps, and while `done()` is false waits (real time, at most `timeoutMs`) for a worker to
    // post and pumps again. False when it timed out.
    bool PumpUntil(const std::function<bool()>& done, int timeoutMs);
    // Fake milliseconds since the platform was made.
    int64_t Now() const { return now_; }
    // Timers started and neither fired nor cancelled.
    size_t PendingTimers() const { return timers_.size(); }

    // The answers the next file dialogs give, in order; a dialog without a scripted answer is
    // Cancelled. Callbacks run on the next Pump.
    void ScriptDialog(FileDialogResult result) { dialogAnswers_.push_back(std::move(result)); }

    // --- What the app did ---

    struct DialogRequest {
        FileDialog kind;
        std::string suggestedPath;
    };
    std::vector<DialogRequest> dialogs;
    std::vector<std::string> openedExternally;
    std::string title;
    Cursor cursor = Cursor::Arrow;
    gfx::RectI textInputRect{};
    bool textInputEnabled = false;
    int frameRequests = 0;
    // The last presented frame (tightly packed, width x height) and its damage.
    std::vector<uint32_t> lastFrame;
    int lastFrameWidth = 0;
    int lastFrameHeight = 0;
    std::vector<gfx::RectI> lastDamage;
    int presents = 0;
    int moves = 0;
    std::vector<Edge> resizes;
    int maximizeToggles = 0;
    int minimizes = 0;
    bool quit = false;
    std::string clipboard;

    // --- What the test sets ---

    bool clientDecorations = false;
    std::optional<std::filesystem::path> omarchyThemeDir;
    std::optional<std::filesystem::path> omarchyUserShellToml;

    // --- Platform ---

    void SetTitle(std::string_view text) override { title = std::string(text); }
    void SetCursor(Cursor shape) override { cursor = shape; }
    void SetTextInputRect(gfx::RectI rect, bool enabled) override {
        textInputRect = rect;
        textInputEnabled = enabled;
    }
    void RequestFrame() override { frameRequests++; }
    void Present(const gfx::Surface& frame, const std::vector<gfx::RectI>& damage) override;
    bool NeedsClientDecorations() const override { return clientDecorations; }
    void StartMove() override { moves++; }
    void StartResize(Edge edge) override { resizes.push_back(edge); }
    void ToggleMaximize() override { maximizeToggles++; }
    void Minimize() override { minimizes++; }

    void SetClipboardText(std::string text) override { clipboard = std::move(text); }
    void RequestClipboardText(std::function<void(std::string)> callback) override;

    void ShowFileDialog(FileDialog kind, std::string suggestedPath,
                        std::function<void(FileDialogResult)> callback) override;
    void OpenExternally(const std::string& path) override { openedExternally.push_back(path); }

    void PostToUi(std::function<void()> task) override;
    uint64_t StartTimer(int ms, std::function<void()> callback) override;
    void CancelTimer(uint64_t id) override;
    void Quit() override { quit = true; }

    std::filesystem::path ConfigDir() const override;
    std::filesystem::path ImageCacheDir() const override;
    std::filesystem::path DocumentsDir() const override;
    std::optional<std::filesystem::path> OmarchyThemeDir() const override { return omarchyThemeDir; }
    std::optional<std::filesystem::path> OmarchyUserShellToml() const override {
        return omarchyUserShellToml;
    }

private:
    struct Timer {
        int64_t deadline;
        uint64_t id;
        std::function<void()> callback;
    };

    // The timer `id`, or end() when it already fired or was cancelled.
    std::vector<Timer>::iterator Find(uint64_t id);
    // Runs the posted tasks queued now; how many ran.
    int RunPosted();
    // Runs the earliest timer due at or before `until`; false when none is.
    bool RunNextTimer(int64_t until);

    std::mutex mutex_;
    std::condition_variable posted_;
    std::deque<std::function<void()>> tasks_;  // guarded by mutex_
    std::vector<Timer> timers_;                // UI thread only
    std::deque<FileDialogResult> dialogAnswers_;
    int64_t now_ = 0;
    uint64_t nextTimerId_ = 1;
};

} // namespace rgui::test
