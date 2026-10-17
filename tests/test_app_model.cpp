// SPDX-License-Identifier: Apache-2.0
// The app's model: export settings, the document, the build worker and the arguments.
#include "app_harness.h"
#include "headless_platform.h"
#include "runner.h"

#include "app/args.h"
#include "app/buildworker.h"
#include "app/document.h"
#include "app/exportsettings.h"
#include "common/file_io.h"
#include "core/rayomd_pdf_source.h"
#include "pdf/reader.h"
#include "ui/settingsfile.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using rgui::Document;
using rgui::ExportSettings;

// An empty directory of its own for a test, under the build directory.
fs::path TempDir(const char* name) {
    const fs::path dir = fs::u8path(rgui::test::BuildDir()) / "test-app" / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    return dir;
}

std::string Utf8(const fs::path& path) {
    return path.u8string();
}

std::string ReadFile(const fs::path& path) {
    std::string content;
    if (!RayoMd::File::ReadAll(path, 1u << 30, content)) return "<unreadable>";
    return content;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

// ---------------------------------------------------------------------------------------------
// Export settings

RGUI_TEST("app.exportsettings.defaults") {
    ExportSettings settings;
    const TinyPdf::PdfOptions options = settings.ToPdfOptions("/doc/a.md", false);
    const TinyPdf::PdfOptions engine;  // the CLI's defaults
    CHECK(options.style == engine.style);
    CHECK(options.margin.preset == TinyPdf::MarginPreset::Normal);
    CHECK_EQ(options.pageSize.width, engine.pageSize.width);
    CHECK_EQ(options.pageSize.height, engine.pageSize.height);
    CHECK_EQ(options.sourcePath, std::string("/doc/a.md"));
    CHECK_EQ(options.enableUrlImages, false);
    CHECK_EQ(options.embedSource, false);
    CHECK_EQ(options.pageNumbers, false);
    CHECK_EQ(options.compress, false);
    CHECK_EQ(options.fastImages, false);
    CHECK_EQ(options.pdfa, false);
    CHECK_EQ(options.pdfua, false);
    CHECK_EQ(options.highlightCode, true);
    CHECK_EQ(options.toc, false);
    CHECK_EQ(options.tocDepth, 3);
    CHECK_EQ(options.numberHeadings, engine.numberHeadings);
    CHECK_EQ(options.pageBreakBeforeLevel, engine.pageBreakBeforeLevel);
    CHECK(options.dialect == engine.dialect);
    CHECK(options.imageCacheDirectory.empty());
    CHECK(options.theme.fontPath.empty());
    CHECK_EQ(settings.openAfterExport, true);
}

RGUI_TEST("app.exportsettings.mapping") {
    ExportSettings settings;
    settings.imageCacheDirectory = "/cache/images";
    CHECK_EQ(settings.ToPdfOptions("", false).imageCacheDirectory, std::string("/cache/images"));

    settings.style = ExportSettings::kModern;
    CHECK(settings.ToPdfOptions("", false).style == TinyPdf::PdfStyle::Modern);
    settings.style = ExportSettings::kTech;
    CHECK(settings.ToPdfOptions("", false).style == TinyPdf::PdfStyle::Tech);

    settings.margin = ExportSettings::kCompact;
    CHECK(settings.ToPdfOptions("", false).margin.preset == TinyPdf::MarginPreset::Compact);
    settings.margin = ExportSettings::kWide;
    CHECK(settings.ToPdfOptions("", false).margin.preset == TinyPdf::MarginPreset::Wide);
    settings.margin = ExportSettings::kCustomMargin;
    settings.customMarginInches = 1.5;
    const TinyPdf::PdfMargin custom = settings.ToPdfOptions("", false).margin;
    CHECK(custom.preset == TinyPdf::MarginPreset::Custom);
    CHECK_NEAR(custom.customPoints, 108.0, 1e-9);

    struct Size {
        int index;
        double width, height;
    };
    const Size sizes[] = {{ExportSettings::kA4, 595, 842}, {ExportSettings::kA3, 842, 1191},
                          {ExportSettings::kA5, 420, 595}, {ExportSettings::kLetter, 612, 792},
                          {ExportSettings::kLegal, 612, 1008}};
    for (const Size& size : sizes) {
        settings.pageSize = size.index;
        settings.landscape = false;
        CHECK_EQ(settings.ToPdfOptions("", false).pageSize.width, size.width);
        CHECK_EQ(settings.ToPdfOptions("", false).pageSize.height, size.height);
        settings.landscape = true;
        CHECK_EQ(settings.ToPdfOptions("", false).pageSize.width, size.height);
        CHECK_EQ(settings.ToPdfOptions("", false).pageSize.height, size.width);
    }
    settings.pageSize = ExportSettings::kLetter;
    settings.landscape = true;
    CHECK_EQ(settings.ToPdfOptions("", false).pageSize.width, 792.0);
    CHECK_EQ(settings.ToPdfOptions("", false).pageSize.height, 612.0);
    settings.pageSize = ExportSettings::kCustomPage;
    settings.landscape = false;
    settings.customWidthMm = 100;
    settings.customHeightMm = 200;
    CHECK_NEAR(settings.ToPdfOptions("", false).pageSize.width, 283.46, 0.005);
    CHECK_NEAR(settings.ToPdfOptions("", false).pageSize.height, 566.93, 0.005);

    settings.toc = true;
    settings.tocDepth = 5;
    settings.pageNumbers = true;
    settings.compress = true;
    settings.fastImages = true;
    settings.pdfa = true;
    settings.pdfua = true;
    settings.embedSource = true;
    settings.highlightCode = false;
    settings.urlImages = true;
    TinyPdf::PdfOptions options = settings.ToPdfOptions("/doc/a.md", false);
    CHECK_EQ(options.toc, true);
    CHECK_EQ(options.tocDepth, 5);
    // Numbering is off until checked, whatever its depth; the page-break index is the level.
    settings.numberDepth = 4;
    CHECK_EQ(settings.ToPdfOptions("", false).numberHeadings, 0);
    settings.numberHeadings = true;
    CHECK_EQ(settings.ToPdfOptions("", false).numberHeadings, 4);
    settings.pageBreakBefore = ExportSettings::kBreakBeforeH2;
    CHECK_EQ(settings.ToPdfOptions("", false).pageBreakBeforeLevel, 2);
    settings.dialect = ExportSettings::kObsidian;
    CHECK(settings.ToPdfOptions("", true).dialect == TinyPdf::MarkdownDialect::Obsidian);
    CHECK_EQ(options.pageNumbers, true);
    CHECK_EQ(options.compress, true);
    CHECK_EQ(options.fastImages, true);
    CHECK_EQ(options.pdfa, true);
    CHECK_EQ(options.pdfua, true);
    CHECK_EQ(options.embedSource, true);
    CHECK_EQ(options.highlightCode, false);
#ifdef _WIN32
    CHECK_EQ(options.enableUrlImages, true);
#else
    CHECK_EQ(options.enableUrlImages, false);  // the Linux engine has no URL images
#endif

    // A preview never compresses and never embeds the source; the rest stays.
    options = settings.ToPdfOptions("/doc/a.md", true);
    CHECK_EQ(options.compress, false);
    CHECK_EQ(options.embedSource, false);
    CHECK_EQ(options.pdfa, true);
    CHECK_EQ(options.toc, true);
    CHECK_EQ(options.sourcePath, std::string("/doc/a.md"));
}

RGUI_TEST("app.exportsettings.theme") {
    const fs::path dir = TempDir("theme");
    ExportSettings settings;

    settings.themeFile = Utf8(dir / "missing.theme");
    CHECK(!settings.ReloadTheme());
    CHECK_EQ(settings.themeError, "cannot read " + settings.themeFile);

    CHECK(RayoMd::File::WriteAll(dir / "good.theme", "heading-color = #112233\nfont = fonts/Body.ttf\n"));
    settings.themeFile = Utf8(dir / "good.theme");
    CHECK(settings.ReloadTheme());
    CHECK(settings.themeError.empty());
    TinyPdf::PdfOptions options = settings.ToPdfOptions("", false);
    CHECK_EQ(options.theme.headingColor, 0x112233);
    // Relative paths are the theme file's.
    CHECK_EQ(options.theme.fontPath, Utf8(dir) + "/fonts/Body.ttf");

    CHECK(RayoMd::File::WriteAll(dir / "bad.theme", "heading-color = #112233\nwobble = 3\n"));
    settings.themeFile = Utf8(dir / "bad.theme");
    CHECK(!settings.ReloadTheme());
    CHECK(settings.themeError.find("line 2") != std::string::npos);
    CHECK(settings.themeError.find("wobble") != std::string::npos);
    // A bad file leaves no half-read theme behind.
    CHECK_EQ(settings.ToPdfOptions("", false).theme.headingColor, TinyPdf::PdfTheme::kStyleColor);

    settings.themeFile.clear();
    CHECK(settings.ReloadTheme());
    CHECK(settings.themeError.empty());
}

RGUI_TEST("app.exportsettings.persist") {
    const fs::path dir = TempDir("persist");
    ExportSettings settings;
    settings.style = ExportSettings::kTech;
    settings.margin = ExportSettings::kCustomMargin;
    settings.customMarginInches = 0.35;
    settings.pageSize = ExportSettings::kCustomPage;
    settings.landscape = true;
    settings.customWidthMm = 123.4;
    settings.customHeightMm = 432.1;
    settings.toc = true;
    settings.tocDepth = 6;
    settings.numberHeadings = true;
    settings.numberDepth = 2;
    settings.pageBreakBefore = ExportSettings::kBreakBeforeH3;
    settings.dialect = ExportSettings::kObsidian;
    settings.pageNumbers = true;
    settings.compress = true;
    settings.fastImages = true;
    settings.pdfa = true;
    settings.pdfua = true;
    settings.embedSource = true;
    settings.highlightCode = false;
    settings.urlImages = true;
    settings.themeFile = Utf8(dir / "zażółć.theme");
    settings.openAfterExport = false;

    rgui::ui::SettingsFile file;
    settings.Store(file);
    CHECK(file.Has("export.customMarginInches"));
    CHECK(file.Save(dir / "settings.ini"));
    rgui::ui::SettingsFile reread;
    CHECK(reread.Load(dir / "settings.ini"));
    ExportSettings loaded;
    loaded.Load(reread);
    CHECK_EQ(loaded.style, settings.style);
    CHECK_EQ(loaded.margin, settings.margin);
    CHECK_EQ(loaded.customMarginInches, settings.customMarginInches);
    CHECK_EQ(loaded.pageSize, settings.pageSize);
    CHECK_EQ(loaded.landscape, true);
    CHECK_EQ(loaded.customWidthMm, settings.customWidthMm);
    CHECK_EQ(loaded.customHeightMm, settings.customHeightMm);
    CHECK_EQ(loaded.toc, true);
    CHECK_EQ(loaded.tocDepth, 6);
    CHECK_EQ(loaded.numberHeadings, true);
    CHECK_EQ(loaded.numberDepth, 2);
    CHECK_EQ(loaded.pageBreakBefore, ExportSettings::kBreakBeforeH3);
    CHECK_EQ(loaded.dialect, ExportSettings::kObsidian);
    CHECK_EQ(loaded.pageNumbers, true);
    CHECK_EQ(loaded.compress, true);
    CHECK_EQ(loaded.fastImages, true);
    CHECK_EQ(loaded.pdfa, true);
    CHECK_EQ(loaded.pdfua, true);
    CHECK_EQ(loaded.embedSource, true);
    CHECK_EQ(loaded.highlightCode, false);
    CHECK_EQ(loaded.urlImages, true);
    CHECK_EQ(loaded.themeFile, settings.themeFile);
    CHECK_EQ(loaded.openAfterExport, false);
    // Loading read the (missing) theme file.
    CHECK_EQ(loaded.themeError, "cannot read " + settings.themeFile);

    // Hand edits out of range are clamped, unknown indices and junk get the defaults.
    rgui::ui::SettingsFile edited;
    edited.SetInt("export.style", 7);
    edited.SetInt("export.margin", -1);
    edited.SetDouble("export.customMarginInches", 9);
    edited.SetInt("export.pageSize", 6);
    edited.SetDouble("export.customWidthMm", 1);
    edited.SetDouble("export.customHeightMm", 99999);
    edited.SetInt("export.tocDepth", 0);
    edited.SetInt("export.numberDepth", 9);
    edited.SetInt("export.pageBreakBefore", 4);
    edited.SetInt("export.dialect", -1);
    edited.SetString("export.highlightCode", "maybe");
    loaded.Load(edited);
    CHECK_EQ(loaded.style, ExportSettings::kElegant);
    CHECK_EQ(loaded.margin, ExportSettings::kNormal);
    CHECK_EQ(loaded.customMarginInches, 2.0);
    CHECK_EQ(loaded.pageSize, ExportSettings::kA4);
    CHECK_EQ(loaded.customWidthMm, 50.8);
    CHECK_EQ(loaded.customHeightMm, 5080.0);
    CHECK_EQ(loaded.tocDepth, 1);
    CHECK_EQ(loaded.numberDepth, 6);
    CHECK_EQ(loaded.pageBreakBefore, ExportSettings::kNoPageBreak);
    CHECK_EQ(loaded.dialect, ExportSettings::kGitHub);
    CHECK_EQ(loaded.highlightCode, true);
    CHECK_EQ(loaded.landscape, false);
    CHECK(loaded.themeFile.empty());
    CHECK(loaded.themeError.empty());
}

// ---------------------------------------------------------------------------------------------
// Document

RGUI_TEST("app.document.crlf") {
    const fs::path dir = TempDir("crlf");
    CHECK(RayoMd::File::WriteAll(dir / "win.md", "# Title\r\n\r\nText\r\n"));
    Document doc;
    std::string status;
    CHECK(doc.Open(Utf8(dir / "win.md"), status));
    CHECK(status.empty());
    CHECK_EQ(doc.text, std::string("# Title\n\nText\n"));
    CHECK_EQ(doc.crlf, true);
    CHECK_EQ(doc.filePath, Utf8(dir / "win.md"));
    CHECK_EQ(doc.displayName, std::string("win.md"));
    CHECK_EQ(doc.modified, false);

    doc.text += "More\n";
    doc.modified = true;
    CHECK(doc.Save(Utf8(dir / "copy.md"), status));
    CHECK_EQ(status, std::string("Saved copy.md"));
    CHECK_EQ(ReadFile(dir / "copy.md"), std::string("# Title\r\n\r\nText\r\nMore\r\n"));
    CHECK(!fs::exists(dir / "copy.md.tmp"));
    CHECK_EQ(doc.filePath, Utf8(dir / "copy.md"));
    CHECK_EQ(doc.displayName, std::string("copy.md"));
    CHECK_EQ(doc.modified, false);

    // A CR run before LF never survives into the text, so it stays LF-only; a lone CR does.
    CHECK(RayoMd::File::WriteAll(dir / "odd.md", "a\r\r\nb\rc"));
    CHECK(doc.Open(Utf8(dir / "odd.md"), status));
    CHECK_EQ(doc.text, std::string("a\nb\rc"));
    CHECK_EQ(doc.crlf, true);

    // A run of lone CRs is read once: read again from each of its CRs, these 256 KiB were 3 * 10^10
    // steps on the loader thread.
    const std::string crs(size_t(256) << 10, '\r');
    CHECK(RayoMd::File::WriteAll(dir / "crs.md", crs + "x\r\r\ny"));
    const auto opening = std::chrono::steady_clock::now();
    CHECK(doc.Open(Utf8(dir / "crs.md"), status));
    CHECK(std::chrono::steady_clock::now() - opening < std::chrono::seconds(5));
    CHECK_EQ(doc.text, crs + "x\ny");

    // LF files stay LF, and saving over the file itself works.
    CHECK(RayoMd::File::WriteAll(dir / "unix.md", "one\ntwo\n"));
    CHECK(doc.Open(Utf8(dir / "unix.md"), status));
    CHECK_EQ(doc.crlf, false);
    doc.text = "one\ntwo\nthree\n";
    CHECK(doc.Save(doc.filePath, status));
    CHECK_EQ(ReadFile(dir / "unix.md"), std::string("one\ntwo\nthree\n"));

    // A save that cannot be written leaves the document as it was.
    CHECK(!doc.Save(Utf8(dir / "no-such-dir" / "x.md"), status));
    CHECK_EQ(status, std::string("Cannot save x.md"));
    CHECK_EQ(doc.filePath, Utf8(dir / "unix.md"));
    CHECK(!fs::exists(dir / "no-such-dir"));
}

RGUI_TEST("app.document.refuse") {
    const fs::path dir = TempDir("refuse");
    CHECK(RayoMd::File::WriteAll(dir / "good.md", "kept\n"));
    CHECK(RayoMd::File::WriteAll(dir / "bad.md", std::string("abc\xff\xfe def", 9)));
    Document doc;
    std::string status;
    CHECK(doc.Open(Utf8(dir / "good.md"), status));
    doc.text = "kept, edited\n";
    doc.modified = true;

    CHECK(!doc.Open(Utf8(dir / "bad.md"), status));
    CHECK_EQ(status, std::string("Not a UTF-8 text file: bad.md"));
    CHECK_EQ(doc.text, std::string("kept, edited\n"));
    CHECK_EQ(doc.filePath, Utf8(dir / "good.md"));
    CHECK_EQ(doc.modified, true);

    CHECK(!doc.Open(Utf8(dir / "missing.md"), status));
    CHECK_EQ(status, std::string("Cannot read missing.md"));
    CHECK_EQ(doc.text, std::string("kept, edited\n"));

    // A PDF without an embedded source is refused with the engine's reason.
    std::string pdf;
    CHECK(TinyPdf::BuildPdf("# Plain\n", TinyPdf::PdfOptions(), pdf).Ok());
    CHECK(RayoMd::File::WriteAll(dir / "plain.pdf", pdf));
    CHECK(!doc.Open(Utf8(dir / "plain.pdf"), status));
    CHECK_EQ(status, std::string(RayoMd::PdfSource::StatusMessage(RayoMd::PdfSource::Status::NotReversible)));
    CHECK_EQ(doc.text, std::string("kept, edited\n"));
    CHECK_EQ(doc.filePath, Utf8(dir / "good.md"));
}

RGUI_TEST("app.document.recover") {
    const fs::path dir = TempDir("recover");
    const std::string source = "---\ntitle: Kept\n---\n\n# Zażółć\n\n<!-- hidden -->\nText with `code`.\n";
    TinyPdf::PdfOptions options;
    options.embedSource = true;
    std::string pdf;
    CHECK(TinyPdf::BuildPdf(source, options, pdf).Ok());
    CHECK(RayoMd::File::WriteAll(dir / "Report.PDF", pdf));

    Document doc;
    std::string status;
    CHECK(doc.Open(Utf8(dir / "Report.PDF"), status));
    CHECK_EQ(doc.text, source);
    CHECK(doc.filePath.empty());
    CHECK_EQ(doc.displayName, std::string("Report.md"));
    CHECK_EQ(doc.modified, false);
    CHECK_EQ(doc.crlf, false);
    CHECK_EQ(status, "Recovered " + std::to_string(source.size()) +
                         " validated bytes (SHA-256 valid) from Report.PDF. Ctrl+S saves the Markdown.");

    // A damaged reversible PDF is refused, the recovered document kept.
    std::string damaged = pdf;
    size_t changed = 0;
    for (size_t at = damaged.find("hidden"); at != std::string::npos; at = damaged.find("hidden", at)) {
        damaged[at] = 'H';
        changed++;
    }
    CHECK(changed > 0);
    CHECK(RayoMd::File::WriteAll(dir / "damaged.pdf", damaged));
    CHECK(!doc.Open(Utf8(dir / "damaged.pdf"), status));
    CHECK(!status.empty());
    CHECK_EQ(doc.text, source);
    CHECK_EQ(doc.displayName, std::string("Report.md"));
}

RGUI_TEST("app.document.stats") {
    CHECK_EQ(Document::ComputeStats("").lines, size_t(1));
    CHECK_EQ(Document::ComputeStats("").words, size_t(0));
    CHECK_EQ(Document::ComputeStats("Hello  world\n\tfoo\r\nbar").lines, size_t(3));
    CHECK_EQ(Document::ComputeStats("Hello  world\n\tfoo\r\nbar").words, size_t(4));
    CHECK_EQ(Document::ComputeStats("a\n\n").lines, size_t(3));
    CHECK_EQ(Document::ComputeStats("a\n\n").words, size_t(1));
    // Only space, tab, LF and CR separate words: punctuation and non-ASCII letters do not.
    CHECK_EQ(Document::ComputeStats("zażółć gęślą-jaźń, 中文").words, size_t(3));
    Document doc;
    doc.New();
    CHECK_EQ(doc.displayName, std::string("untitled.md"));
    doc.text = "one two";
    CHECK_EQ(doc.Stats().words, size_t(2));
}

// ---------------------------------------------------------------------------------------------
// Arguments

RGUI_TEST("app.args") {
    using Mode = rgui::Args::Mode;
    rgui::Args args = rgui::ParseArgs({});
    CHECK(args.mode == Mode::Run);
    CHECK(args.file.empty());
    args = rgui::ParseArgs({"notes.md"});
    CHECK(args.mode == Mode::Run);
    CHECK_EQ(args.file, std::string("notes.md"));
    CHECK(rgui::ParseArgs({"--version"}).mode == Mode::Version);
    args = rgui::ParseArgs({"--snapshot", "out.png", "doc.md"});
    CHECK(args.mode == Mode::Snapshot);
    CHECK_EQ(args.snapshotPath, std::string("out.png"));
    CHECK_EQ(args.file, std::string("doc.md"));
    args = rgui::ParseArgs({"--snapshot", "out.png"});
    CHECK(args.mode == Mode::Snapshot);
    CHECK(args.file.empty());

    const std::vector<std::vector<std::string>> bad = {
        {"--help"}, {"-x"}, {"--version", "a.md"}, {"a.md", "--version"}, {"--snapshot"},
        {"--snapshot", "--version"}, {"a.md", "b.md"}, {"--snapshot", "o.png", "a.md", "b.md"},
        {"a.md", "--snapshot", "o.png"}, {""},
    };
    for (const auto& arguments : bad) CHECK(rgui::ParseArgs(arguments).mode == Mode::Usage);
    CHECK_EQ(std::string(rgui::Args::kUsage), std::string("Usage: rayomd-gui [--version | --snapshot <out.png>] [file]"));
    CHECK_EQ(rgui::Args::kUsageExitCode, 2);
}

// ---------------------------------------------------------------------------------------------
// The headless platform the app tests run on: its fake clock must order timers exactly.

RGUI_TEST("app.headless.timers") {
    rgui::test::HeadlessPlatform platform;
    std::string fired;
    platform.StartTimer(30, [&] { fired += "c"; });
    const uint64_t cancelled = platform.StartTimer(10, [&] { fired += "x"; });
    platform.StartTimer(20, [&] { fired += "a"; });
    platform.StartTimer(20, [&] {
        fired += "b";
        // Started at t = 20 for 0 ms: due at once, within the same Advance.
        platform.StartTimer(0, [&] { fired += "d" + std::to_string(platform.Now()); });
        platform.PostToUi([&] { fired += "p"; });
    });
    platform.CancelTimer(cancelled);
    platform.CancelTimer(cancelled);  // twice is harmless
    CHECK_EQ(platform.Pump(), 0);      // nothing is due at t = 0
    CHECK_EQ(platform.Advance(25), 4);
    CHECK_EQ(fired, std::string("abpd20"));
    CHECK_EQ(platform.Now(), int64_t(25));
    CHECK_EQ(platform.PendingTimers(), size_t(1));
    platform.Advance(5);
    CHECK_EQ(fired, std::string("abpd20c"));
    CHECK_EQ(platform.PendingTimers(), size_t(0));

    // A worker thread's post wakes PumpUntil.
    bool done = false;
    std::thread poster([&platform, &done] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        platform.PostToUi([&done] { done = true; });
    });
    CHECK(platform.PumpUntil([&] { return done; }, 5000));
    poster.join();
    CHECK(!platform.PumpUntil([] { return false; }, 10));

    // Scripted dialogs answer in order, then cancel.
    platform.ScriptDialog({rgui::FileDialogResult::Chosen, "/tmp/a.md"});
    std::vector<rgui::FileDialogResult> answers;
    platform.ShowFileDialog(rgui::FileDialog::OpenDocument, "/docs/x.md",
                            [&](rgui::FileDialogResult r) { answers.push_back(std::move(r)); });
    platform.ShowFileDialog(rgui::FileDialog::ExportPdf, "",
                            [&](rgui::FileDialogResult r) { answers.push_back(std::move(r)); });
    CHECK(answers.empty());  // callbacks run on the next pump, as on a real platform
    platform.Pump();
    CHECK_EQ(answers.size(), size_t(2));
    if (answers.size() == 2) {
        CHECK(answers[0].kind == rgui::FileDialogResult::Chosen);
        CHECK_EQ(answers[0].path, std::string("/tmp/a.md"));
        CHECK(answers[1].kind == rgui::FileDialogResult::Cancelled);
    }
    CHECK_EQ(platform.dialogs.size(), size_t(2));
    if (!platform.dialogs.empty()) CHECK_EQ(platform.dialogs[0].suggestedPath, std::string("/docs/x.md"));
}

