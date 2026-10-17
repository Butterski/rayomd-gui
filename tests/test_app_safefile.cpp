// SPDX-License-Identifier: Apache-2.0
// Safe file I/O (app/safefile.h) and its users: documents, exports, settings, theme files. A
// shared folder may hold files of another user planted where the app writes, or FIFOs where it
// reads; none of them may redirect a write, take over a file's permissions or hang the app.
#include "headless_platform.h"
#include "runner.h"

#include "app/buildworker.h"
#include "app/document.h"
#include "app/documentloader.h"
#include "app/exportsettings.h"
#include "app/safefile.h"
#include "common/file_io.h"
#include "ui/settingsfile.h"
#include "ui/theme.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using rgui::WriteMode;

fs::path TempDir(const char* name) {
    const fs::path dir = fs::u8path(rgui::test::BuildDir()) / "test-safefile" / name;
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    return dir;
}

std::string ReadFile(const fs::path& path) {
    std::string content;
    if (!RayoMd::File::ReadAll(path, 1u << 30, content)) return "<unreadable>";
    return content;
}

// The names in `dir`, for "nothing else was left behind".
std::vector<std::string> Entries(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(dir, error)) names.push_back(entry.path().filename().u8string());
    return names;
}

size_t TempFilesIn(const fs::path& dir) {
    size_t count = 0;
    for (const std::string& name : Entries(dir)) {
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0) count++;
    }
    return count;
}

#ifndef _WIN32

mode_t ModeOf(const fs::path& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 ? (info.st_mode & 07777) : 0;
}

bool IsSymlink(const fs::path& path) {
    std::error_code error;
    return fs::is_symlink(fs::symlink_status(path, error));
}

// Runs `body` on a thread; true when it returns within `ms`. Otherwise the FIFOs are opened
// from this side (O_RDWR never blocks on Linux) to release a reader or writer stuck in open,
// so a regression fails the test instead of hanging the run.
bool FinishesWithin(int ms, const std::vector<fs::path>& fifos, const std::function<void()>& body) {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::thread thread([&] {
        body();
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        done.notify_all();
    });
    bool inTime = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        inTime = done.wait_for(lock, std::chrono::milliseconds(ms), [&] { return finished; });
    }
    while (!inTime) {
        for (const fs::path& fifo : fifos) {
            const int fd = open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                (void)!write(fd, "x", 1);
                close(fd);
            }
        }
        std::unique_lock<std::mutex> lock(mutex);
        if (done.wait_for(lock, std::chrono::milliseconds(100), [&] { return finished; })) break;
    }
    thread.join();
    return inTime;
}

#endif

} // namespace

RGUI_TEST("app.safefile.round_trip") {
    const fs::path dir = TempDir("round-trip");
    std::string error;
    for (const WriteMode mode : {WriteMode::Document, WriteMode::Output}) {
        CHECK(rgui::WriteFileAtomically(dir / "a.md", "first\n", mode, &error));
        CHECK(rgui::WriteFileAtomically(dir / "a.md", "second, longer\n", mode, &error));
        std::string read;
        CHECK(rgui::ReadRegularFile(dir / "a.md", 1 << 20, read, &error));
        CHECK_EQ(read, std::string("second, longer\n"));
        CHECK(rgui::WriteFileAtomically(dir / "empty.md", "", mode, &error));
        CHECK(rgui::ReadRegularFile(dir / "empty.md", 0, read, &error));
        CHECK(read.empty());
    }
    // A name near the 255-byte limit still finds a temporary name, and on Windows a path past
    // MAX_PATH (this one is) is written and read through LongPath's `\\?\` form.
    const std::string longName = std::string(250, 'n') + ".md";
    CHECK(rgui::WriteFileAtomically(dir / longName, "long", WriteMode::Output, &error));
    std::string longRead;
    CHECK(rgui::ReadRegularFile(dir / longName, 64, longRead, &error));
    CHECK_EQ(longRead, std::string("long"));
    CHECK_EQ(TempFilesIn(dir), size_t(0));

    // The size limit: exactly `limit` bytes read, one more refused with `out` untouched.
    std::string out = "kept";
    CHECK(rgui::ReadRegularFile(dir / "a.md", 15, out, &error));
    out = "kept";
    CHECK(!rgui::ReadRegularFile(dir / "a.md", 14, out, &error));
    CHECK_EQ(error, std::string("too large"));
    CHECK_EQ(out, std::string("kept"));
    CHECK(!rgui::ReadRegularFile(dir / "missing.md", 100, out, &error));
    // A folder is neither read nor written over.
    fs::create_directories(dir / "folder.md");
    CHECK(!rgui::ReadRegularFile(dir / "folder.md", 100, out, &error));
    CHECK(!rgui::WriteFileAtomically(dir / "folder.md", "x", WriteMode::Document, &error));
    CHECK(fs::is_directory(dir / "folder.md"));
    // A missing folder fails without creating anything.
    CHECK(!rgui::WriteFileAtomically(dir / "no-dir" / "x.md", "x", WriteMode::Output, &error));
    CHECK(!fs::exists(dir / "no-dir"));
    CHECK_EQ(TempFilesIn(dir), size_t(0));
}

