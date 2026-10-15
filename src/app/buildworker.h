// SPDX-License-Identifier: Apache-2.0
// The build worker: one thread runs every TinyPdf::BuildPdf, for the live preview and for
// exports. Requests come from the UI thread; results go back to it through
// Platform::PostToUi.
#pragma once

#include "rayomd/tiny_pdf.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace rgui {

class Platform;

namespace pdf {
class PdfFile;
}

struct PreviewReady {
    int generation = 0;
    // The PDF parsed for the preview renderer; nullptr when the build or the parse failed.
    std::shared_ptr<const pdf::PdfFile> pdf;
    uint32_t pages = 0;
    uint32_t missingCharacters = 0;
    uint32_t failedImages = 0;
    TinyPdf::BuildError buildError = TinyPdf::BuildError::None;
    // Empty on success; else BuildErrorText of buildError, or why the PDF could not be read.
    std::string error;
    double ms = 0;  // build and parse
};

struct ExportFinished {
    bool ok = false;
    // `Exported <file> (<pages> pages, <ms> ms)` or `Export failed: <why>`.
    std::string message;
    std::string path;  // the output path (UTF-8) as requested
    uint32_t pages = 0;
    uint32_t missingCharacters = 0;
    uint32_t failedImages = 0;
    double ms = 0;  // build and write
};

// What a build error means, as the status line says it after `Export failed: `.
const char* BuildErrorText(TinyPdf::BuildError error);
// `<n> ms`, with one decimal below 10 ms.
std::string FormatMs(double ms);

class BuildWorker {
public:
    using PreviewCallback = std::function<void(PreviewReady)>;
    using ExportCallback = std::function<void(ExportFinished)>;
    // TinyPdf::BuildPdf's signature; tests substitute their own.
    using BuildFunction =
        std::function<TinyPdf::BuildResult(const std::string&, const TinyPdf::PdfOptions&, std::string&)>;

    // Starts the thread. The callbacks run on the UI thread, and never after the worker is
    // destroyed. `platform` must outlive the worker.
    BuildWorker(Platform& platform, PreviewCallback onPreview, ExportCallback onExport,
                BuildFunction build = nullptr);
    // Drops the pending requests and joins the thread (after the build it runs, if any).
    ~BuildWorker();
    BuildWorker(const BuildWorker&) = delete;
    BuildWorker& operator=(const BuildWorker&) = delete;

    // A preview of `utf8`; replaces a preview still pending, so only the newest is built.
    void RequestPreview(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options, int generation);
    // An export to `outputPath` (UTF-8): written to a temporary file, then renamed over it
    // (WriteFileAtomically, WriteMode::Output).
    // Exports run in request order, each before any pending preview. With `openAfter`, a
    // finished export is opened with Platform::OpenExternally before onExport runs.
    void RequestExport(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                       std::string outputPath, bool openAfter);

private:
    struct Job {
        std::string utf8;
        std::string sourcePath;
        TinyPdf::PdfOptions options;
        int generation = 0;
        std::string outputPath;
        bool openAfter = false;
    };
    // Shared with the tasks posted to the UI thread, which may run after the worker is gone.
    struct Callbacks {
        bool alive = true;  // UI thread only
        PreviewCallback onPreview;
        ExportCallback onExport;
    };

    void Run();
    void RunPreview(Job& job);
    void RunExport(Job& job);

    Platform& platform_;
    BuildFunction build_;
    std::shared_ptr<Callbacks> callbacks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> preview_;  // guarded by mutex_
    std::deque<Job> exports_;     // guarded by mutex_
    bool stop_ = false;           // guarded by mutex_
    std::thread thread_;
};

} // namespace rgui