// ---------------------------------------------------------------------------------------------
// Build worker

// A build function that records what it builds and, while closed, holds the build that comes in;
// a build cancelled meanwhile then ends as the engine's would.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = true;
    int waiting = 0;
    std::vector<std::string> built;

    rgui::BuildWorker::BuildFunction Function() {
        return [this](const std::string& md, const TinyPdf::PdfOptions& options, std::string& out) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                built.push_back(md);
                waiting++;
                changed.notify_all();
                changed.wait(lock, [this] { return open; });
                waiting--;
            }
            if (options.cancel != nullptr && options.cancel->load()) {
                TinyPdf::BuildResult cancelled;
                cancelled.error = TinyPdf::BuildError::Cancelled;
                return cancelled;
            }
            return TinyPdf::BuildPdf(md, options, out);
        };
    }
    void Close() {
        std::lock_guard<std::mutex> lock(mutex);
        open = false;
    }
    void Open() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
    void WaitForBuild() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] { return waiting > 0; });
    }
};

RGUI_TEST("app.buildworker.order") {
    const fs::path dir = TempDir("order");
    rgui::test::HeadlessPlatform platform;
    Gate gate;
    std::vector<rgui::PreviewReady> previews;
    std::vector<rgui::ExportFinished> exports;
    {
        rgui::BuildWorker worker(
            platform, [&](rgui::PreviewReady ready) { previews.push_back(std::move(ready)); },
            [&](rgui::ExportFinished finished) { exports.push_back(std::move(finished)); }, gate.Function());
        gate.Close();
        worker.RequestPreview("# p1\n", "", TinyPdf::PdfOptions(), 1);
        gate.WaitForBuild();
        // While p1 builds: newer previews replace each other, exports queue up before them, and
        // the first export cancels p1 (p4, newer, replaces it rather than p1 building again).
        worker.RequestPreview("# p2\n", "", TinyPdf::PdfOptions(), 2);
        worker.RequestPreview("# p3\n", "", TinyPdf::PdfOptions(), 3);
        worker.RequestExport("# e1\n", "", TinyPdf::PdfOptions(), Utf8(dir / "e1.pdf"), false);
        worker.RequestPreview("# p4\n", "", TinyPdf::PdfOptions(), 4);
        worker.RequestExport("# e2\n", "", TinyPdf::PdfOptions(), Utf8(dir / "e2.pdf"), true);
        gate.Open();
        CHECK(platform.PumpUntil([&] { return previews.size() == 1 && exports.size() == 2; }, 20000));
    }
    const std::vector<std::string> order = {"# p1\n", "# e1\n", "# e2\n"};
    CHECK_EQ(gate.built.size(), size_t(4));
    for (size_t i = 0; i < order.size() && i < gate.built.size(); i++) CHECK_EQ(gate.built[i], order[i]);
    if (gate.built.size() == 4) CHECK_EQ(gate.built[3], std::string("# p4\n"));

    CHECK_EQ(previews.size(), size_t(1));
    if (previews.size() == 1) {
        const rgui::PreviewReady& ready = previews[0];
        CHECK_EQ(ready.generation, 4);
        CHECK(ready.error.empty());
        CHECK(ready.pdf != nullptr);
        if (ready.pdf) CHECK_EQ(static_cast<uint32_t>(ready.pdf->PageCount()), ready.pages);
        CHECK(ready.pages >= 1);
    }
    CHECK_EQ(exports.size(), size_t(2));
    if (exports.size() == 2) {
        CHECK_EQ(exports[0].path, Utf8(dir / "e1.pdf"));
        CHECK_EQ(exports[1].path, Utf8(dir / "e2.pdf"));
        for (const rgui::ExportFinished& finished : exports) {
            CHECK(finished.ok);
            CHECK_EQ(finished.pages, 1u);
            CHECK(StartsWith(ReadFile(fs::u8path(finished.path)), "%PDF-"));
            CHECK(!fs::exists(fs::u8path(finished.path + ".tmp")));
        }
        CHECK(StartsWith(exports[0].message, "Exported e1.pdf (1 page, "));
    }
    // Only the export that asked for it is opened.
    CHECK_EQ(platform.openedExternally.size(), size_t(1));
    if (platform.openedExternally.size() == 1) CHECK_EQ(platform.openedExternally[0], Utf8(dir / "e2.pdf"));
}