RGUI_TEST("app.safefile.new_file") {
    // NewFile writes a name that is free and never replaces what is there: a file, a folder or
    // (below) a symlink, dangling or not.
    const fs::path dir = TempDir("new-file");
    std::string error;
    CHECK(rgui::WriteFileAtomically(dir / "a.png", "first", WriteMode::NewFile, &error));
    CHECK(!rgui::WriteFileAtomically(dir / "a.png", "second", WriteMode::NewFile, &error));
    CHECK_EQ(error, std::string(rgui::kFileExistsError));
    std::string read;
    CHECK(rgui::ReadRegularFile(dir / "a.png", 64, read, &error));
    CHECK_EQ(read, std::string("first"));
    fs::create_directories(dir / "folder.png");
    CHECK(!rgui::WriteFileAtomically(dir / "folder.png", "x", WriteMode::NewFile, &error));
    CHECK_EQ(error, std::string(rgui::kFileExistsError));
    CHECK(fs::is_directory(dir / "folder.png"));
#ifndef _WIN32
    const fs::path victim = dir / "victim.txt";
    CHECK(RayoMd::File::WriteAll(victim, "victim\n"));
    fs::create_symlink(victim, dir / "link.png");
    fs::create_symlink(dir / "nowhere", dir / "dangling.png");
    CHECK(!rgui::WriteFileAtomically(dir / "link.png", "x", WriteMode::NewFile, &error));
    CHECK(!rgui::WriteFileAtomically(dir / "dangling.png", "x", WriteMode::NewFile, &error));
    CHECK(!fs::exists(dir / "nowhere"));
    CHECK(rgui::ReadRegularFile(victim, 64, read, &error));
    CHECK_EQ(read, std::string("victim\n"));
    CHECK(fs::is_symlink(dir / "link.png"));
#endif
    CHECK_EQ(TempFilesIn(dir), size_t(0));
}

