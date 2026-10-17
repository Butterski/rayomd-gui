// SPDX-License-Identifier: Apache-2.0
// rgui-fuzz-seeds <source dir> <out dir>: the starting corpus of every harness, in
// <out dir>/<harness>/. PDFs the engine builds from the test corpus (preview options, and a few
// export variants), the font programs embedded in them plus two system fonts, the JPEG test
// data and the JPEGs inside the PDFs, the bundled Omarchy themes and the theme fixtures,
// fontconfig files, a settings file, Markdown for the editor's helpers, the XKB keymaps and
// Compose fixture, D-Bus messages built with our encoder, and Wayland event streams for every
// event of every object FuzzWlWire makes.
#include "harness.h"

#include "common/file_io.h"
#include "pdf/reader.h"
#include "platform/linux/dbus_wire.h"
#include "platform/linux/wl_wire.h"
#include "rayomd/tiny_pdf.h"
#include "ui/theme.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path g_out;
int g_written = 0;

void Write(const char* harness, const std::string& name, std::string_view bytes) {
    const fs::path dir = g_out / harness;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!RayoMd::File::WriteAll(dir / name, bytes)) {
        std::fprintf(stderr, "rgui-fuzz-seeds: cannot write %s/%s\n", harness, name.c_str());
        std::exit(1);
    }
    ++g_written;
}

std::string Read(const fs::path& path) {
    std::string text;
    if (!RayoMd::File::ReadAll(path, 64u << 20, text)) return {};
    return text;
}

void CopyDir(const char* harness, const fs::path& dir, const char* extension) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (extension && entry.path().extension() != extension) continue;
        Write(harness, entry.path().filename().string(), Read(entry.path()));
    }
}

// The font programs and DCT images of a PDF (built uncompressed: the streams are in place).
void Extract(const std::string& name, const std::string& bytes) {
    std::string error;
    const auto file = rgui::pdf::PdfFile::Parse(bytes, &error);
    if (!file) return;
    for (uint32_t num = 1; num < file->ObjectCount(); ++num) {
        const rgui::pdf::PdfValue& object = file->Object(num);
        for (const char* key : {"FontFile", "FontFile2", "FontFile3"}) {
            const rgui::pdf::PdfValue& stream = file->Get(object, key);
            std::string_view data;
            if (file->StreamData(stream, data, nullptr)) {
                Write("font", name + "-" + std::to_string(num) + "-" + key, data);
            }
        }
        rgui::pdf::PdfRawStream raw;
        if (file->RawStream(object, raw) && raw.data.size() > 2 && static_cast<uint8_t>(raw.data[0]) == 0xFF &&
            static_cast<uint8_t>(raw.data[1]) == 0xD8) {
            Write("jpeg", name + "-" + std::to_string(num) + ".jpg", raw.data);
        }
    }
}

void Pdfs(const fs::path& source) {
    struct Doc {
        const char* name;
        fs::path path;
    };
    const Doc docs[] = {
        {"tester", source / "external/rayomd/tester.md"},
        {"ascii", source / "tests/corpus/ascii.md"},
        {"svg", source / "tests/corpus/svg.md"},
        {"photo", source / "tests/corpus/photo.md"},
        {"math", source / "external/rayomd/docs/development/native_math.md"},
    };
    for (const Doc& doc : docs) {
        const std::string markdown = Read(doc.path);
        if (markdown.empty()) continue;
        for (int variant = 0; variant < 4; ++variant) {
            if (variant > 0 && std::strcmp(doc.name, "tester") != 0) break;
            TinyPdf::PdfOptions options;
            options.sourcePath = doc.path.string();
            if (variant == 1) {
                options.toc = true;
                options.pageNumbers = true;
            } else if (variant == 2) {
                options.pdfua = true;
            } else if (variant == 3) {
                options.compress = true;  // FlateDecode content and fonts: the "filter:" paths
            }
            std::string bytes;
            if (!TinyPdf::BuildPdf(markdown, options, bytes).Ok()) continue;
            const std::string name = std::string(doc.name) + "-" + std::to_string(variant);
            Write("pdf", name + ".pdf", bytes);
            if (!options.compress) Extract(name, bytes);
        }
    }
    // Source Sans 3 as the PDF theme's font: a CID-keyed bare CFF program (CIDFontType0C).
    const fs::path otf = "/usr/share/fonts/adobe-source-sans/SourceSans3-Regular.otf";
    if (fs::exists(otf)) {
        TinyPdf::PdfOptions options;
        options.theme.fontPath = otf.string();
        std::string bytes;
        if (TinyPdf::BuildPdf("# Source Sans\n\nText *and* **bold** `code`.\n", options, bytes).Ok()) {
            Write("pdf", "sourcesans.pdf", bytes);
            Extract("sourcesans", bytes);
        }
    }
}