RGUI_TEST("app.buildworker.errors") {
    const fs::path dir = TempDir("errors");
    struct Case {
        TinyPdf::BuildError error;
        const char* text;
    };
    const Case cases[] = {
        {TinyPdf::BuildError::SourceTooLarge, "the document is larger than the embedded-source limit (10 MiB)"},
        {TinyPdf::BuildError::InvalidSourceUtf8, "the document is not valid UTF-8"},
        {TinyPdf::BuildError::ReversiblePdfTooLarge, "the PDF would pass the reversible-profile limit (256 MiB)"},
        {TinyPdf::BuildError::ThemeFontUnavailable, "the PDF theme's font cannot be read"},
        {TinyPdf::BuildError::PdfaFontUnavailable, "PDF/A and PDF/UA need a TrueType or OpenType font; none was found"},
        {TinyPdf::BuildError::FontUnavailable, "no font is available"},
    };
    rgui::test::HeadlessPlatform platform;
    TinyPdf::BuildError next = TinyPdf::BuildError::None;
    std::vector<rgui::PreviewReady> previews;
    std::vector<rgui::ExportFinished> exports;
    rgui::BuildWorker worker(
        platform, [&](rgui::PreviewReady ready) { previews.push_back(std::move(ready)); },
        [&](rgui::ExportFinished finished) { exports.push_back(std::move(finished)); },
        [&next](const std::string&, const TinyPdf::PdfOptions&, std::string& out) {
            TinyPdf::BuildResult result;
            result.error = next;
            out = "not a PDF";
            return result;
        });
    int generation = 0;
    for (const Case& c : cases) {
        CHECK_EQ(std::string(rgui::BuildErrorText(c.error)), std::string(c.text));
        next = c.error;  // read by the worker only after the request below
        previews.clear();
        exports.clear();
        worker.RequestPreview("x", "", TinyPdf::PdfOptions(), ++generation);
        CHECK(platform.PumpUntil([&] { return !previews.empty(); }, 10000));
        worker.RequestExport("x", "", TinyPdf::PdfOptions(), Utf8(dir / "out.pdf"), true);
        CHECK(platform.PumpUntil([&] { return !exports.empty(); }, 10000));
        if (previews.empty() || exports.empty()) break;
        CHECK(previews[0].buildError == c.error);
        CHECK_EQ(previews[0].error, std::string(c.text));
        CHECK(previews[0].pdf == nullptr);
        CHECK_EQ(exports[0].ok, false);
        CHECK_EQ(exports[0].message, "Export failed: " + std::string(c.text));
        CHECK(!fs::exists(dir / "out.pdf"));
        CHECK(!fs::exists(dir / "out.pdf.tmp"));
    }
    CHECK(platform.openedExternally.empty());

    // Bytes the preview reader cannot read.
    next = TinyPdf::BuildError::None;
    previews.clear();
    worker.RequestPreview("x", "", TinyPdf::PdfOptions(), ++generation);
    CHECK(platform.PumpUntil([&] { return !previews.empty(); }, 10000));
    if (!previews.empty()) {
        CHECK(previews[0].pdf == nullptr);
        CHECK(StartsWith(previews[0].error, "the preview PDF cannot be read ("));
    }
}

