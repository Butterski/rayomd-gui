// SPDX-License-Identifier: Apache-2.0
#include "app/buildworker.h"

#include "app/platform.h"
#include "app/safefile.h"
#include "pdf/reader.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <utility>

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
    }
    wake_.notify_all();
    thread_.join();
    callbacks_->alive = false;
}

void BuildWorker::RequestPreview(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                                 int generation) {
    Job job;
    job.utf8 = std::move(utf8);
    job.sourcePath = std::move(sourcePath);
    job.options = std::move(options);
    job.generation = generation;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preview_ = std::move(job);
    }
    wake_.notify_one();
}

void BuildWorker::RequestExport(std::string utf8, std::string sourcePath, TinyPdf::PdfOptions options,
                                std::string outputPath, bool openAfter) {
    Job job;
    job.utf8 = std::move(utf8);
    job.sourcePath = std::move(sourcePath);
    job.options = std::move(options);
    job.outputPath = std::move(outputPath);
    job.openAfter = openAfter;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exports_.push_back(std::move(job));
    }
    wake_.notify_one();
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
        }
        if (isExport) {
            RunExport(job);
        } else {
            RunPreview(job);
        }
    }
}

void BuildWorker::RunPreview(Job& job) {
    const auto start = std::chrono::steady_clock::now();
    job.options.sourcePath = job.sourcePath;
    std::string bytes;
    const TinyPdf::BuildResult result = build_(job.utf8, job.options, bytes);
    PreviewReady ready;
    ready.generation = job.generation;
    ready.pages = result.pages;
    ready.missingCharacters = result.missingCharacters;
    ready.failedImages = result.failedImages;
    ready.buildError = result.error;
    if (!result.Ok()) {
        ready.error = BuildErrorText(result.error);
    } else {
        std::string parseError;
        ready.pdf = pdf::PdfFile::Parse(std::move(bytes), &parseError);
        if (!ready.pdf) ready.error = "the preview PDF cannot be read (" + parseError + ")";
    }
    ready.ms = MsSince(start);
    platform_.PostToUi([callbacks = callbacks_, ready = std::move(ready)]() mutable {
        if (callbacks->alive && callbacks->onPreview) callbacks->onPreview(std::move(ready));
    });
}

void BuildWorker::RunExport(Job& job) {
    const auto start = std::chrono::steady_clock::now();
    job.options.sourcePath = job.sourcePath;
    std::string bytes;
    const TinyPdf::BuildResult result = build_(job.utf8, job.options, bytes);
    ExportFinished finished;
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