RGUI_TEST("app.safefile.image_copy") {
    // A dropped image goes into images/ beside the document under a safe name, never over a
    // file that is there.
    const fs::path dir = TempDir("image-copy");
    const fs::path document = dir / "doc.md";
    CHECK(RayoMd::File::WriteAll(document, "# Doc\n"));
    fs::create_directories(dir / "elsewhere");
    const fs::path source = dir / "elsewhere" / "My (best) photo.PNG";
    CHECK(RayoMd::File::WriteAll(source, "png one"));
    rgui::ImageCopied copied;
    rgui::CopyImageBeside(source, document, copied);
    CHECK(copied.ok);
    CHECK_EQ(copied.markdown, std::string("![My--best--photo](images/My--best--photo.png)"));
    CHECK_EQ(copied.status, std::string("Copied My (best) photo.PNG into images/ as My--best--photo.png"));
    std::string read;
    CHECK(rgui::ReadRegularFile(dir / "images" / "My--best--photo.png", 64, read, nullptr));
    CHECK_EQ(read, std::string("png one"));
    // Again, with other bytes: the next free name, the first copy kept.
    CHECK(RayoMd::File::WriteAll(source, "png two"));
    rgui::CopyImageBeside(source, document, copied);
    CHECK(copied.ok);
    CHECK_EQ(copied.markdown, std::string("![My--best--photo](images/My--best--photo-2.png)"));
    CHECK(rgui::ReadRegularFile(dir / "images" / "My--best--photo.png", 64, read, nullptr));
    CHECK_EQ(read, std::string("png one"));
    CHECK(rgui::ReadRegularFile(dir / "images" / "My--best--photo-2.png", 64, read, nullptr));
    CHECK_EQ(read, std::string("png two"));
    // An image already in images/ is linked as it is.
    CHECK(RayoMd::File::WriteAll(dir / "images" / "plain.svg", "<svg/>"));
    rgui::CopyImageBeside(dir / "images" / "plain.svg", document, copied);
    CHECK(copied.ok);
    CHECK_EQ(copied.markdown, std::string("![plain](images/plain.svg)"));
    CHECK_EQ(copied.status, std::string("Linked images/plain.svg"));
    CHECK(!fs::exists(dir / "images" / "plain-2.svg"));
    // A hidden name is made visible; a file past the size cap is refused, nothing written.
    CHECK(RayoMd::File::WriteAll(dir / "elsewhere" / ".hidden.jpg", "jpg"));
    rgui::CopyImageBeside(dir / "elsewhere" / ".hidden.jpg", document, copied);
    CHECK_EQ(copied.markdown, std::string("![-hidden](images/-hidden.jpg)"));
    const fs::path big = dir / "elsewhere" / "big.png";
    CHECK(RayoMd::File::WriteAll(big, "x"));
    fs::resize_file(big, rgui::kMaxDroppedImageBytes + 1);
    rgui::CopyImageBeside(big, document, copied);
    CHECK(!copied.ok);
    CHECK_EQ(copied.status, std::string("Cannot copy big.png: too large"));
    CHECK(!fs::exists(dir / "images" / "big.png"));
#ifndef _WIN32
    // A name that is not UTF-8 (Latin-1 `caf\xE9`): the byte becomes `-` in the file's name as in
    // its link, so the link names the file; UTF-8 names stay as they are.
    CHECK(RayoMd::File::WriteAll(dir / "elsewhere" / "caf\xE9.png", "latin"));
    rgui::CopyImageBeside(dir / "elsewhere" / "caf\xE9.png", document, copied);
    CHECK(copied.ok);
    CHECK_EQ(copied.markdown, std::string("![caf-](images/caf-.png)"));
    CHECK_EQ(ReadFile(dir / "images" / "caf-.png"), std::string("latin"));
    CHECK(RayoMd::File::WriteAll(dir / "elsewhere" / "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xC3.png", "utf8"));
    rgui::CopyImageBeside(dir / "elsewhere" / "za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 \xC3.png", document, copied);
    CHECK_EQ(copied.markdown, std::string("![za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87--](images/za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87--.png)"));
    // Already in images/ under a name that is not UTF-8: copied under the safe name, not linked.
    CHECK(RayoMd::File::WriteAll(dir / "images" / "x\xE9.svg", "<svg/>"));
    rgui::CopyImageBeside(dir / "images" / "x\xE9.svg", document, copied);
    CHECK_EQ(copied.markdown, std::string("![x-](images/x-.svg)"));
    CHECK_EQ(ReadFile(dir / "images" / "x-.svg"), std::string("<svg/>"));
    // A link planted at the name is passed over, its target untouched.
    const fs::path victim = dir / "victim.txt";
    CHECK(RayoMd::File::WriteAll(victim, "victim\n"));
    fs::create_symlink(victim, dir / "images" / "shot.png");
    CHECK(RayoMd::File::WriteAll(dir / "elsewhere" / "shot.png", "shot"));
    rgui::CopyImageBeside(dir / "elsewhere" / "shot.png", document, copied);
    CHECK_EQ(copied.markdown, std::string("![shot](images/shot-2.png)"));
    CHECK(rgui::ReadRegularFile(victim, 64, read, nullptr));
    CHECK_EQ(read, std::string("victim\n"));
    // A file where the folder should be.
    const fs::path other = dir / "other";
    fs::create_directories(other);
    CHECK(RayoMd::File::WriteAll(other / "images", "not a folder"));
    rgui::CopyImageBeside(dir / "elsewhere" / "shot.png", other / "doc.md", copied);
    CHECK(!copied.ok);
    CHECK_EQ(copied.status, std::string("Cannot copy shot.png: no images folder beside the document"));
#endif
    CHECK_EQ(TempFilesIn(dir / "images"), size_t(0));
}