RGUI_TEST("app.buildworker.engine_errors") {
    const fs::path dir = TempDir("engine-errors");
    rgui::test::HeadlessPlatform platform;
    std::vector<rgui::ExportFinished> exports;
    rgui::BuildWorker worker(platform, nullptr,
                             [&](rgui::ExportFinished finished) { exports.push_back(std::move(finished)); });

    // The real engine: an embedded source must be UTF-8; a theme font must exist; the output
    // must be writable.
    TinyPdf::PdfOptions options;
    options.embedSource = true;
    worker.RequestExport("bad \xff byte", "", options, Utf8(dir / "a.pdf"), false);
    TinyPdf::PdfOptions themed;
    themed.theme.fontPath = Utf8(dir / "missing.ttf");
    worker.RequestExport("# Themed\n", "", themed, Utf8(dir / "b.pdf"), false);
    worker.RequestExport("# Nowhere\n", "", TinyPdf::PdfOptions(), Utf8(dir / "no-dir" / "c.pdf"), true);
    CHECK(platform.PumpUntil([&] { return exports.size() == 3; }, 20000));
    if (exports.size() == 3) {
        CHECK_EQ(exports[0].message, std::string("Export failed: the document is not valid UTF-8"));
        CHECK_EQ(exports[1].message, std::string("Export failed: the PDF theme's font cannot be read"));
        CHECK_EQ(exports[2].message, "Export failed: cannot write " + Utf8(dir / "no-dir" / "c.pdf"));
        for (const auto& finished : exports) CHECK(!finished.ok);
    }
    CHECK(platform.openedExternally.empty());
    CHECK(!fs::exists(dir / "no-dir"));
}

