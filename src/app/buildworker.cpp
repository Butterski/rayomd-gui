// SPDX-License-Identifier: Apache-2.0
#include "app/buildworker.h"

#include "app/exportsettings.h"
#include "app/platform.h"
#include "app/safefile.h"
#include "pdf/reader.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace rgui {
namespace {

namespace fs = std::filesystem;

double MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::string Pages(uint32_t pages) {
    return std::to_string(pages) + (pages == 1 ? " page" : " pages");
}

} // namespace

const char* BuildErrorText(TinyPdf::BuildError error) {
    switch (error) {
    case TinyPdf::BuildError::None: return "";
    case TinyPdf::BuildError::FontUnavailable: return "no font is available";
    case TinyPdf::BuildError::SourceTooLarge: return "the document is larger than the embedded-source limit (10 MiB)";
    case TinyPdf::BuildError::InvalidSourceUtf8: return "the document is not valid UTF-8";
    case TinyPdf::BuildError::ReversiblePdfTooLarge: return "the PDF would pass the reversible-profile limit (256 MiB)";
    case TinyPdf::BuildError::ThemeFontUnavailable: return "the PDF theme's font cannot be read";
    case TinyPdf::BuildError::PdfaFontUnavailable:
        return "PDF/A and PDF/UA need a TrueType or OpenType font; none was found";
    case TinyPdf::BuildError::Cancelled: return "the build was cancelled";
    }
    return "the engine reported an unknown error";
}

std::string FormatMs(double ms) {
    char text[32];
    if (ms < 0) ms = 0;
    if (ms < 9.95) {
        std::snprintf(text, sizeof text, "%.1f ms", ms);
    } else {
        std::snprintf(text, sizeof text, "%.0f ms", ms);
    }
    return text;
}

BuildWorker::BuildWorker(Platform& platform, PreviewCallback onPreview, ExportCallback onExport,
                         BuildFunction build)
    : platform_(platform),
      build_(build ? std::move(build) : BuildFunction(&TinyPdf::BuildPdf)),
      callbacks_(std::make_shared<Callbacks>()) {
    callbacks_->onPreview = std::move(onPreview);
    callbacks_->onExport = std::move(onExport);
    thread_ = std::thread([this] { Run(); });
}

BuildWorker::~BuildWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        preview_.reset();
        exports_.clear();
        cancel_.store(true);
    }
    wake_.notify_all();
    thread_.join();
    callbacks_->alive = false;
}

void BuildWorker::RequestPreview(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                                 int generation, std::string themeFile) {
    Job job;
    job.utf8 = std::move(utf8);
    job.sourcePath = std::move(sourcePath);
    job.options = std::move(options);
    job.generation = generation;
    job.themeFile = std::move(themeFile);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preview_ = std::move(job);
    }
    wake_.notify_one();
}

void BuildWorker::RequestExport(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                                std::string outputPath, bool openAfter, std::string themeFile) {
    Job job;
    job.utf8 = std::move(utf8);
    job.sourcePath = std::move(sourcePath);
    job.options = std::move(options);
    job.outputPath = std::move(outputPath);
    job.openAfter = openAfter;
    job.themeFile = std::move(themeFile);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exports_.push_back(std::move(job));
        // The export goes first: the preview being built stops and is built again after it.
        if (runningPreview_ && !rerunPreview_) {
            rerunPreview_ = true;
            cancel_.store(true);
        }
    }
    wake_.notify_one();
}

void BuildWorker::CancelPreview() {
    PreviewReady dropped;
    dropped.buildError = TinyPdf::BuildError::Cancelled;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rerunPreview_ = false;
        if (runningPreview_) cancel_.store(true);
        if (!preview_) return;
        dropped.generation = preview_->generation;
        preview_.reset();
    }
    // A preview dropped before it started is answered too: every request gets one PreviewReady
    // (or is replaced by a newer one).
    platform_.PostToUi([callbacks = callbacks_, dropped = std::move(dropped)]() mutable {
        if (callbacks->alive && callbacks->onPreview) callbacks->onPreview(std::move(dropped));
    });
}

void BuildWorker::Run() {
    for (;;) {
        Job job;
        bool isExport = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !exports_.empty() || preview_.has_value(); });
            if (stop_) return;
            if (!exports_.empty()) {
                job = std::move(exports_.front());
                exports_.pop_front();
                isExport = true;
            } else {
                job = std::move(*preview_);
                preview_.reset();
            }
            runningPreview_ = !isExport;
            rerunPreview_ = false;
            cancel_.store(false);
        }
        job.options.cancel = &cancel_;
        if (isExport) {
            RunExport(job);
        } else {
            RunPreview(job);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        runningPreview_ = false;
    }
}

