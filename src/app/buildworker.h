// SPDX-License-Identifier: Apache-2.0
// The build worker: one thread runs every TinyPdf::BuildPdf, for the live preview and for
// exports. Requests come from the UI thread; results go back to it through
// Platform::PostToUi. Besides the build, the worker does the request's other O(n) and I/O work
// the UI thread must not: the text's word and line counts, and reading and parsing the PDF
// theme file.
#pragma once

#include "app/document.h"
#include "rayomd/tiny_pdf.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
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
    // Cancelled: the build was abandoned (BuildWorker::CancelPreview); nothing else is set.
    TinyPdf::BuildError buildError = TinyPdf::BuildError::None;
    // Empty on success; else BuildErrorText of buildError, or why the PDF could not be read.
    std::string error;
    double ms = 0;  // build and parse
    DocumentStats stats;     // of the text built
    std::string themeFile;   // the request's theme file
    std::string themeError;  // why it could not be used (ExportSettings::themeError)
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
    std::string themeFile;   // the request's theme file
    std::string themeError;  // why it could not be used (ExportSettings::themeError)
};

// What a build error means, as the status line says it after `Export failed: `.
const char* BuildErrorText(TinyPdf::BuildError error);
// `<n> ms`, with one decimal below 10 ms.
std::string FormatMs(double ms);

class BuildWorker {
public:
    using PreviewCallback = std::function<void(PreviewReady)>;
    using ExportCallback = std::function<void(ExportFinished)>;
    // TinyPdf::BuildPdf's signature; tests substitute their own (which should honour
    // PdfOptions::cancel as the engine does).
    using BuildFunction =
        std::function<TinyPdf::BuildResult(const std::string&, const TinyPdf::PdfOptions&, std::string&)>;

    // Starts the thread. The callbacks run on the UI thread, and never after the worker is
    // destroyed. `platform` must outlive the worker.
    BuildWorker(Platform& platform, PreviewCallback onPreview, ExportCallback onExport,
                BuildFunction build = nullptr);
    // Drops the pending requests, cancels the build it runs (preview or export: through
    // PdfOptions::cancel, so a slow build, a URL image fetch among them, ends within the
    // engine's cancel granularity) and joins the thread.
    ~BuildWorker();
    BuildWorker(const BuildWorker&) = delete;
    BuildWorker& operator=(const BuildWorker&) = delete;

    // A preview of `utf8`; replaces a preview still pending, so only the newest is built. With
    // a `themeFile` (UTF-8), the worker reads and parses it into options.theme before the
    // build, again only when the file's size or modification time changed since it last did;
    // its error comes back in PreviewReady::themeError. Without one, options.theme is used as
    // given.
    void RequestPreview(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options, int generation,
                        std::string themeFile = {});
    // An export to `outputPath` (UTF-8): written to `<outputPath>.tmp`, then renamed over it.
    // Exports run in request order, each before any pending preview; a preview being built is
    // cancelled and built again after the exports. With `openAfter`, a finished export is
    // opened with Platform::OpenExternally before onExport runs. `themeFile` as for previews.
    void RequestExport(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                       std::string outputPath, bool openAfter, std::string themeFile = {});
    // Drops the pending preview and cancels the preview being built; the PreviewReady of each
    // comes with BuildError::Cancelled (unless the build finished first). Exports go on.
    void CancelPreview();

private:
    struct Job {
        std::string utf8;
        std::string sourcePath;
        TinyPdf::PdfOptions options;
        int generation = 0;
        std::string outputPath;
        bool openAfter = false;
        std::string themeFile;
    };
    // Shared with the tasks posted to the UI thread, which may run after the worker is gone.
    struct Callbacks {
        bool alive = true;  // UI thread only
        PreviewCallback onPreview;
        ExportCallback onExport;
    };
    // The theme file last read (worker thread only).
    struct ThemeCache {
        std::string file;
        bool known = false;
        uintmax_t size = 0;
        std::filesystem::file_time_type modified;
        TinyPdf::PdfTheme theme;
        std::string error;
    };

    void Run();
    void RunPreview(Job& job);
    void RunExport(Job& job);
    // Sets job.options.theme from job.themeFile (when it has one); the theme's error.
    std::string ApplyTheme(Job& job);

    Platform& platform_;
    BuildFunction build_;
    std::shared_ptr<Callbacks> callbacks_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Job> preview_;  // guarded by mutex_
    std::deque<Job> exports_;     // guarded by mutex_
    bool stop_ = false;           // guarded by mutex_
    bool runningPreview_ = false; // guarded by mutex_: the job being built is a preview
    bool rerunPreview_ = false;   // guarded by mutex_: an export cancelled it; build it again
    // The cancel flag of the job being built (PdfOptions::cancel); reset when a job starts.
    std::atomic<bool> cancel_{false};
    ThemeCache themeCache_;
    std::thread thread_;
};

} // namespace rgui