void Themes(const fs::path& source) {
    for (const rgui::ui::BundledTheme& theme : rgui::ui::kBundledThemes) {
        std::string input(1, static_cast<char>(theme.lightModeFile ? 1 : 0));
        input += theme.colors;
        input += '\0';
        if (theme.shell) input += theme.shell;
        Write("theme", theme.slug, input);
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(source / "tests/data/themes/fixtures", ec)) {
        if (!entry.is_directory()) continue;
        std::string input(1, static_cast<char>(fs::exists(entry.path() / "light.mode") ? 1 : 0));
        input += Read(entry.path() / "colors.toml");
        Write("theme", "fixture-" + entry.path().filename().string(), input);
    }
}

std::string Take(rgui_dbus_buf& buf) {
    std::string bytes(reinterpret_cast<const char*>(buf.data), buf.size);
    rgui_dbus_buf_free(&buf);
    return bytes;
}

void Dbus() {
    auto message = [](uint8_t type, const char* path, const char* interface, const char* member, const char* signature,
                      const std::string& body, uint32_t replySerial) {
        rgui_dbus_message m;
        std::memset(&m, 0, sizeof m);
        m.type = type;
        m.serial = 7;
        m.reply_serial = replySerial;
        m.path = path;
        m.interface = interface;
        m.member = member;
        m.error_name = type == RGUI_DBUS_ERROR ? "org.freedesktop.DBus.Error.UnknownMethod" : nullptr;
        m.destination = ":1.42";
        m.sender = "org.freedesktop.portal.Desktop";
        m.signature = signature;
        m.body = reinterpret_cast<const uint8_t*>(body.data());
        m.body_size = static_cast<uint32_t>(body.size());
        rgui_dbus_buf out;
        std::memset(&out, 0, sizeof out);
        rgui_dbus_encode_message(&out, &m);
        return Take(out);
    };
    rgui_dbus_buf b;

    std::memset(&b, 0, sizeof b);
    rgui_dbus_put_string(&b, ":1.42");
    const std::string hello = Take(b);
    Write("dbus", "hello-reply", message(RGUI_DBUS_METHOD_RETURN, nullptr, nullptr, nullptr, "s", hello, 1));

    std::memset(&b, 0, sizeof b);
    rgui_dbus_put_signature(&b, "v");
    rgui_dbus_put_signature(&b, "u");
    rgui_dbus_put_uint32(&b, 1);
    Write("dbus", "read-reply", message(RGUI_DBUS_METHOD_RETURN, nullptr, nullptr, nullptr, "v", Take(b), 2));

    std::memset(&b, 0, sizeof b);
    rgui_dbus_put_string(&b, "org.freedesktop.appearance");
    rgui_dbus_put_string(&b, "color-scheme");
    rgui_dbus_put_signature(&b, "u");
    rgui_dbus_put_uint32(&b, 2);
    Write("dbus", "setting-changed",
          message(RGUI_DBUS_SIGNAL, "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings",
                  "SettingChanged", "ssv", Take(b), 0));

    std::memset(&b, 0, sizeof b);
    rgui_dbus_put_uint32(&b, 0);
    rgui_dbus_array dict = rgui_dbus_open_array(&b, '{');
    rgui_dbus_open_struct(&b);
    rgui_dbus_put_string(&b, "uris");
    rgui_dbus_put_signature(&b, "as");
    rgui_dbus_array uris = rgui_dbus_open_array(&b, 's');
    rgui_dbus_put_string(&b, "file:///home/user/My%20Notes/a.md");
    rgui_dbus_close_array(&b, uris);
    rgui_dbus_open_struct(&b);
    rgui_dbus_put_string(&b, "choices");
    rgui_dbus_put_signature(&b, "a(ss)");
    rgui_dbus_array choices = rgui_dbus_open_array(&b, '(');
    rgui_dbus_open_struct(&b);
    rgui_dbus_put_string(&b, "encoding");
    rgui_dbus_put_string(&b, "utf8");
    rgui_dbus_close_array(&b, choices);
    rgui_dbus_close_array(&b, dict);
    const std::string response = Take(b);
    Write("dbus", "response",
          message(RGUI_DBUS_SIGNAL, "/org/freedesktop/portal/desktop/request/1_42/rayomd_gui_1",
                  "org.freedesktop.portal.Request", "Response", "ua{sv}", response, 0));
    // The iterator alone: a signature length byte, the signature, the body.
    std::string iterator(1, static_cast<char>(6));
    iterator += "ua{sv}";
    iterator += response;
    Write("dbus", "iterator-response", iterator);

    std::memset(&b, 0, sizeof b);
    rgui_dbus_put_string(&b, "No such method");
    Write("dbus", "error", message(RGUI_DBUS_ERROR, nullptr, nullptr, nullptr, "s", Take(b), 3));

    std::memset(&b, 0, sizeof b);
    rgui_dbus_array creds = rgui_dbus_open_array(&b, '{');
    rgui_dbus_open_struct(&b);
    rgui_dbus_put_string(&b, "UnixUserID");
    rgui_dbus_put_signature(&b, "u");
    rgui_dbus_put_uint32(&b, 1000);
    rgui_dbus_open_struct(&b);
    rgui_dbus_put_string(&b, "ProcessID");
    rgui_dbus_put_signature(&b, "u");
    rgui_dbus_put_uint32(&b, 4242);
    rgui_dbus_close_array(&b, creds);
    Write("dbus", "credentials", message(RGUI_DBUS_METHOD_RETURN, nullptr, nullptr, nullptr, "a{sv}", Take(b), 4));
}