RGUI_TEST("app.buildworker.shutdown") {
    rgui::test::HeadlessPlatform platform;
    Gate gate;
    int callbacks = 0;
    std::thread opener;
    {
        rgui::BuildWorker worker(
            platform, [&](rgui::PreviewReady) { callbacks++; }, [&](rgui::ExportFinished) { callbacks++; },
            gate.Function());
        gate.Close();
        worker.RequestPreview("# p1\n", "", TinyPdf::PdfOptions(), 1);
        gate.WaitForBuild();
        worker.RequestPreview("# p2\n", "", TinyPdf::PdfOptions(), 2);
        worker.RequestExport("# e1\n", "", TinyPdf::PdfOptions(), Utf8(TempDir("shutdown") / "e.pdf"), true);
        // The destructor drops the pending requests and waits for the running build, which
        // the gate lets finish only while the destructor waits.
        opener = std::thread([&gate] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            gate.Open();
        });
    }
    opener.join();
    // p1's result may have been posted before the worker went away: it is never delivered.
    platform.Pump();
    CHECK_EQ(callbacks, 0);
    CHECK_EQ(gate.built.size(), size_t(1));
    CHECK(platform.openedExternally.empty());
}

RGUI_TEST("app.buildworker.cancel") {
    const fs::path dir = TempDir("cancel");
    rgui::test::HeadlessPlatform platform;
    rgui::test::CancellableBuild engine;
    std::vector<rgui::PreviewReady> previews;
    std::vector<rgui::ExportFinished> exports;
    std::string order;
    auto worker = std::make_unique<rgui::BuildWorker>(
        platform,
        [&](rgui::PreviewReady ready) {
            order += "p" + std::to_string(ready.generation);
            previews.push_back(std::move(ready));
        },
        [&](rgui::ExportFinished finished) {
            order += "e";
            exports.push_back(std::move(finished));
        },
        engine.Function());

    // The preview being built stops; one still waiting is dropped; both are answered.
    engine.shots = 1;
    worker->RequestPreview("# p1\n", "", TinyPdf::PdfOptions(), 1);
    CHECK(engine.WaitBlocked());
    worker->CancelPreview();
    CHECK(platform.PumpUntil([&] { return previews.size() == 1; }, 10000));
    engine.hold = true;
    worker->RequestExport("# e1\n", "", TinyPdf::PdfOptions(), Utf8(dir / "e1.pdf"), false);
    worker->RequestPreview("# p2\n", "", TinyPdf::PdfOptions(), 2);
    worker->CancelPreview();
    CHECK(platform.PumpUntil([&] { return previews.size() == 2; }, 10000));
    engine.hold = false;
    CHECK(platform.PumpUntil([&] { return exports.size() == 1; }, 10000));
    if (previews.size() != 2 || exports.size() != 1) return;
    for (const rgui::PreviewReady& ready : previews) {
        CHECK(ready.buildError == TinyPdf::BuildError::Cancelled);
        CHECK(ready.pdf == nullptr);
        CHECK(ready.error.empty());
    }
    CHECK(exports[0].ok);   // CancelPreview leaves exports alone

    // An export stops the preview being built, which is built again after it.
    engine.shots = 1;
    worker->RequestPreview("# p3\n", "", TinyPdf::PdfOptions(), 3);
    CHECK(engine.WaitBlocked());
    worker->RequestExport("# e2\n", "", TinyPdf::PdfOptions(), Utf8(dir / "e2.pdf"), false);
    CHECK(platform.PumpUntil([&] { return previews.size() == 3 && exports.size() == 2; }, 20000));
    CHECK_EQ(order, std::string("p1p2eep3"));
    if (previews.size() == 3) {
        CHECK(previews[2].buildError == TinyPdf::BuildError::None);
        CHECK(previews[2].pdf != nullptr);
        CHECK_EQ(previews[2].stats.words, size_t(2));
    }

    // Quitting: a build that ends only when cancelled does not hold the destructor up.
    engine.shots = 1;
    worker->RequestPreview("# p4\n", "", TinyPdf::PdfOptions(), 4);
    CHECK(engine.WaitBlocked());
    const auto start = std::chrono::steady_clock::now();
    worker.reset();
    const double quitMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    CHECK(quitMs < 1000);
    platform.Pump();
    CHECK_EQ(previews.size(), size_t(3));
    CHECK_EQ(engine.cancelled.load(), 3);   // p1, p3 (built again) and p4; p2 never started
}