std::string BuildWorker::ApplyTheme(Job& job) {
    if (job.themeFile.empty()) return {};
    ThemeCache& cache = themeCache_;
    const fs::path path = fs::u8path(job.themeFile);
    std::error_code sizeError, timeError;
    const uintmax_t size = fs::file_size(path, sizeError);
    const fs::file_time_type modified = fs::last_write_time(path, timeError);
    const bool stamped = !sizeError && !timeError;
    if (!(cache.known && stamped && cache.file == job.themeFile && cache.size == size && cache.modified == modified)) {
        ExportSettings settings;
        settings.themeFile = job.themeFile;
        settings.ReloadTheme();
        cache.file = job.themeFile;
        cache.known = stamped;  // a file that cannot be stat'ed is read again next time
        cache.size = size;
        cache.modified = modified;
        cache.theme = std::move(settings.theme);
        cache.error = std::move(settings.themeError);
    }
    job.options.theme = cache.theme;
    return cache.error;
}

void BuildWorker::RunPreview(Job& job) {
    const auto start = std::chrono::steady_clock::now();
    job.options.sourcePath = job.sourcePath;
    PreviewReady ready;
    ready.themeFile = job.themeFile;
    ready.themeError = ApplyTheme(job);
    std::string bytes;
    // Where each top-level block is laid out, for the source sync; fake builds leave it empty.
    std::vector<TinyPdf::SourceBlock> blocks;
    job.options.sourceMap = &blocks;
    const TinyPdf::BuildResult result = build_(job.utf8, job.options, bytes);
    job.options.sourceMap = nullptr;
    ready.generation = job.generation;
    bool abandoned = result.error == TinyPdf::BuildError::Cancelled;
    if (!abandoned && cancel_.load()) {
        // Built before the cancel came: an export's goes on (the PDF is current), a dropped
        // preview's is not parsed.
        std::lock_guard<std::mutex> lock(mutex_);
        abandoned = stop_ || !rerunPreview_;
    }
    if (abandoned) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) return;
            // Cancelled for an export: built again after it, unless a newer preview came.
            if (result.error == TinyPdf::BuildError::Cancelled && rerunPreview_) {
                if (!preview_) preview_ = std::move(job);
                return;
            }
        }
        PreviewReady cancelled;
        cancelled.generation = job.generation;
        cancelled.buildError = TinyPdf::BuildError::Cancelled;
        platform_.PostToUi([callbacks = callbacks_, cancelled = std::move(cancelled)]() mutable {
            if (callbacks->alive && callbacks->onPreview) callbacks->onPreview(std::move(cancelled));
        });
        return;
    }
    ready.stats = Document::ComputeStats(job.utf8);
    ready.pages = result.pages;
    ready.missingCharacters = result.missingCharacters;
    ready.failedImages = result.failedImages;
    ready.buildError = result.error;
    if (!result.Ok()) {
        ready.error = BuildErrorText(result.error);
    } else {
        std::string parseError;
        ready.pdf = pdf::PdfFile::Parse(std::move(bytes), &parseError);
        if (!ready.pdf) {
            ready.error = "the preview PDF cannot be read (" + parseError + ")";
        } else {
            std::vector<double> heights(static_cast<size_t>(ready.pdf->PageCount()));
            for (size_t i = 0; i < heights.size(); i++) {
                const pdf::PdfRect box = ready.pdf->PageMediaBox(static_cast<int>(i));
                heights[i] = box.y1 - box.y0;
            }
            ready.sourceMap = SourceMap::FromEngine(blocks, job.utf8, heights);
        }
    }
    ready.ms = MsSince(start);
    platform_.PostToUi([callbacks = callbacks_, ready = std::move(ready)]() mutable {
        if (callbacks->alive && callbacks->onPreview) callbacks->onPreview(std::move(ready));
    });
}

void BuildWorker::RunExport(Job& job) {
    const auto start = std::chrono::steady_clock::now();
    job.options.sourcePath = job.sourcePath;
    ExportFinished finished;
    finished.themeFile = job.themeFile;
    finished.themeError = ApplyTheme(job);
    std::string bytes;
    const TinyPdf::BuildResult result = build_(job.utf8, job.options, bytes);
    finished.path = job.outputPath;
    finished.pages = result.pages;
    finished.missingCharacters = result.missingCharacters;
    finished.failedImages = result.failedImages;
    const fs::path output = fs::u8path(job.outputPath);
    if (!result.Ok()) {
        finished.message = std::string("Export failed: ") + BuildErrorText(result.error);
    } else {
        if (!WriteFileAtomically(output, bytes, WriteMode::Output, nullptr)) {
            finished.message = "Export failed: cannot write " + job.outputPath;
        } else {
            finished.ok = true;
        }
    }
    finished.ms = MsSince(start);
    if (finished.ok) {
        finished.message = "Exported " + output.filename().u8string() + " (" + Pages(result.pages) + ", " +
                           FormatMs(finished.ms) + ")";
    }
    const bool open = finished.ok && job.openAfter;
    Platform* platform = &platform_;
    platform_.PostToUi([callbacks = callbacks_, platform, open, finished = std::move(finished)]() mutable {
        if (!callbacks->alive) return;
        if (open) platform->OpenExternally(finished.path);
        if (callbacks->onExport) callbacks->onExport(std::move(finished));
    });
}

} // namespace rgui