// One event with plausible arguments for its signature.
void PutEvent(std::string& out, uint32_t id, const rgui_wl_message& m, int& fds) {
    std::string args;
    auto u32 = [&args](uint32_t v) { args.append(reinterpret_cast<const char*>(&v), 4); };
    for (const char* s = m.signature; *s; ++s) {
        switch (*s) {
        case 'i': case 'u': u32(1); break;
        case 'f': u32(256 * 12); break;
        case 'o': u32(2); break;
        case 'n': u32(0xff000001u); break;
        case 's': {
            const char text[] = "text/plain;charset=utf-8";
            u32(sizeof text);
            args.append(text, sizeof text);
            while (args.size() % 4) args.push_back('\0');
            break;
        }
        case 'a': u32(8); u32(1); u32(4); break;
        case 'h': ++fds; break;
        default: break;
        }
    }
    const uint32_t header[2] = {id, static_cast<uint32_t>((8 + args.size()) << 16 | m.opcode)};
    out.append(reinterpret_cast<const char*>(header), 8);
    out += args;
}

void Wayland() {
    const rgui_wl_interface* const* objects = nullptr;
    const size_t count = rgui::fuzz::WlWireObjects(&objects);
    std::string all;
    int allFds = 0;
    for (size_t i = 0; i < count; ++i) {
        std::string events;
        int fds = 0;
        for (uint16_t e = 0; e < objects[i]->event_count; ++e) PutEvent(events, static_cast<uint32_t>(i + 2), objects[i]->events[e], fds);
        if (events.empty()) continue;
        fds = fds > 3 ? 3 : fds;
        Write("wl_wire", objects[i]->name, std::string(1, static_cast<char>(fds)) + events);
        all += events;
        allFds = allFds > fds ? allFds : fds;
    }
    Write("wl_wire", "all", std::string(1, static_cast<char>(allFds)) + all);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: rgui-fuzz-seeds <source dir> <out dir>\n");
        return 2;
    }
    const fs::path source = argv[1];
    g_out = argv[2];
    Pdfs(source);
    CopyDir("jpeg", source / "tests/data/jpeg", ".jpg");
    CopyDir("jpeg", source / "tests/corpus", ".jpg");
    for (const char* font : {"/usr/share/fonts/TTF/DejaVuSansMono-Oblique.ttf",
                             "/usr/share/fonts/adobe-source-sans/SourceSans3-ExtraLightIt.otf"}) {
        const std::string bytes = Read(font);
        if (!bytes.empty()) Write("font", fs::path(font).filename().string(), bytes);
    }
    Themes(source);
    CopyDir("fontconf", source / "tests/data/text", ".conf");
    Write("settings", "settings.ini",
          "# RayoMD GUI\nexport.style=1\nexport.customMarginInches=1.25\nexport.landscape=true\n"
          "export.themeFile=/home/user/Themes/company.theme\nui.theme=omarchy\nui.splitRatio=0.5\r\n"
          "ui.lastDirectory=/home/user/Documents/Notatki zażółć\n");
    Write("markdown", "tester.md", Read(source / "external/rayomd/tester.md"));
    Write("markdown", "typing.md",
          "---\ntitle: x\n---\n# One ##\n\n- a\n  - [ ] b\n    1. c\n> > [!NOTE]\n> - d\n```py\n# no\n```\n"
          "~~~~\n~~~\n## Two\n\t3) e\n- - -\n```\n[[#One]] [[\n");
    CopyDir("xkb", source / "tests/data/xkb", ".xkb");
    Write("compose", "Compose", Read(source / "tests/data/xkb/compose/Compose"));
    Write("compose", "self.Compose", Read(source / "tests/data/xkb/compose/locale/self.Compose"));
    Dbus();
    Wayland();
    std::printf("rgui-fuzz-seeds: %d files in %s\n", g_written, g_out.string().c_str());
    return 0;
}