// The worker reads the PDF theme file, again only when its size or time changed.
RGUI_TEST("app.buildworker.theme") {
    const fs::path dir = TempDir("worker-theme");
    rgui::test::HeadlessPlatform platform;
    std::vector<int32_t> headings;   // options.theme.headingColor per build (worker thread)
    std::mutex mutex;
    std::vector<rgui::PreviewReady> previews;
    std::vector<rgui::ExportFinished> exports;
    rgui::BuildWorker worker(
        platform, [&](rgui::PreviewReady ready) { previews.push_back(std::move(ready)); },
        [&](rgui::ExportFinished finished) { exports.push_back(std::move(finished)); },
        [&](const std::string& md, const TinyPdf::PdfOptions& options, std::string& out) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                headings.push_back(options.theme.headingColor);
            }
            return TinyPdf::BuildPdf(md, options, out);
        });
    const fs::path theme = dir / "a.theme";
    const std::string themeFile = Utf8(theme);
    auto preview = [&](int generation, const std::string& file) {
        previews.clear();
        worker.RequestPreview("# T\n", "", TinyPdf::PdfOptions(), generation, file);
        CHECK(platform.PumpUntil([&] { return !previews.empty(); }, 10000));
        std::lock_guard<std::mutex> lock(mutex);
        return headings.empty() ? 0 : headings.back();
    };

    CHECK(RayoMd::File::WriteAll(theme, "heading-color = #112233\n"));
    CHECK_EQ(preview(1, themeFile), 0x112233);
    if (previews.empty()) return;
    CHECK_EQ(previews[0].themeFile, themeFile);
    CHECK(previews[0].themeError.empty());

    // Changed (same size, a later time): read again.
    const fs::file_time_type first = fs::last_write_time(theme);
    CHECK(RayoMd::File::WriteAll(theme, "heading-color = #445566\n"));
    fs::last_write_time(theme, first + std::chrono::seconds(2));
    CHECK_EQ(preview(2, themeFile), 0x445566);
    // Same size and time: not read again.
    CHECK(RayoMd::File::WriteAll(theme, "heading-color = #778899\n"));
    fs::last_write_time(theme, first + std::chrono::seconds(2));
    CHECK_EQ(preview(3, themeFile), 0x445566);

    // A bad file: the default theme, and the error.
    CHECK(RayoMd::File::WriteAll(theme, "wobble = 3\n"));
    CHECK_EQ(preview(4, themeFile), TinyPdf::PdfTheme::kStyleColor);
    if (!previews.empty()) CHECK(previews[0].themeError.find("wobble") != std::string::npos);
    const std::string missing = Utf8(dir / "missing.theme");
    CHECK_EQ(preview(5, missing), TinyPdf::PdfTheme::kStyleColor);
    if (!previews.empty()) CHECK_EQ(previews[0].themeError, "cannot read " + missing);

    // Exports too.
    CHECK(RayoMd::File::WriteAll(theme, "heading-color = #0a0b0c\n"));
    worker.RequestExport("# T\n", "", TinyPdf::PdfOptions(), Utf8(dir / "t.pdf"), false, missing);
    CHECK(platform.PumpUntil([&] { return !exports.empty(); }, 10000));
    if (!exports.empty()) {
        CHECK(exports[0].ok);
        CHECK_EQ(exports[0].themeFile, missing);
        CHECK_EQ(exports[0].themeError, "cannot read " + missing);
    }

    // Without a theme file, the options' theme is used as given.
    TinyPdf::PdfOptions given;
    given.theme.headingColor = 0x010203;
    previews.clear();
    worker.RequestPreview("# T\n", "", given, 6);
    CHECK(platform.PumpUntil([&] { return !previews.empty(); }, 10000));
    std::lock_guard<std::mutex> lock(mutex);
    if (!headings.empty()) CHECK_EQ(headings.back(), 0x010203);
}

