// SPDX-License-Identifier: Apache-2.0
// The fontconfig rules read here are those of Omarchy (omacom/omarchy 246f33a2, MIT):
// bin/omarchy-font-set writes the user's fonts.conf; default/fontconfig/conf.avail/
// 50-omarchy.conf is installed as /etc/fonts/conf.d/50-omarchy.conf.
#include "text/fontchoice.h"

#include "common/file_io.h"

#include <cstdlib>
#include <filesystem>
#include <utility>

namespace rgui::text {
namespace {

constexpr size_t kMaxXmlBytes = 1u << 20;
constexpr size_t kMaxXmlDepth = 64;
constexpr size_t kMaxXmlElements = 65536;

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<XmlNode> children;
    std::string text;

    const std::string* Attribute(std::string_view key) const {
        for (const auto& a : attributes) {
            if (a.first == key) return &a.second;
        }
        return nullptr;
    }
};

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string Trim(std::string_view s) {
    size_t begin = 0, end = s.size();
    while (begin < end && IsSpace(s[begin])) begin++;
    while (end > begin && IsSpace(s[end - 1])) end--;
    return std::string(s.substr(begin, end - begin));
}

// Character data with the five predefined entities replaced; any other '&' stays as it is.
void AppendDecoded(std::string& out, std::string_view s) {
    static constexpr struct {
        std::string_view entity;
        char c;
    } kEntities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    for (size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&') {
            for (const auto& e : kEntities) {
                if (s.substr(i, e.entity.size()) == e.entity) {
                    out += e.c;
                    i += e.entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out += s[i++];
    }
}

bool IsNameChar(char c) {
    return !IsSpace(c) && c != '/' && c != '>' && c != '=' && c != '<' && c != '"' && c != '\'';
}

// A bounded XML reader: elements, attributes and character data; comments, processing
// instructions and declarations are skipped. False for anything malformed or over a limit.
bool ParseXml(std::string_view xml, XmlNode& root) {
    root = XmlNode();
    if (xml.size() > kMaxXmlBytes) return false;
    std::vector<XmlNode*> open{&root};
    size_t elements = 0;
    size_t at = 0;
    const size_t size = xml.size();
    auto skipTo = [&](std::string_view end) {
        const size_t found = xml.find(end, at);
        if (found == std::string_view::npos) return false;
        at = found + end.size();
        return true;
    };
    while (at < size) {
        if (xml[at] != '<') {
            const size_t next = xml.find('<', at);
            const size_t end = next == std::string_view::npos ? size : next;
            AppendDecoded(open.back()->text, xml.substr(at, end - at));
            at = end;
            continue;
        }
        if (xml.substr(at, 4) == "<!--") {
            if (!skipTo("-->")) return false;
        } else if (xml.substr(at, 9) == "<![CDATA[") {
            const size_t begin = at + 9;
            if (!skipTo("]]>")) return false;
            open.back()->text.append(xml.substr(begin, at - 3 - begin));
        } else if (xml.substr(at, 2) == "<?") {
            if (!skipTo("?>")) return false;
        } else if (xml.substr(at, 2) == "<!") {
            if (!skipTo(">")) return false;
        } else if (xml.substr(at, 2) == "</") {
            at += 2;
            const size_t begin = at;
            while (at < size && IsNameChar(xml[at])) at++;
            const std::string_view name = xml.substr(begin, at - begin);
            while (at < size && IsSpace(xml[at])) at++;
            if (at >= size || xml[at] != '>' || open.size() < 2 || open.back()->name != name) return false;
            at++;
            open.pop_back();
        } else {
            at++;
            const size_t begin = at;
            while (at < size && IsNameChar(xml[at])) at++;
            if (at == begin) return false;
            if (++elements > kMaxXmlElements || open.size() > kMaxXmlDepth) return false;
            XmlNode* const parent = open.back();
            parent->children.emplace_back();
            XmlNode* const node = &parent->children.back();
            node->name = std::string(xml.substr(begin, at - begin));
            bool selfClosing = false;
            for (;;) {
                while (at < size && IsSpace(xml[at])) at++;
                if (at >= size) return false;
                if (xml[at] == '>') {
                    at++;
                    break;
                }
                if (xml[at] == '/') {
                    if (at + 1 >= size || xml[at + 1] != '>') return false;
                    at += 2;
                    selfClosing = true;
                    break;
                }
                const size_t keyBegin = at;
                while (at < size && IsNameChar(xml[at])) at++;
                if (at == keyBegin) return false;
                std::string key(xml.substr(keyBegin, at - keyBegin));
                while (at < size && IsSpace(xml[at])) at++;
                if (at >= size || xml[at] != '=') return false;
                at++;
                while (at < size && IsSpace(xml[at])) at++;
                if (at >= size || (xml[at] != '"' && xml[at] != '\'')) return false;
                const char quote = xml[at++];
                const size_t valueEnd = xml.find(quote, at);
                if (valueEnd == std::string_view::npos) return false;
                std::string value;
                AppendDecoded(value, xml.substr(at, valueEnd - at));
                at = valueEnd + 1;
                node->attributes.emplace_back(std::move(key), std::move(value));
            }
            if (!selfClosing) open.push_back(node);
        }
    }
    return open.size() == 1;
}

// The trimmed text of the first child element called `name`, or nullptr.
const XmlNode* FirstChild(const XmlNode& node, std::string_view name) {
    for (const XmlNode& child : node.children) {
        if (child.name == name) return &child;
    }
    return nullptr;
}

bool HasMonospaceString(const XmlNode& test) {
    for (const XmlNode& child : test.children) {
        if (child.name == "string" && Trim(child.text) == "monospace") return true;
    }
    return false;
}

void AddUnique(std::vector<std::string>& list, std::string value) {
    if (value.empty()) return;
    for (const std::string& s : list) {
        if (FamilyEquals(s, value)) return;
    }
    list.push_back(std::move(value));
}

std::string ReadSmallFile(const std::string& path) {
    std::string content;
    if (path.empty() || !RayoMd::File::ReadAll(std::filesystem::u8path(path), kMaxXmlBytes, content)) return {};
    return content;
}

std::string FileName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

FontChoicePaths FontChoicePaths::Default() {
    FontChoicePaths paths;
#ifndef _WIN32
    const char* const configHome = std::getenv("XDG_CONFIG_HOME");
    const char* const home = std::getenv("HOME");
    if (configHome != nullptr && configHome[0] == '/') paths.userFontsConf = std::string(configHome) + "/fontconfig/fonts.conf";
    else if (home != nullptr && home[0] != 0) paths.userFontsConf = std::string(home) + "/.config/fontconfig/fonts.conf";
    paths.omarchyFontsConf = "/etc/fonts/conf.d/50-omarchy.conf";
#endif
    return paths;
}

std::vector<std::string> MonospaceFamilies(std::string_view xml, bool aliases) {
    std::vector<std::string> families;
    XmlNode root;
    if (!ParseXml(xml, root)) return families;
    const XmlNode* const config = FirstChild(root, "fontconfig");
    if (config == nullptr) return families;
    for (const XmlNode& match : config->children) {
        if (match.name != "match") continue;
        const std::string* const target = match.Attribute("target");
        if (target != nullptr && *target != "pattern") continue;
        bool tested = false, onlyMonospace = true;
        for (const XmlNode& test : match.children) {
            if (test.name != "test") continue;
            tested = true;
            const std::string* const name = test.Attribute("name");
            onlyMonospace = onlyMonospace && name != nullptr && *name == "family" && HasMonospaceString(test);
        }
        if (!tested || !onlyMonospace) continue;
        for (const XmlNode& edit : match.children) {
            if (edit.name != "edit") continue;
            const std::string* const name = edit.Attribute("name");
            const std::string* const mode = edit.Attribute("mode");
            if (name == nullptr || *name != "family") continue;
            if (mode != nullptr && (*mode == "append" || *mode == "append_last" || *mode == "delete" ||
                                       *mode == "delete_all")) {
                continue;
            }
            if (const XmlNode* const s = FirstChild(edit, "string")) AddUnique(families, Trim(s->text));
            break;
        }
    }
    if (aliases) {
        for (const XmlNode& alias : config->children) {
            if (alias.name != "alias") continue;
            const XmlNode* const family = FirstChild(alias, "family");
            if (family == nullptr || Trim(family->text) != "monospace") continue;
            for (const XmlNode& list : alias.children) {
                if (list.name != "prefer" && list.name != "accept" && list.name != "default") continue;
                if (const XmlNode* const first = FirstChild(list, "family")) {
                    AddUnique(families, Trim(first->text));
                    break;
                }
            }
        }
    }
    return families;
}

std::vector<std::string> InterfaceFamilyCandidates(const FontChoicePaths& paths) {
    std::vector<std::string> families;
    for (std::string& f : MonospaceFamilies(ReadSmallFile(paths.userFontsConf), false)) AddUnique(families, std::move(f));
    for (std::string& f : MonospaceFamilies(ReadSmallFile(paths.omarchyFontsConf), true)) AddUnique(families, std::move(f));
#ifdef _WIN32
    static const char* const kBuiltIn[] = {"Cascadia Mono", "Consolas", "Courier New"};
#else
    static const char* const kBuiltIn[] = {"JetBrainsMono Nerd Font", "JetBrains Mono", "DejaVu Sans Mono",
        "Liberation Mono", "Noto Sans Mono"};
#endif
    for (const char* f : kBuiltIn) AddUnique(families, f);
    return families;
}

std::vector<std::string> DefaultFallbackFamilies() {
#ifdef _WIN32
    return {"Segoe UI", "Microsoft YaHei UI", "Yu Gothic UI", "Malgun Gothic", "Segoe UI Symbol", "Segoe UI Emoji"};
#else
    return {"Noto Sans Mono", "DejaVu Sans Mono", "Noto Sans Mono CJK SC", "Noto Sans CJK SC", "Noto Sans",
        "DejaVu Sans", "Noto Sans Symbols", "Noto Sans Symbols 2"};
#endif
}

std::unique_ptr<FontSet::Loaded> FontSet::OpenEntry(const FontEntry& entry) {
    auto loaded = std::make_unique<Loaded>();
    if (!loaded->file.Open(entry.path) ||
        !loaded->face.Open(loaded->file.Data(), loaded->file.Size(), entry.faceIndex)) {
        return nullptr;
    }
    return loaded;
}

bool FontSet::Load(const FontIndex& index, const std::vector<std::string>& candidates,
    const std::vector<std::string>& fallbacks) {
    index_ = &index;
    family_.clear();
    status_.clear();
    regular_.reset();
    bold_.reset();
    fallbacks_.clear();
    cache_.clear();

    for (const std::string& family : candidates) {
        const FontEntry* const entry = index.Find(family, 400, false);
        if (entry == nullptr) continue;
        regular_ = OpenEntry(*entry);
        if (regular_ == nullptr) continue;
        family_ = family;
        const FontEntry* const bold = index.Find(family, 700, false);
        if (bold != nullptr && bold != entry && bold->weight >= 600) bold_ = OpenEntry(*bold);
        break;
    }
    if (regular_ == nullptr) {
        const FontEntry* chosen = nullptr;
        for (const FontEntry& entry : index.Entries()) {
            if (entry.fixedPitch && (regular_ = OpenEntry(entry)) != nullptr) {
                chosen = &entry;
                break;
            }
        }
        for (size_t i = 0; chosen == nullptr && i < index.Entries().size(); i++) {
            if ((regular_ = OpenEntry(index.Entries()[i])) != nullptr) chosen = &index.Entries()[i];
        }
        if (chosen == nullptr) {
            status_ = "No font found";
            return false;
        }
        family_ = chosen->families.empty() ? std::string() : chosen->families.front();
        status_ = "Interface font " + (candidates.empty() ? std::string("monospace") : candidates.front()) +
            " not found; using " + FileName(chosen->path);
    }
    for (const std::string& family : fallbacks) {
        if (!FamilyEquals(family, family_)) fallbacks_.push_back(Fallback{family, {}, {false, false}});
    }
    return true;
}

const FontFace& FontSet::Face(bool bold) const {
    static const FontFace kNone;
    if (bold && bold_ != nullptr) return bold_->face;
    return regular_ != nullptr ? regular_->face : kNone;
}

const FontFace* FontSet::FallbackFace(Fallback& fallback, bool bold) const {
    const int slot = bold ? 1 : 0;
    if (!fallback.tried[slot]) {
        fallback.tried[slot] = true;
        const FontEntry* const entry = index_ != nullptr ? index_->Find(fallback.family, bold ? 700 : 400, false) : nullptr;
        if (entry != nullptr) fallback.faces[slot] = OpenEntry(*entry);
    }
    return fallback.faces[slot] != nullptr ? &fallback.faces[slot]->face : nullptr;
}

FontSet::Resolved FontSet::Resolve(uint32_t cp, bool bold) const {
    if (regular_ == nullptr) return {};
    const uint32_t key = cp << 1 | (bold ? 1u : 0u);
    const auto found = cache_.find(key);
    if (found != cache_.end()) return found->second;
    const FontFace& main = Face(bold);
    Resolved result{&main, main.GlyphForCodePoint(cp)};
    if (result.glyph == 0) {
        for (Fallback& fallback : fallbacks_) {
            const FontFace* const face = FallbackFace(fallback, bold);
            if (face == nullptr) continue;
            const uint16_t glyph = face->GlyphForCodePoint(cp);
            if (glyph != 0) {
                result = {face, glyph};
                break;
            }
        }
    }
    cache_.emplace(key, result);
    return result;
}

} // namespace rgui::text