#ifndef _WIN32

// A file system without RENAME_NOREPLACE (renameat2 failing with EINVAL, or a kernel without
// renameat2: ENOSYS): NewFile takes a hard link instead, which never replaces a name either. The
// failure is made by a seccomp filter on one thread, which runs the writes.
RGUI_TEST("app.safefile.new_file_link_fallback") {
    const fs::path dir = TempDir("new-file-link");
    for (const int code : {EINVAL, ENOSYS}) {
        const std::string name = code == EINVAL ? "einval.png" : "enosys.png";
        bool filtered = false, blocked = false, first = false, second = false;
        std::string firstError, secondError;
        std::thread thread([&] {
            sock_filter filter[] = {
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
                BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
                BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_renameat2, 0, 1),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (static_cast<uint32_t>(code) & SECCOMP_RET_DATA)),
                BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
            };
            sock_fprog program{static_cast<unsigned short>(sizeof filter / sizeof filter[0]), filter};
            // Both apply to this thread only.
            if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0) {
                return;
            }
            filtered = true;
            blocked = syscall(__NR_renameat2, AT_FDCWD, "/nonexistent-a", AT_FDCWD, "/nonexistent-b", 0) != 0 &&
                      errno == code;
            first = rgui::WriteFileAtomically(dir / name, "first", WriteMode::NewFile, &firstError);
            second = rgui::WriteFileAtomically(dir / name, "second", WriteMode::NewFile, &secondError);
        });
        thread.join();
        if (!filtered) {
            std::printf("skip app.safefile.new_file_link_fallback: no seccomp filter\n");
            return;
        }
        CHECK(blocked);
        CHECK(first);
        CHECK_EQ(firstError, std::string());
        CHECK(!second);
        CHECK_EQ(secondError, std::string(rgui::kFileExistsError));
        CHECK_EQ(ReadFile(dir / name), std::string("first"));
        // The link's temporary name is gone: one name, one link.
        struct stat st {};
        CHECK(stat((dir / name).c_str(), &st) == 0 && st.st_nlink == 1);
    }
    CHECK_EQ(TempFilesIn(dir), size_t(0));
    CHECK_EQ(Entries(dir).size(), size_t(2));
}