// ---------------------------------------------------------------------------------------------
// Export through the worker, as the app does: these bytes must equal the CLI's
// (`rayomd --export tester.md --no-image-cache`, with the same RAYOMD_FONT).

RGUI_TEST("app.export_bytes") {
    rgui::test::HeadlessPlatform platform;
    Document doc;
    std::string status;
    const std::string tester = Utf8(fs::u8path(rgui::test::SourceDir()) / "external" / "rayomd" / "tester.md");
    CHECK(doc.Open(tester, status));
    ExportSettings settings;
    settings.imageCacheDirectory = Utf8(platform.ImageCacheDir());
    settings.ReloadTheme();
    const std::string output = Utf8(fs::u8path(rgui::test::BuildDir()) / "gui.pdf");

    std::vector<rgui::ExportFinished> exports;
    rgui::BuildWorker worker(platform, nullptr,
                             [&](rgui::ExportFinished finished) { exports.push_back(std::move(finished)); });
    worker.RequestExport(doc.text, doc.filePath, settings.ToPdfOptions(doc.filePath, false), output,
                         settings.openAfterExport);
    CHECK(platform.PumpUntil([&] { return !exports.empty(); }, 60000));
    if (exports.empty()) return;
    CHECK(exports[0].ok);
    CHECK(StartsWith(exports[0].message, "Exported gui.pdf ("));
    CHECK(exports[0].pages > 1);
    CHECK(StartsWith(ReadFile(fs::u8path(output)), "%PDF-"));
    CHECK_EQ(platform.openedExternally.size(), size_t(1));
    std::printf("  %s\n", exports[0].message.c_str());
}

} // namespace