RGUI_TEST("app.safefile.planted_names") {
    // Another user's symlinks (and a FIFO) at every name a predictable temporary file could
    // have: none is followed or opened; the victims stay as they were and the links stay links.
    const fs::path dir = TempDir("planted");
    const fs::path victim = dir / "victim.txt";
    CHECK(RayoMd::File::WriteAll(victim, "victim\n"));
    const char* const planted[] = {"doc.md.tmp", ".doc.md.tmp", "doc.md~", "doc.tmp", ".doc.md.swp",
                                   "out.pdf.tmp", ".out.pdf.tmp", "settings.ini.tmp", ".settings.ini.tmp"};
    std::error_code error;
    for (const char* name : planted) fs::create_symlink(victim, dir / name, error);
    CHECK(mkfifo((dir / "doc.md.part").c_str(), 0600) == 0);

    bool finished = FinishesWithin(5000, {dir / "doc.md.part"}, [&] {
        rgui::Document doc;
        std::string status;
        doc.text = "the document\n";
        CHECK(doc.Save((dir / "doc.md").u8string(), status));
        rgui::ui::SettingsFile settings;
        CHECK(settings.SetString("ui.theme", "tokyo-night"));
        CHECK(settings.Save(dir / "settings.ini"));
    });
    CHECK(finished);

    // The export, through the build worker.
    rgui::test::HeadlessPlatform platform;
    std::vector<rgui::ExportFinished> exports;
    rgui::BuildWorker worker(
        platform, nullptr, [&](rgui::ExportFinished done) { exports.push_back(std::move(done)); },
        [](const std::string&, const TinyPdf::PdfOptions&, std::string& out) {
            out = "%PDF-1.7 exported";
            TinyPdf::BuildResult result;
            result.pages = 1;
            return result;
        });
    worker.RequestExport("x", "", TinyPdf::PdfOptions(), (dir / "out.pdf").u8string(), false);
    CHECK(platform.PumpUntil([&] { return !exports.empty(); }, 10000));
    CHECK(!exports.empty() && exports[0].ok);

    CHECK_EQ(ReadFile(dir / "doc.md"), std::string("the document\n"));
    CHECK_EQ(ReadFile(dir / "out.pdf"), std::string("%PDF-1.7 exported"));
    CHECK_EQ(ReadFile(dir / "settings.ini"), std::string("ui.theme=tokyo-night\n"));
    CHECK_EQ(ReadFile(victim), std::string("victim\n"));
    for (const char* name : planted) CHECK(IsSymlink(dir / name));
    // Nothing else appeared: the planted names, the victim, the FIFO and the three files.
    CHECK_EQ(Entries(dir).size(), sizeof planted / sizeof planted[0] + 5);
}

RGUI_TEST("app.safefile.fifo") {
    // A FIFO as a document, a theme file, a settings file or a save target: refused at once.
    const fs::path dir = TempDir("fifo");
    const fs::path fifo = dir / "pipe.md";
    const fs::path pdfFifo = dir / "pipe.pdf";
    const fs::path tomlFifo = dir / "colors.toml";
    for (const fs::path& path : {fifo, pdfFifo, tomlFifo}) CHECK(mkfifo(path.c_str(), 0600) == 0);

    const bool finished = FinishesWithin(5000, {fifo, pdfFifo, tomlFifo}, [&] {
        std::string out, error;
        CHECK(!rgui::ReadRegularFile(fifo, 1 << 20, out, &error));
        CHECK_EQ(error, std::string("not a regular file"));

        rgui::Document doc;
        std::string status;
        CHECK(!doc.Open(fifo.u8string(), status));
        CHECK_EQ(status, std::string("Cannot read pipe.md"));
        CHECK(!doc.Open(pdfFifo.u8string(), status));

        rgui::ui::Toml toml;
        CHECK(!rgui::ui::ReadTomlFile(tomlFifo, toml, false));
        rgui::ui::SettingsFile settings;
        CHECK(!settings.Load(fifo));

        rgui::ExportSettings exportSettings;
        exportSettings.themeFile = fifo.u8string();
        CHECK(!exportSettings.ReloadTheme());
        CHECK_EQ(exportSettings.themeError, "cannot read " + exportSettings.themeFile);

        // Saving over a FIFO neither blocks in open nor replaces it.
        doc.text = "text\n";
        CHECK(!doc.Save(fifo.u8string(), status));
        CHECK_EQ(status, std::string("Cannot save pipe.md"));
        CHECK(!rgui::WriteFileAtomically(fifo, "x", WriteMode::Output, &error));
        CHECK_EQ(error, std::string("not a regular file"));
    });
    CHECK(finished);
    struct stat info {};
    CHECK(lstat(fifo.c_str(), &info) == 0 && S_ISFIFO(info.st_mode));
    CHECK_EQ(TempFilesIn(dir), size_t(0));

    // A character device is no regular file either.
    std::string out, error;
    CHECK(!rgui::ReadRegularFile("/dev/zero", 1 << 20, out, &error));
    CHECK_EQ(error, std::string("not a regular file"));
}

RGUI_TEST("app.safefile.permissions") {
    // A private document stays private, a shared one shared; a new file follows the umask.
    const fs::path dir = TempDir("permissions");
    rgui::Document doc;
    std::string status;
    for (const mode_t mode : {mode_t(0600), mode_t(0640), mode_t(0444)}) {
        const fs::path path = dir / ("doc-" + std::to_string(mode) + ".md");
        CHECK(RayoMd::File::WriteAll(path, "before\n"));
        CHECK(chmod(path.c_str(), mode) == 0);
        CHECK(doc.Open(path.u8string(), status));
        doc.text = "after\n";
        CHECK(doc.Save(path.u8string(), status));
        CHECK_EQ(ReadFile(path), std::string("after\n"));
        CHECK_EQ(ModeOf(path), mode);
        // Exports and settings keep an existing file's bits too.
        CHECK(rgui::WriteFileAtomically(path, "again\n", WriteMode::Output, nullptr));
        CHECK_EQ(ModeOf(path), mode);
    }
    const mode_t previous = umask(027);
    doc.text = "new\n";
    CHECK(doc.Save((dir / "new.md").u8string(), status));
    CHECK(rgui::WriteFileAtomically(dir / "new.pdf", "pdf", WriteMode::Output, nullptr));
    umask(previous);
    CHECK_EQ(ModeOf(dir / "new.md"), mode_t(0640));
    CHECK_EQ(ModeOf(dir / "new.pdf"), mode_t(0640));
    CHECK_EQ(TempFilesIn(dir), size_t(0));
}

RGUI_TEST("app.safefile.symlinked_destination") {
    // Saving a document through a symlink (dotfiles, a notes folder linked elsewhere) updates
    // the file it names and keeps the link; a relative link across folders too. An export or
    // settings file replaces the link instead of writing where it points.
    const fs::path dir = TempDir("symlinked");
    fs::create_directories(dir / "real");
    fs::create_directories(dir / "links");
    CHECK(RayoMd::File::WriteAll(dir / "real" / "notes.md", "old\n"));
    CHECK(chmod((dir / "real" / "notes.md").c_str(), 0600) == 0);
    std::error_code error;
    fs::create_symlink("../real/notes.md", dir / "links" / "notes.md", error);
    CHECK(!error);

    rgui::Document doc;
    std::string status;
    CHECK(doc.Open((dir / "links" / "notes.md").u8string(), status));
    doc.text = "new\n";
    CHECK(doc.Save((dir / "links" / "notes.md").u8string(), status));
    CHECK(IsSymlink(dir / "links" / "notes.md"));
    CHECK_EQ(ReadFile(dir / "real" / "notes.md"), std::string("new\n"));
    CHECK_EQ(ModeOf(dir / "real" / "notes.md"), mode_t(0600));
    CHECK_EQ(doc.filePath, (dir / "links" / "notes.md").u8string());
    CHECK_EQ(TempFilesIn(dir / "real"), size_t(0));
    CHECK_EQ(Entries(dir / "links").size(), size_t(1));

    // A dangling link is refused, not guessed at.
    fs::create_symlink(dir / "real" / "gone.md", dir / "links" / "gone.md", error);
    CHECK(!doc.Save((dir / "links" / "gone.md").u8string(), status));
    CHECK(!fs::exists(dir / "real" / "gone.md"));

    // Output: the link is replaced, its target untouched.
    CHECK(RayoMd::File::WriteAll(dir / "real" / "victim.pdf", "victim"));
    fs::create_symlink(dir / "real" / "victim.pdf", dir / "links" / "out.pdf", error);
    CHECK(rgui::WriteFileAtomically(dir / "links" / "out.pdf", "export", WriteMode::Output, nullptr));
    CHECK(!IsSymlink(dir / "links" / "out.pdf"));
    CHECK_EQ(ReadFile(dir / "links" / "out.pdf"), std::string("export"));
    CHECK_EQ(ReadFile(dir / "real" / "victim.pdf"), std::string("victim"));
}

RGUI_TEST("app.safefile.failed_write") {
    // A write that fails half way (the file size limit here, as a full disk would) leaves the
    // original as it was and no temporary file behind.
    const fs::path dir = TempDir("failed-write");
    const fs::path path = dir / "doc.md";
    CHECK(RayoMd::File::WriteAll(path, "original\n"));
    rgui::Document doc;
    std::string status;
    CHECK(doc.Open(path.u8string(), status));
    doc.text = std::string(64 * 1024, 'x');
    doc.modified = true;

    struct rlimit previous {};
    CHECK(getrlimit(RLIMIT_FSIZE, &previous) == 0);
    void (*previousHandler)(int) = std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = previous;
    small.rlim_cur = 4096;
    CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    const bool saved = doc.Save(path.u8string(), status);
    std::string error;
    const bool written = rgui::WriteFileAtomically(dir / "out.pdf", doc.text, WriteMode::Output, &error);
    setrlimit(RLIMIT_FSIZE, &previous);
    std::signal(SIGXFSZ, previousHandler);

    CHECK(!saved);
    CHECK_EQ(status, std::string("Cannot save doc.md"));
    CHECK(doc.modified);
    CHECK(!written);
    CHECK(error.rfind("cannot write", 0) == 0);
    CHECK_EQ(ReadFile(path), std::string("original\n"));
    CHECK(!fs::exists(dir / "out.pdf"));
    CHECK_EQ(Entries(dir).size(), size_t(1));

    // A folder the user cannot write to: nothing is created (root writes anyway; skip there).
    if (geteuid() != 0) {
        CHECK(chmod(dir.c_str(), 0500) == 0);
        CHECK(!doc.Save(path.u8string(), status));
        CHECK(chmod(dir.c_str(), 0700) == 0);
        CHECK_EQ(ReadFile(path), std::string("original\n"));
        CHECK_EQ(Entries(dir).size(), size_t(1));
    }
}

#else

RGUI_TEST("app.safefile.windows") {
    const fs::path dir = TempDir("windows");
    std::string out, error;

    // A file planted at the old predictable name is left alone.
    CHECK(RayoMd::File::WriteAll(dir / "doc.md.tmp", "planted"));
    rgui::Document doc;
    std::string status;
    doc.text = "line\n";
    doc.crlf = true;
    CHECK(doc.Save((dir / "doc.md").u8string(), status));
    CHECK_EQ(ReadFile(dir / "doc.md"), std::string("line\r\n"));
    CHECK_EQ(ReadFile(dir / "doc.md.tmp"), std::string("planted"));
    CHECK_EQ(Entries(dir).size(), size_t(2));

    // Devices and pipes are no regular files: NUL, and a named pipe whose server never answers.
    CHECK(!rgui::ReadRegularFile(L"\\\\.\\NUL", 1 << 20, out, &error));
    CHECK_EQ(error, std::string("not a regular file"));
    const std::wstring pipeName = L"\\\\.\\pipe\\rayomd-gui-safefile-" + std::to_wstring(GetCurrentProcessId());
    const HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096,
                                         4096, 0, nullptr);
    CHECK(pipe != INVALID_HANDLE_VALUE);
    if (pipe != INVALID_HANDLE_VALUE) {
        CHECK(!rgui::ReadRegularFile(fs::path(pipeName), 1 << 20, out, &error));
        CloseHandle(pipe);
    }

    // A failed replace (the destination is open without delete sharing) keeps the original and
    // leaves no temporary file.
    const HANDLE held = CreateFileW((dir / "doc.md").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(held != INVALID_HANDLE_VALUE);
    doc.text = "changed\n";
    CHECK(!doc.Save((dir / "doc.md").u8string(), status));
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    CHECK_EQ(ReadFile(dir / "doc.md"), std::string("line\r\n"));
    CHECK_EQ(Entries(dir).size(), size_t(2));
}

#endif
