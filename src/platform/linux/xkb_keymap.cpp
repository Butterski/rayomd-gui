// SPDX-License-Identifier: Apache-2.0
// The XKB keymap reader. It follows what libxkbcommon does with the same text (keymap.c,
// symbols.c, types.c, compat.c and state.c of libxkbcommon 1.13), written fresh: the rules are
// XKB's, the code is ours.
#include "platform/linux/xkb_keymap.h"

#include "text/unicode.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rgui::xkb {
namespace {

struct KeysymName {
    const char* name;
    uint32_t value;
};

struct KeysymChar {
    uint32_t keysym;
    uint32_t cp;
};

#include "platform/linux/keysyms.inc"

constexpr uint32_t kUnicodeOffset = 0x01000000;
constexpr uint32_t kKeysymMax = 0x1fffffff;
// A level holding several keysyms: it gives NoSymbol to single-keysym lookups.
constexpr uint32_t kSeveralKeysyms = 0xffffffff;

constexpr size_t kMaxText = 8u << 20;
constexpr size_t kMaxTokens = 4u << 20;
constexpr int kMaxDepth = 32;
constexpr uint32_t kMaxKeycode = 0xffff;
constexpr uint32_t kMaxGroups = 32;
constexpr uint32_t kMaxLevels = 256;
constexpr size_t kMaxTypes = 1024;
constexpr size_t kMaxEntries = 1024;
constexpr size_t kMaxInterprets = 4096;
constexpr size_t kMaxKeys = 0x10000;
constexpr int kNumRealMods = 8;
constexpr int kMaxMods = 32;
constexpr uint32_t kRealMask = 0xff;
constexpr uint32_t kShift = 0x1;
constexpr uint32_t kLock = 0x2;
constexpr uint32_t kControl = 0x4;
constexpr uint32_t kMod1 = 0x8;
constexpr uint32_t kMod4 = 0x40;

const char* const kRealModNames[kNumRealMods] = {"Shift", "Lock", "Control", "Mod1",
                                                 "Mod2",  "Mod3", "Mod4",    "Mod5"};

bool IsLatin1Printable(uint32_t v) {
    return (v >= 0x20 && v <= 0x7e) || (v >= 0xa0 && v <= 0xff);
}

// The code point a keysym stands for, control characters included; 0 when none.
uint32_t RawCodepoint(uint32_t keysym) {
    if (IsLatin1Printable(keysym)) return keysym;
    if (keysym >= kUnicodeOffset && keysym <= kUnicodeOffset + 0x10ffff) {
        const uint32_t cp = keysym - kUnicodeOffset;
        if (cp >= 0xd800 && cp <= 0xdfff) return 0;
        return cp;
    }
    const KeysymChar* end = kKeysymChars + sizeof(kKeysymChars) / sizeof(kKeysymChars[0]);
    const KeysymChar* it = std::lower_bound(
        kKeysymChars, end, keysym, [](const KeysymChar& e, uint32_t k) { return e.keysym < k; });
    return it != end && it->keysym == keysym ? it->cp : 0;
}

uint32_t CodepointToKeysym(uint32_t cp) {
    if (IsLatin1Printable(cp)) return cp;
    const KeysymChar* end = kCharKeysyms + sizeof(kCharKeysyms) / sizeof(kCharKeysyms[0]);
    const KeysymChar* it = std::lower_bound(
        kCharKeysyms, end, cp, [](const KeysymChar& e, uint32_t c) { return e.cp < c; });
    if (it != end && it->cp == cp) return it->keysym;
    return kUnicodeOffset + cp;
}

// Unicode's simple uppercase mapping, plus ß -> ẞ (U+1E9E), which libxkbcommon's keysym case
// mapping has and Unicode's simple mapping lacks.
uint32_t UpperCodepoint(uint32_t cp) {
    return cp == 0xdf ? 0x1e9e : text::ToUpper(cp);
}

bool IsLowerKeysym(uint32_t keysym) {
    const uint32_t cp = RawCodepoint(keysym);
    return cp != 0 && UpperCodepoint(cp) != cp;
}

bool IsUpperKeysym(uint32_t keysym) {
    const uint32_t cp = RawCodepoint(keysym);
    return cp != 0 && text::ToLower(cp) != cp;
}

bool IsKeypadKeysym(uint32_t keysym) {
    return keysym >= 0xff80 && keysym <= 0xffbd;  // KP_Space .. KP_Equal
}

// The Caps Lock transformation of one keysym.
uint32_t UpperKeysym(uint32_t keysym) {
    const uint32_t cp = RawCodepoint(keysym);
    if (cp == 0) return keysym;
    const uint32_t upper = UpperCodepoint(cp);
    if (upper == cp) return keysym;
    if (keysym >= kUnicodeOffset) return IsLatin1Printable(upper) ? upper : kUnicodeOffset + upper;
    return CodepointToKeysym(upper);
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

bool ParseHex(std::string_view digits, uint32_t limit, uint32_t& value) {
    if (digits.empty() || digits.size() > 8) return false;
    uint64_t v = 0;
    for (char c : digits) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = v * 16 + static_cast<uint64_t>(d);
    }
    if (v > limit) return false;
    value = static_cast<uint32_t>(v);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Tokens

enum class T : uint8_t { End, Ident, Number, String, KeyName, Punct };

struct Token {
    T type = T::End;
    char punct = 0;
    uint32_t line = 0;
    std::string_view text;  // identifier, string body (escapes kept), key name without <>
    uint64_t number = 0;    // saturated at UINT32_MAX + 1
};

bool IsIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool Lex(std::string_view s, std::vector<Token>& out, std::string* error) {
    uint32_t line = 1;
    size_t i = 0;
    auto fail = [&](const char* what) {
        if (error) *error = "line " + std::to_string(line) + ": " + what;
        return false;
    };
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\n') {
            ++line;
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            ++i;
            continue;
        }
        if (c == '#' || (c == '/' && i + 1 < s.size() && s[i + 1] == '/')) {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
                if (s[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 >= s.size()) return fail("unterminated comment");
            i += 2;
            continue;
        }
        if (out.size() >= kMaxTokens) return fail("too many tokens");
        Token tok;
        tok.line = line;
        if (c == '"') {
            const size_t start = ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\n') return fail("unterminated string");
                if (s[i] == '\\') ++i;
                ++i;
            }
            if (i >= s.size()) return fail("unterminated string");
            tok.type = T::String;
            tok.text = s.substr(start, i - start);
            ++i;
        } else if (c == '<') {
            const size_t start = ++i;
            while (i < s.size() && s[i] != '>' && i - start <= 64) {
                const unsigned char k = static_cast<unsigned char>(s[i]);
                if (k <= 0x20 || k >= 0x7f) return fail("bad key name");
                ++i;
            }
            if (i >= s.size() || s[i] != '>' || i == start) return fail("bad key name");
            tok.type = T::KeyName;
            tok.text = s.substr(start, i - start);
            ++i;
        } else if (IsIdentChar(c)) {
            const size_t start = i;
            while (i < s.size() && IsIdentChar(s[i])) ++i;
            std::string_view word = s.substr(start, i - start);
            bool decimal = true;
            for (char d : word) decimal = decimal && d >= '0' && d <= '9';
            uint32_t hex = 0;
            const bool isHex = word.size() > 2 && word[0] == '0' && (word[1] == 'x' || word[1] == 'X');
            if (decimal) {
                uint64_t v = 0;
                for (char d : word) {
                    v = v * 10 + static_cast<uint64_t>(d - '0');
                    if (v > 0xffffffffull) v = 0x100000000ull;
                }
                // A fraction (geometry) is read and dropped.
                if (i + 1 < s.size() && s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
                    ++i;
                    while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
                }
                tok.type = T::Number;
                tok.number = v;
            } else if (isHex) {
                tok.type = T::Number;
                tok.number = ParseHex(word.substr(2), 0xffffffffu, hex) ? hex : 0x100000000ull;
            } else {
                tok.type = T::Ident;
                tok.text = word;
            }
        } else if (static_cast<unsigned char>(c) >= 0x21 && static_cast<unsigned char>(c) <= 0x7e) {
            tok.type = T::Punct;
            tok.punct = c;
            ++i;
        } else {
            return fail("unexpected byte");
        }
        out.push_back(tok);
    }
    if (out.size() >= kMaxTokens) return fail("too many tokens");
    Token end;
    end.line = line;
    out.push_back(end);
    return true;
}

// ---------------------------------------------------------------------------------------------
// What the text declares, before the derived fields are computed.

enum class Match : uint8_t { Exactly, AllOf, NoneOf, AnyOf, AnyOfOrNone };

struct RawEntry {
    uint32_t mods = 0;
    uint32_t preserve = 0;
    uint32_t level = 0;  // from 0
};

struct RawType {
    std::string name;
    uint32_t mods = 0;
    std::vector<RawEntry> entries;
    uint32_t numLevels = 1;
};

struct Interpret {
    uint32_t keysym = 0;
    bool any = false;  // `Any`: every keysym
    Match match = Match::AnyOfOrNone;
    uint32_t mods = kRealMask;
    int vmod = -1;
    bool repeat = false;
    bool levelOneOnly = false;
};

struct RawGroup {
    bool defined = false;
    bool hasType = false;
    std::string type;
    std::vector<uint32_t> levels;
};

enum class OutOfRange : uint8_t { Wrap, Clamp, Redirect };

struct RawKey {
    std::string name;
    std::vector<RawGroup> groups;
    bool hasDefaultType = false;
    std::string defaultType;
    bool explicitVmods = false;
    uint32_t vmods = 0;
    int repeat = -1;
    bool explicitActions = false;
    OutOfRange outOfRange = OutOfRange::Wrap;
    uint32_t redirect = 0;
};

struct ModMapEntry {
    int mod = 0;
    bool isKey = true;
    std::string key;
    uint32_t keysym = 0;
};

} // namespace

// ---------------------------------------------------------------------------------------------
// The compiled keymap.

struct Keymap::Data {
    struct Entry {
        uint32_t mask = 0;
        uint32_t preserve = 0;
        uint32_t level = 0;
        bool active = true;
    };
    struct Type {
        uint32_t mask = 0;
        std::vector<Entry> entries;
    };
    struct Group {
        uint32_t type = 0;
        std::vector<uint32_t> levels;
    };
    struct KeyInfo {
        std::vector<Group> groups;
        bool repeats = false;
        OutOfRange outOfRange = OutOfRange::Wrap;
        uint32_t redirect = 0;
    };

    std::vector<Type> types;
    std::vector<KeyInfo> keys;
    std::vector<int32_t> keyByCode;  // XKB keycode -> keys index, -1 none
    uint32_t altMask = kMod1;
    uint32_t superMask = kMod4;
    uint32_t numGroups = 1;  // the keymap's layouts: the most groups of any key

    const KeyInfo* Find(uint32_t keycode) const {
        if (keycode >= keyByCode.size() || keyByCode[keycode] < 0) return nullptr;
        return &keys[static_cast<size_t>(keyByCode[keycode])];
    }

    // The group index of `key` for an effective group (wrapped into the keymap's layouts first,
    // as the compositor's state does), or -1 when the key has none.
    int EffectiveGroup(const KeyInfo& key, uint32_t group) const {
        const uint32_t n = static_cast<uint32_t>(key.groups.size());
        if (n == 0) return -1;
        if (group >= numGroups) group %= numGroups;
        if (group < n) return static_cast<int>(group);
        switch (key.outOfRange) {
        case OutOfRange::Clamp: return static_cast<int>(n - 1);
        case OutOfRange::Redirect: return static_cast<int>(key.redirect < n ? key.redirect : 0);
        case OutOfRange::Wrap: break;
        }
        return static_cast<int>(group % n);
    }

    uint32_t Lookup(uint32_t keycode, uint32_t group, uint32_t mods) const {
        const KeyInfo* key = Find(keycode);
        if (!key) return 0;
        const int g = EffectiveGroup(*key, group);
        if (g < 0) return 0;
        const Group& grp = key->groups[static_cast<size_t>(g)];
        const Type& type = types[grp.type];
        const uint32_t relevant = mods & type.mask;
        const Entry* entry = nullptr;
        for (const Entry& e : type.entries) {
            if (e.active && e.mask == relevant) {
                entry = &e;
                break;
            }
        }
        const uint32_t level = entry ? entry->level : 0;
        uint32_t sym = level < grp.levels.size() ? grp.levels[level] : 0;
        if (sym == kSeveralKeysyms) return 0;
        if (sym != 0 && (mods & kLock)) {
            const uint32_t consumed = type.mask & ~(entry ? entry->preserve : 0);
            if (!(consumed & kLock)) sym = UpperKeysym(sym);
        }
        return sym;
    }

    uint32_t LevelOneSym(const KeyInfo& key, int group) const {
        const Group& grp = key.groups[static_cast<size_t>(group)];
        const uint32_t sym = grp.levels.empty() ? 0 : grp.levels[0];
        return sym == kSeveralKeysyms ? 0 : sym;
    }
};

namespace {

class Builder {
public:
    Builder(const std::vector<Token>& tokens, std::string* error) : t_(tokens), error_(error) {
        for (int i = 0; i < kNumRealMods; ++i) modNames_.emplace_back(kRealModNames[i]);
    }

    bool ParseKeymap();
    std::unique_ptr<Keymap::Data> Finish();

private:
    // Token access.
    const Token& Peek(size_t k = 0) const {
        const size_t i = pos_ + k;
        return i < t_.size() ? t_[i] : t_.back();
    }
    const Token& Next() {
        const Token& tok = Peek();
        if (pos_ < t_.size() - 1) ++pos_;
        return tok;
    }
    bool IsPunct(char p, size_t k = 0) const { return Peek(k).type == T::Punct && Peek(k).punct == p; }
    bool IsWord(const char* word, size_t k = 0) const {
        return Peek(k).type == T::Ident && EqualsIgnoreCase(Peek(k).text, word);
    }
    bool Accept(char p) {
        if (!IsPunct(p)) return false;
        Next();
        return true;
    }
    bool Fail(const std::string& what) {
        if (error_ && error_->empty()) *error_ = "line " + std::to_string(Peek().line) + ": " + what;
        return false;
    }
    bool Expect(char p) {
        if (Accept(p)) return true;
        return Fail(std::string("expected '") + p + "'");
    }

    bool SkipStatement();
    bool SkipValue();
    bool SkipBalanced();
    bool ParseSection();
    bool ParseBody(bool (Builder::*statement)());
    bool KeycodesStatement();
    bool TypesStatement();
    bool CompatStatement();
    bool SymbolsStatement();
    bool VirtualModifiers();
    bool ParseType();
    bool ParseInterpret();
    bool ParseKey();
    bool ParseModMap();
    bool ParseSymbolList(std::vector<uint32_t>& levels);
    bool ParseKeysym(uint32_t& keysym);
    bool ParseModMask(uint32_t& mask, bool realOnly);
    bool ParseLevel(uint32_t& level);
    bool ParseGroupIndex(uint32_t& group);
    bool ParseBool(bool& value);
    bool ParseString(std::string& value);
    int FindMod(std::string_view name) const;

    uint32_t Resolve(uint32_t mods) const;
    const Interpret* FindInterpret(const RawKey& key, uint32_t modmap, uint32_t group, uint32_t level) const;

    const std::vector<Token>& t_;
    std::string* error_;
    size_t pos_ = 0;

    std::vector<std::string> modNames_;
    uint32_t mapping_[kMaxMods] = {};
    bool explicitMapping_[kMaxMods] = {};
    std::unordered_map<std::string, uint32_t> keycodes_;
    std::unordered_map<std::string, std::string> aliases_;
    std::vector<RawType> types_;
    std::vector<Interpret> interprets_;
    Interpret defaultInterpret_;
    std::vector<RawKey> keys_;
    std::unordered_map<std::string, size_t> keyIndex_;
    std::vector<ModMapEntry> modmap_;
};

bool Builder::SkipBalanced() {
    // Skips one bracketed group starting at the current token.
    int depth = 0;
    do {
        const Token& tok = Next();
        if (tok.type == T::End) return Fail("unexpected end");
        if (tok.type == T::Punct) {
            if (tok.punct == '{' || tok.punct == '[' || tok.punct == '(') {
                if (++depth > kMaxDepth) return Fail("nested too deeply");
            } else if (tok.punct == '}' || tok.punct == ']' || tok.punct == ')') {
                --depth;
            }
        }
    } while (depth > 0);
    return true;
}

bool Builder::SkipStatement() {
    // Up to and including the next ';' outside brackets; stops before a closing '}'.
    for (;;) {
        const Token& tok = Peek();
        if (tok.type == T::End) return Fail("unexpected end");
        if (tok.type == T::Punct) {
            if (tok.punct == ';') {
                Next();
                return true;
            }
            if (tok.punct == '}') return true;
            if (tok.punct == ']' || tok.punct == ')') return Fail("unbalanced brackets");
            if (tok.punct == '{' || tok.punct == '[' || tok.punct == '(') {
                if (!SkipBalanced()) return false;
                continue;
            }
        }
        Next();
    }
}

bool Builder::SkipValue() {
    // Up to the next ',' or the closing bracket of the enclosing list.
    for (;;) {
        const Token& tok = Peek();
        if (tok.type == T::End) return Fail("unexpected end");
        if (tok.type == T::Punct) {
            if (tok.punct == ',' || tok.punct == ';' || tok.punct == '}' || tok.punct == ']' ||
                tok.punct == ')')
                return true;
            if (tok.punct == '{' || tok.punct == '[' || tok.punct == '(') {
                if (!SkipBalanced()) return false;
                continue;
            }
        }
        Next();
    }
}

bool Builder::ParseKeymap() {
    // [flags] xkb_keymap ["name"] { sections } [;]
    while (Peek().type == T::Ident && !IsWord("xkb_keymap")) Next();
    if (!IsWord("xkb_keymap")) return Fail("expected xkb_keymap");
    Next();
    if (Peek().type == T::String) Next();
    if (!Expect('{')) return false;
    while (!IsPunct('}')) {
        if (Peek().type == T::End) return Fail("unexpected end");
        if (!ParseSection()) return false;
    }
    Next();
    Accept(';');
    while (Accept(';')) {
    }
    if (Peek().type != T::End) return Fail("text after the keymap");
    return true;
}

bool Builder::ParseSection() {
    // Flags (default, partial, alphanumeric_keys, ...) come before the section keyword.
    size_t k = 0;
    while (Peek(k).type == T::Ident && !EqualsIgnoreCase(Peek(k).text.substr(0, 4), "xkb_")) ++k;
    if (Peek(k).type != T::Ident) return Fail("expected a section");
    for (size_t i = 0; i < k; ++i) Next();
    const std::string_view kind = Next().text;
    if (Peek().type == T::String) Next();
    if (!IsPunct('{')) return Fail("expected '{'");
    bool ok;
    if (EqualsIgnoreCase(kind, "xkb_keycodes")) {
        ok = ParseBody(&Builder::KeycodesStatement);
    } else if (EqualsIgnoreCase(kind, "xkb_types")) {
        ok = ParseBody(&Builder::TypesStatement);
    } else if (EqualsIgnoreCase(kind, "xkb_compatibility") || EqualsIgnoreCase(kind, "xkb_compat") ||
               EqualsIgnoreCase(kind, "xkb_compatibility_map") || EqualsIgnoreCase(kind, "xkb_compat_map")) {
        ok = ParseBody(&Builder::CompatStatement);
    } else if (EqualsIgnoreCase(kind, "xkb_symbols")) {
        ok = ParseBody(&Builder::SymbolsStatement);
    } else {
        ok = SkipBalanced();  // xkb_geometry and anything newer
    }
    if (!ok) return false;
    Accept(';');
    return true;
}

bool Builder::ParseBody(bool (Builder::*statement)()) {
    Next();  // '{'
    while (!IsPunct('}')) {
        if (Peek().type == T::End) return Fail("unexpected end");
        if (Accept(';')) continue;
        // Merge modes are irrelevant in a compiled keymap.
        while (IsWord("override") || IsWord("augment") || IsWord("replace") || IsWord("alternate"))
            Next();
        const size_t before = pos_;
        if (!(this->*statement)()) return false;
        if (pos_ == before && !SkipStatement()) return false;
        if (pos_ == before) return Fail("unexpected token");
    }
    Next();  // '}'
    return true;
}

int Builder::FindMod(std::string_view name) const {
    for (int i = 0; i < kNumRealMods; ++i)
        if (EqualsIgnoreCase(name, modNames_[static_cast<size_t>(i)])) return i;
    for (size_t i = kNumRealMods; i < modNames_.size(); ++i)
        if (name == modNames_[i]) return static_cast<int>(i);
    return -1;
}

bool Builder::ParseModMask(uint32_t& mask, bool realOnly) {
    mask = 0;
    for (;;) {
        const Token& tok = Peek();
        if (tok.type == T::Number) {
            if (tok.number > 0xffffffffull) return Fail("bad modifier mask");
            mask |= static_cast<uint32_t>(tok.number);
        } else if (tok.type == T::Ident) {
            if (EqualsIgnoreCase(tok.text, "none")) {
            } else if (EqualsIgnoreCase(tok.text, "all") || EqualsIgnoreCase(tok.text, "any")) {
                mask |= realOnly ? kRealMask : (modNames_.size() >= 32 ? 0xffffffffu
                                                   : (1u << modNames_.size()) - 1);
            } else {
                const int mod = FindMod(tok.text);
                if (mod < 0) return Fail("unknown modifier " + std::string(tok.text));
                if (realOnly && mod >= kNumRealMods) return Fail("not a real modifier");
                mask |= 1u << mod;
            }
        } else {
            return Fail("expected a modifier");
        }
        Next();
        if (!Accept('+')) return true;
    }
}

bool Builder::ParseLevel(uint32_t& level) {
    const Token& tok = Next();
    uint64_t n = 0;
    if (tok.type == T::Number) {
        n = tok.number;
    } else if (tok.type == T::Ident && tok.text.size() > 5 && EqualsIgnoreCase(tok.text.substr(0, 5), "level")) {
        for (char c : tok.text.substr(5)) {
            if (c < '0' || c > '9') return Fail("bad level");
            n = n * 10 + static_cast<uint64_t>(c - '0');
            if (n > kMaxLevels) break;
        }
    } else {
        return Fail("expected a level");
    }
    if (n < 1 || n > kMaxLevels) return Fail("level out of range");
    level = static_cast<uint32_t>(n - 1);
    return true;
}

bool Builder::ParseGroupIndex(uint32_t& group) {
    const Token& tok = Next();
    uint64_t n = 0;
    if (tok.type == T::Number) {
        n = tok.number;
    } else if (tok.type == T::Ident && tok.text.size() > 5 && EqualsIgnoreCase(tok.text.substr(0, 5), "group")) {
        for (char c : tok.text.substr(5)) {
            if (c < '0' || c > '9') return Fail("bad group");
            n = n * 10 + static_cast<uint64_t>(c - '0');
            if (n > kMaxGroups) break;
        }
    } else {
        return Fail("expected a group");
    }
    if (n < 1 || n > kMaxGroups) return Fail("group out of range");
    group = static_cast<uint32_t>(n - 1);
    return true;
}

bool Builder::ParseBool(bool& value) {
    const Token& tok = Next();
    if (tok.type == T::Ident) {
        if (EqualsIgnoreCase(tok.text, "true") || EqualsIgnoreCase(tok.text, "yes") ||
            EqualsIgnoreCase(tok.text, "on")) {
            value = true;
            return true;
        }
        if (EqualsIgnoreCase(tok.text, "false") || EqualsIgnoreCase(tok.text, "no") ||
            EqualsIgnoreCase(tok.text, "off")) {
            value = false;
            return true;
        }
    }
    return Fail("expected a boolean");
}

bool Builder::ParseString(std::string& value) {
    const Token& tok = Next();
    if (tok.type != T::String) return Fail("expected a string");
    value.clear();
    const std::string_view s = tok.text;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            value.push_back(s[i]);
            continue;
        }
        const char e = s[++i];
        if (e == 'n') value.push_back('\n');
        else if (e == 't') value.push_back('\t');
        else if (e >= '0' && e <= '7') {
            unsigned v = static_cast<unsigned>(e - '0');
            for (int d = 0; d < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++d)
                v = v * 8 + static_cast<unsigned>(s[++i] - '0');
            value.push_back(static_cast<char>(v & 0xff));
        } else value.push_back(e);
    }
    return true;
}

bool Builder::ParseKeysym(uint32_t& keysym) {
    const Token& tok = Next();
    if (tok.type == T::Number) {
        if (tok.number <= 9) keysym = static_cast<uint32_t>('0' + tok.number);
        else if (tok.number <= kKeysymMax) keysym = static_cast<uint32_t>(tok.number);
        else return Fail("keysym out of range");
        return true;
    }
    if (tok.type == T::Ident) {
        keysym = KeysymFromName(tok.text);  // unknown names are NoSymbol, as in libxkbcommon
        return true;
    }
    return Fail("expected a keysym");
}

bool Builder::VirtualModifiers() {
    // virtual_modifiers A, B = Mod1, C = 0x4000;
    Next();
    for (;;) {
        const Token& name = Next();
        if (name.type != T::Ident) return Fail("expected a modifier name");
        int mod = FindMod(name.text);
        if (mod < 0) {
            if (modNames_.size() >= kMaxMods) return Fail("too many modifiers");
            modNames_.emplace_back(name.text);
            mod = static_cast<int>(modNames_.size() - 1);
        }
        if (Accept('=')) {
            uint32_t mask = 0;
            if (!ParseModMask(mask, false)) return false;
            if (mod >= kNumRealMods) {
                // Virtual modifiers in a mapping stand for their own bits (canonical encoding).
                mapping_[mod] = mask;
                explicitMapping_[mod] = true;
            }
        }
        if (Accept(';')) return true;
        if (!Expect(',')) return false;
    }
}

bool Builder::KeycodesStatement() {
    const Token& tok = Peek();
    if (tok.type == T::KeyName) {
        // <NAME> = keycode;
        const std::string name(Next().text);
        if (!Expect('=')) return false;
        const Token& code = Next();
        if (code.type != T::Number) return Fail("expected a keycode");
        if (code.number <= kMaxKeycode) keycodes_[name] = static_cast<uint32_t>(code.number);
        return Expect(';');
    }
    if (IsWord("alias") && Peek(1).type == T::KeyName) {
        Next();
        const std::string alias(Next().text);
        if (!Expect('=')) return false;
        const Token& real = Next();
        if (real.type != T::KeyName) return Fail("expected a key name");
        aliases_[alias] = std::string(real.text);
        return Expect(';');
    }
    return SkipStatement();  // minimum, maximum, indicator
}

bool Builder::TypesStatement() {
    if (IsWord("virtual_modifiers")) return VirtualModifiers();
    if (IsWord("type") && Peek(1).type == T::String) return ParseType();
    return SkipStatement();
}

bool Builder::ParseType() {
    Next();
    if (types_.size() >= kMaxTypes) return Fail("too many types");
    RawType type;
    if (!ParseString(type.name)) return false;
    if (!Expect('{')) return false;
    uint32_t maxLevel = 0;
    while (!Accept('}')) {
        if (Peek().type == T::End) return Fail("unexpected end");
        if (Accept(';')) continue;
        if (IsWord("modifiers")) {
            Next();
            if (!Expect('=') || !ParseModMask(type.mods, false)) return false;
        } else if (IsWord("map")) {
            Next();
            if (!Expect('[')) return false;
            uint32_t mods = 0;
            if (!ParseModMask(mods, false) || !Expect(']') || !Expect('=')) return false;
            uint32_t level = 0;
            if (!ParseLevel(level)) return false;
            maxLevel = std::max(maxLevel, level);
            auto it = std::find_if(type.entries.begin(), type.entries.end(),
                                   [&](const RawEntry& e) { return e.mods == mods; });
            if (it != type.entries.end()) {
                it->level = level;  // a later map[] of the same modifiers wins
            } else {
                if (type.entries.size() >= kMaxEntries) return Fail("too many map entries");
                RawEntry entry;
                entry.mods = mods;
                entry.level = level;
                type.entries.push_back(entry);
            }
        } else if (IsWord("preserve")) {
            Next();
            if (!Expect('[')) return false;
            uint32_t mods = 0;
            uint32_t preserve = 0;
            if (!ParseModMask(mods, false) || !Expect(']') || !Expect('=') ||
                !ParseModMask(preserve, false))
                return false;
            auto it = std::find_if(type.entries.begin(), type.entries.end(),
                                   [&](const RawEntry& e) { return e.mods == mods; });
            if (it != type.entries.end()) {
                it->preserve = preserve;
            } else {
                // A preserve without its map[] entry maps to level 1.
                if (type.entries.size() >= kMaxEntries) return Fail("too many map entries");
                RawEntry entry;
                entry.mods = mods;
                entry.preserve = preserve;
                type.entries.push_back(entry);
            }
        } else if (IsWord("level_name") || IsWord("levelname")) {
            Next();
            if (!Expect('[')) return false;
            uint32_t level = 0;
            std::string ignored;
            if (!ParseLevel(level) || !Expect(']') || !Expect('=') || !ParseString(ignored)) return false;
            maxLevel = std::max(maxLevel, level);
        } else {
            if (!SkipStatement()) return false;
            continue;
        }
        if (!IsPunct('}') && !Expect(';')) return false;
    }
    Accept(';');
    type.numLevels = maxLevel + 1;
    for (RawType& existing : types_) {
        if (existing.name == type.name) {
            existing = std::move(type);
            return true;
        }
    }
    types_.push_back(std::move(type));
    return true;
}

bool Builder::CompatStatement() {
    if (IsWord("virtual_modifiers")) return VirtualModifiers();
    if (IsWord("interpret") && IsPunct('.', 1)) {
        // interpret.field = value;
        Next();
        Next();
        const Token& field = Next();
        if (field.type != T::Ident || !Expect('=')) return Fail("bad interpret default");
        if (EqualsIgnoreCase(field.text, "repeat")) {
            if (!ParseBool(defaultInterpret_.repeat)) return false;
        } else if (EqualsIgnoreCase(field.text, "useModMapMods") || EqualsIgnoreCase(field.text, "usemodmap")) {
            const Token& v = Next();
            defaultInterpret_.levelOneOnly =
                v.type == T::Ident && (EqualsIgnoreCase(v.text, "level1") || EqualsIgnoreCase(v.text, "levelone"));
        } else if (EqualsIgnoreCase(field.text, "virtualModifier") || EqualsIgnoreCase(field.text, "virtualmod")) {
            const Token& v = Next();
            const int mod = v.type == T::Ident ? FindMod(v.text) : -1;
            if (mod < kNumRealMods) return Fail("bad virtual modifier");
            defaultInterpret_.vmod = mod;
        } else {
            return SkipStatement();
        }
        return Expect(';');
    }
    if (IsWord("interpret")) return ParseInterpret();
    return SkipStatement();  // indicator, group, setMods defaults, ...
}

bool Builder::ParseInterpret() {
    Next();
    if (interprets_.size() >= kMaxInterprets) return Fail("too many interprets");
    Interpret interp = defaultInterpret_;
    if (IsWord("Any") || IsWord("NoSymbol")) {
        Next();
        interp.any = true;
    } else if (!ParseKeysym(interp.keysym)) {
        return false;
    }
    interp.match = Match::AnyOfOrNone;
    interp.mods = kRealMask;
    if (Accept('+')) {
        static const struct {
            const char* name;
            Match match;
        } kPredicates[] = {{"NoneOf", Match::NoneOf}, {"AnyOfOrNone", Match::AnyOfOrNone},
                           {"AnyOf", Match::AnyOf},   {"AllOf", Match::AllOf},
                           {"Exactly", Match::Exactly}};
        bool predicate = false;
        if (Peek().type == T::Ident && IsPunct('(', 1)) {
            for (const auto& p : kPredicates) {
                if (EqualsIgnoreCase(Peek().text, p.name)) {
                    interp.match = p.match;
                    predicate = true;
                }
            }
            if (!predicate) return Fail("unknown predicate");
            Next();
            Next();
            if (!ParseModMask(interp.mods, true) || !Expect(')')) return false;
        } else if (IsWord("Any")) {
            Next();
            interp.match = Match::AnyOf;
            interp.mods = kRealMask;
        } else {
            interp.match = Match::Exactly;
            if (!ParseModMask(interp.mods, true)) return false;
        }
    }
    if (!Expect('{')) return false;
    while (!Accept('}')) {
        if (Peek().type == T::End) return Fail("unexpected end");
        if (Accept(';')) continue;
        const Token& field = Peek();
        if (field.type == T::Ident && IsPunct('=', 1)) {
            if (EqualsIgnoreCase(field.text, "repeat")) {
                Next();
                Next();
                if (!ParseBool(interp.repeat)) return false;
            } else if (EqualsIgnoreCase(field.text, "virtualModifier") ||
                       EqualsIgnoreCase(field.text, "virtualmod")) {
                Next();
                Next();
                const Token& v = Next();
                const int mod = v.type == T::Ident ? FindMod(v.text) : -1;
                if (mod < kNumRealMods) return Fail("bad virtual modifier");
                interp.vmod = mod;
            } else if (EqualsIgnoreCase(field.text, "useModMapMods") ||
                       EqualsIgnoreCase(field.text, "usemodmap")) {
                Next();
                Next();
                const Token& v = Next();
                interp.levelOneOnly = v.type == T::Ident && (EqualsIgnoreCase(v.text, "level1") ||
                                                             EqualsIgnoreCase(v.text, "levelone"));
            } else {
                if (!SkipStatement()) return false;  // action, locking
                continue;
            }
            if (!IsPunct('}') && !Expect(';')) return false;
        } else if (!SkipStatement()) {
            return false;
        }
    }
    Accept(';');
    interprets_.push_back(interp);
    return true;
}

bool Builder::SymbolsStatement() {
    if (IsWord("virtual_modifiers")) return VirtualModifiers();
    if (IsWord("key") && Peek(1).type == T::KeyName) return ParseKey();
    if ((IsWord("modifier_map") || IsWord("modmap") || IsWord("mod_map")) && Peek(1).type == T::Ident)
        return ParseModMap();
    return SkipStatement();  // name[Group1] = "...", key.repeat = ..., include
}

bool Builder::ParseSymbolList(std::vector<uint32_t>& levels) {
    // [ sym, sym, { sym, sym }, ... ]
    if (!Expect('[')) return false;
    levels.clear();
    if (Accept(']')) return true;
    for (;;) {
        if (levels.size() >= kMaxLevels) return Fail("too many levels");
        if (Accept('{')) {
            uint32_t count = 0;
            uint32_t first = 0;
            if (!IsPunct('}')) {
                for (;;) {
                    uint32_t sym = 0;
                    if (!ParseKeysym(sym)) return false;
                    if (sym != 0) {
                        if (count == 0) first = sym;
                        ++count;
                    }
                    if (count > kMaxLevels) return Fail("too many keysyms");
                    if (!Accept(',')) break;
                }
            }
            if (!Expect('}')) return false;
            levels.push_back(count > 1 ? kSeveralKeysyms : first);
        } else {
            uint32_t sym = 0;
            if (!ParseKeysym(sym)) return false;
            levels.push_back(sym);
        }
        if (Accept(']')) return true;
        if (!Expect(',')) return false;
    }
}

bool Builder::ParseKey() {
    Next();
    const std::string name(Next().text);
    if (!Expect('{')) return false;
    size_t index;
    auto found = keyIndex_.find(name);
    if (found != keyIndex_.end()) {
        index = found->second;
    } else {
        if (keys_.size() >= kMaxKeys) return Fail("too many keys");
        index = keys_.size();
        keys_.emplace_back();
        keys_.back().name = name;
        keyIndex_[name] = index;
    }
    uint32_t nextBareGroup = 0;
    auto group = [&](uint32_t g) -> RawGroup& {
        RawKey& key = keys_[index];
        if (key.groups.size() <= g) key.groups.resize(g + 1);
        return key.groups[g];
    };
    while (!Accept('}')) {
        if (Peek().type == T::End) return Fail("unexpected end");
        if (Accept(',')) continue;
        if (IsPunct('[')) {
            if (nextBareGroup >= kMaxGroups) return Fail("too many groups");
            RawGroup& g = group(nextBareGroup++);
            if (!ParseSymbolList(g.levels)) return false;
            g.defined = true;
            continue;
        }
        bool negated = false;
        if (Accept('!')) negated = true;
        const Token& field = Next();
        if (field.type != T::Ident) return Fail("expected a key field");
        const std::string_view f = field.text;
        bool hasIndex = false;
        uint32_t index2 = 0;
        if (IsPunct('[')) {
            Next();
            if (!ParseGroupIndex(index2) || !Expect(']')) return false;
            hasIndex = true;
        }
        if (!IsPunct('=')) {
            // A bare flag: groupsWrap, groupsClamp, repeat, !repeat, ...
            if (EqualsIgnoreCase(f, "repeat") || EqualsIgnoreCase(f, "repeats") || EqualsIgnoreCase(f, "repeating"))
                keys_[index].repeat = negated ? 0 : 1;
            else if (EqualsIgnoreCase(f, "groupsWrap") || EqualsIgnoreCase(f, "wrapGroups"))
                keys_[index].outOfRange = OutOfRange::Wrap;
            else if (EqualsIgnoreCase(f, "groupsClamp") || EqualsIgnoreCase(f, "clampGroups"))
                keys_[index].outOfRange = OutOfRange::Clamp;
            continue;
        }
        Next();  // '='
        RawKey& key = keys_[index];
        if (EqualsIgnoreCase(f, "symbols")) {
            const uint32_t g = hasIndex ? index2 : 0;
            RawGroup& grp = group(g);
            if (!ParseSymbolList(grp.levels)) return false;
            grp.defined = true;
            nextBareGroup = std::max(nextBareGroup, g + 1);
        } else if (EqualsIgnoreCase(f, "actions")) {
            keys_[index].explicitActions = true;
            if (hasIndex) group(index2).defined = true;
            if (!SkipValue()) return false;
        } else if (EqualsIgnoreCase(f, "type")) {
            std::string typeName;
            if (!ParseString(typeName)) return false;
            if (hasIndex) {
                RawGroup& grp = group(index2);
                grp.hasType = true;
                grp.type = std::move(typeName);
            } else {
                key.hasDefaultType = true;
                key.defaultType = std::move(typeName);
            }
        } else if (EqualsIgnoreCase(f, "vmods") || EqualsIgnoreCase(f, "virtualMods") ||
                   EqualsIgnoreCase(f, "virtualModifiers")) {
            uint32_t mods = 0;
            if (!ParseModMask(mods, false)) return false;
            key.explicitVmods = true;
            key.vmods = mods & ~kRealMask;
        } else if (EqualsIgnoreCase(f, "repeat") || EqualsIgnoreCase(f, "repeats") ||
                   EqualsIgnoreCase(f, "repeating")) {
            if (IsWord("default")) {
                Next();
                key.repeat = -1;
            } else {
                bool value = false;
                if (!ParseBool(value)) return false;
                key.repeat = value != negated ? 1 : 0;
            }
        } else if (EqualsIgnoreCase(f, "groupsWrap") || EqualsIgnoreCase(f, "wrapGroups")) {
            bool value = false;
            if (!ParseBool(value)) return false;
            if (value) key.outOfRange = OutOfRange::Wrap;
        } else if (EqualsIgnoreCase(f, "groupsClamp") || EqualsIgnoreCase(f, "clampGroups")) {
            bool value = false;
            if (!ParseBool(value)) return false;
            key.outOfRange = value ? OutOfRange::Clamp : OutOfRange::Wrap;
        } else if (EqualsIgnoreCase(f, "groupsRedirect") || EqualsIgnoreCase(f, "redirectGroups")) {
            uint32_t g = 0;
            if (!ParseGroupIndex(g)) return false;
            key.outOfRange = OutOfRange::Redirect;
            key.redirect = g;
        } else if (!SkipValue()) {
            return false;  // locks, radioGroup, overlay, ...
        }
    }
    Accept(';');
    return true;
}

bool Builder::ParseModMap() {
    // modifier_map Mod1 { <LALT>, Alt_L };
    Next();
    const Token& modTok = Next();
    const int mod = FindMod(modTok.text);
    if (mod < 0 || mod >= kNumRealMods) {
        if (mod < 0 && !EqualsIgnoreCase(modTok.text, "none")) return Fail("bad modifier_map");
        return SkipStatement();
    }
    if (!Expect('{')) return false;
    while (!Accept('}')) {
        if (Accept(',')) continue;
        if (modmap_.size() >= kMaxKeys * 2) return Fail("modifier_map too long");
        ModMapEntry entry;
        entry.mod = mod;
        if (Peek().type == T::KeyName) {
            entry.key = std::string(Next().text);
        } else {
            entry.isKey = false;
            if (!ParseKeysym(entry.keysym)) return false;
        }
        modmap_.push_back(std::move(entry));
    }
    Accept(';');
    return true;
}

uint32_t Builder::Resolve(uint32_t mods) const {
    uint32_t mask = mods & kRealMask;
    for (int i = kNumRealMods; i < kMaxMods; ++i)
        if (mods & (1u << i)) mask |= mapping_[i];
    return mask;
}

const Interpret* Builder::FindInterpret(const RawKey& key, uint32_t modmap, uint32_t group,
                                        uint32_t level) const {
    static const Interpret kDefault = [] {
        Interpret d;
        d.repeat = true;
        return d;
    }();
    if (group >= key.groups.size() || level >= key.groups[group].levels.size()) return nullptr;
    const uint32_t sym = key.groups[group].levels[level];
    if (sym == 0) return nullptr;
    if (sym == kSeveralKeysyms) return &kDefault;
    for (const Interpret& interp : interprets_) {
        if (!interp.any && interp.keysym != sym) continue;
        const uint32_t mods = interp.levelOneOnly && level != 0 ? 0 : modmap;
        bool found = false;
        switch (interp.match) {
        case Match::NoneOf: found = !(interp.mods & mods); break;
        case Match::AnyOfOrNone: found = !mods || (interp.mods & mods); break;
        case Match::AnyOf: found = (interp.mods & mods) != 0; break;
        case Match::AllOf: found = (interp.mods & mods) == interp.mods; break;
        case Match::Exactly: found = interp.mods == mods; break;
        }
        if (found) return &interp;
    }
    return &kDefault;
}

std::unique_ptr<Keymap::Data> Builder::Finish() {
    auto data = std::make_unique<Keymap::Data>();

    // Key names to keycodes, through aliases.
    auto keycodeOf = [&](const std::string& name, uint32_t& code) {
        const std::string* n = &name;
        for (int hop = 0; hop < 8; ++hop) {
            auto it = keycodes_.find(*n);
            if (it != keycodes_.end()) {
                code = it->second;
                return true;
            }
            auto alias = aliases_.find(*n);
            if (alias == aliases_.end()) return false;
            n = &alias->second;
        }
        return false;
    };
    std::vector<uint32_t> codes(keys_.size(), 0);
    std::vector<bool> valid(keys_.size(), false);
    uint32_t maxCode = 0;
    for (size_t i = 0; i < keys_.size(); ++i) {
        valid[i] = keycodeOf(keys_[i].name, codes[i]);
        if (valid[i]) maxCode = std::max(maxCode, codes[i]);
    }
    // Two names for one keycode: the later key wins, as libxkbcommon merges them.
    std::vector<int32_t> byCode(maxCode + 1, -1);
    for (size_t i = 0; i < keys_.size(); ++i)
        if (valid[i]) byCode[codes[i]] = static_cast<int32_t>(i);

    // The modifier map.
    std::vector<uint32_t> modmap(keys_.size(), 0);
    for (const ModMapEntry& e : modmap_) {
        if (e.isKey) {
            uint32_t code = 0;
            if (keycodeOf(e.key, code) && code < byCode.size() && byCode[code] >= 0)
                modmap[static_cast<size_t>(byCode[code])] |= 1u << e.mod;
        } else if (e.keysym != 0) {
            // The key with the lowest keycode that has the keysym.
            for (uint32_t code = 0; code < byCode.size(); ++code) {
                if (byCode[code] < 0) continue;
                const RawKey& key = keys_[static_cast<size_t>(byCode[code])];
                bool has = false;
                for (const RawGroup& g : key.groups)
                    for (uint32_t s : g.levels) has = has || s == e.keysym;
                if (has) {
                    modmap[static_cast<size_t>(byCode[code])] |= 1u << e.mod;
                    break;
                }
            }
        }
    }

    // Interprets from the most specific to the least: keysym ones before Any, then by predicate.
    std::stable_sort(interprets_.begin(), interprets_.end(), [](const Interpret& a, const Interpret& b) {
        if (a.any != b.any) return !a.any;
        return static_cast<int>(a.match) < static_cast<int>(b.match);
    });

    // Repeat and virtual modifiers from the interprets.
    std::vector<uint32_t> vmodmap(keys_.size(), 0);
    std::vector<bool> repeats(keys_.size(), false);
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (!valid[i] || byCode[codes[i]] != static_cast<int32_t>(i)) continue;
        const RawKey& key = keys_[i];
        uint32_t vm = 0;
        bool rep = false;
        if (!key.explicitActions) {
            for (uint32_t g = 0; g < key.groups.size(); ++g) {
                for (uint32_t l = 0; l < key.groups[g].levels.size(); ++l) {
                    const Interpret* interp = FindInterpret(key, modmap[i], g, l);
                    if (!interp) continue;
                    if (g == 0 && l == 0 && interp->repeat) rep = true;
                    if (((g == 0 && l == 0) || !interp->levelOneOnly) && interp->vmod >= 0)
                        vm |= 1u << interp->vmod;
                }
            }
        }
        vmodmap[i] = key.explicitVmods ? key.vmods : vm;
        repeats[i] = key.repeat >= 0 ? key.repeat == 1 : rep;
    }

    // Virtual modifier mappings: explicit, else the real modifiers of the keys that bind them,
    // else the usual mapping.
    for (size_t m = kNumRealMods; m < modNames_.size(); ++m) {
        if (explicitMapping_[m]) continue;
        uint32_t mask = 0;
        for (size_t i = 0; i < keys_.size(); ++i)
            if (vmodmap[i] & (1u << m)) mask |= modmap[i];
        if (mask == 0) {
            const std::string& n = modNames_[m];
            if (n == "LevelThree") mask = 0x80;
            else if (n == "NumLock") mask = 0x10;
            else if (n == "Alt") mask = kMod1;
            else if (n == "Super") mask = kMod4;
            else if (n == "LevelFive") mask = 0x20;
        }
        mapping_[m] = mask;
    }

    // Types.
    if (types_.empty()) {
        RawType one;
        one.name = "ONE_LEVEL";
        types_.push_back(one);
    }
    data->types.reserve(types_.size());
    for (const RawType& raw : types_) {
        Keymap::Data::Type type;
        type.mask = Resolve(raw.mods);
        for (const RawEntry& e : raw.entries) {
            Keymap::Data::Entry entry;
            entry.mask = Resolve(e.mods);
            entry.preserve = Resolve(e.preserve);
            entry.level = e.level;
            entry.active = e.mods == 0 || entry.mask != 0;
            type.entries.push_back(entry);
        }
        data->types.push_back(std::move(type));
    }
    auto typeIndex = [&](const std::string& name) -> uint32_t {
        for (size_t i = 0; i < types_.size(); ++i)
            if (types_[i].name == name) return static_cast<uint32_t>(i);
        return 0;  // libxkbcommon falls back on the first type
    };
    auto automaticType = [](const std::vector<uint32_t>& levels) -> const char* {
        const size_t width = levels.size();
        auto sym = [&](size_t i) { return i < width && levels[i] != kSeveralKeysyms ? levels[i] : 0u; };
        if (width <= 1) return "ONE_LEVEL";
        if (width == 2) {
            if (IsLowerKeysym(sym(0)) && IsUpperKeysym(sym(1))) return "ALPHABETIC";
            if (IsKeypadKeysym(sym(0)) || IsKeypadKeysym(sym(1))) return "KEYPAD";
            return "TWO_LEVEL";
        }
        if (width <= 4) {
            if (IsLowerKeysym(sym(0)) && IsUpperKeysym(sym(1))) {
                if (IsLowerKeysym(sym(2)) && IsUpperKeysym(sym(3))) return "FOUR_LEVEL_ALPHABETIC";
                return "FOUR_LEVEL_SEMIALPHABETIC";
            }
            if (IsKeypadKeysym(sym(0)) || IsKeypadKeysym(sym(1))) return "FOUR_LEVEL_KEYPAD";
            return "FOUR_LEVEL";
        }
        return nullptr;
    };

    // Keys.
    data->keyByCode.assign(byCode.size(), -1);
    for (uint32_t code = 0; code < byCode.size(); ++code) {
        if (byCode[code] < 0) continue;
        const size_t i = static_cast<size_t>(byCode[code]);
        const RawKey& raw = keys_[i];
        Keymap::Data::KeyInfo key;
        key.repeats = repeats[i];
        key.outOfRange = raw.outOfRange;
        key.redirect = raw.redirect;
        size_t numGroups = 0;
        for (size_t g = 0; g < raw.groups.size(); ++g)
            if (raw.groups[g].defined || raw.groups[g].hasType) numGroups = g + 1;
        for (size_t g = 0; g < numGroups; ++g) {
            const RawGroup& rg = raw.groups[g];
            Keymap::Data::Group group;
            if (rg.hasType) {
                group.type = typeIndex(rg.type);
            } else if (raw.hasDefaultType) {
                group.type = typeIndex(raw.defaultType);
            } else {
                const char* name = automaticType(rg.levels);
                group.type = name ? typeIndex(name) : 0;
            }
            group.levels = rg.levels;
            const uint32_t numLevels = types_[group.type].numLevels;
            if (group.levels.size() > numLevels) group.levels.resize(numLevels);
            key.groups.push_back(std::move(group));
        }
        data->keyByCode[code] = static_cast<int32_t>(data->keys.size());
        data->numGroups = std::max<uint32_t>(data->numGroups, static_cast<uint32_t>(key.groups.size()));
        data->keys.push_back(std::move(key));
    }

    for (size_t m = kNumRealMods; m < modNames_.size(); ++m) {
        if (modNames_[m] == "Alt" && mapping_[m]) data->altMask = mapping_[m];
        if (modNames_[m] == "Super" && mapping_[m]) data->superMask = mapping_[m];
    }
    return data;
}

// The US key of an evdev code, for keys whose layouts have no Latin letter or digit.
Key UsKey(uint32_t evdev) {
    static const Key kRow1[] = {Key::Q, Key::W, Key::E, Key::R, Key::T, Key::Y, Key::U, Key::I, Key::O, Key::P};
    static const Key kRow2[] = {Key::A, Key::S, Key::D, Key::F, Key::G, Key::H, Key::J, Key::K, Key::L};
    static const Key kRow3[] = {Key::Z, Key::X, Key::C, Key::V, Key::B, Key::N, Key::M};
    if (evdev >= 2 && evdev <= 10) return static_cast<Key>(static_cast<int>(Key::Digit1) + static_cast<int>(evdev - 2));
    if (evdev == 11) return Key::Digit0;
    if (evdev >= 16 && evdev <= 25) return kRow1[evdev - 16];
    if (evdev >= 30 && evdev <= 38) return kRow2[evdev - 30];
    if (evdev >= 44 && evdev <= 50) return kRow3[evdev - 44];
    switch (evdev) {
    case 1: return Key::Escape;
    case 12: return Key::Minus;
    case 13: return Key::Equal;
    case 14: return Key::Backspace;
    case 15: return Key::Tab;
    case 28: return Key::Enter;
    case 51: return Key::Comma;
    case 53: return Key::Slash;
    case 57: return Key::Space;
    default: return Key::None;
    }
}

Key LatinKey(uint32_t keysym) {
    if (keysym >= 'a' && keysym <= 'z') return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(keysym - 'a'));
    if (keysym >= 'A' && keysym <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + static_cast<int>(keysym - 'A'));
    if (keysym >= '0' && keysym <= '9') return static_cast<Key>(static_cast<int>(Key::Digit0) + static_cast<int>(keysym - '0'));
    return Key::None;
}

Key NamedKey(uint32_t keysym) {
    if (keysym >= 0xffbe && keysym <= 0xffc9) return static_cast<Key>(static_cast<int>(Key::F1) + static_cast<int>(keysym - 0xffbe));
    switch (keysym) {
    case 0xff0d: case 0xff8d: return Key::Enter;         // Return, KP_Enter
    case 0xff1b: return Key::Escape;
    case 0xff09: case 0xfe20: return Key::Tab;           // Tab, ISO_Left_Tab
    case 0xff08: return Key::Backspace;
    case 0xffff: case 0xff9f: return Key::Delete;        // Delete, KP_Delete
    case 0xff51: case 0xff96: return Key::Left;
    case 0xff53: case 0xff98: return Key::Right;
    case 0xff52: case 0xff97: return Key::Up;
    case 0xff54: case 0xff99: return Key::Down;
    case 0xff50: case 0xff95: return Key::Home;
    case 0xff57: case 0xff9c: return Key::End;
    case 0xff55: case 0xff9a: return Key::PageUp;        // Prior
    case 0xff56: case 0xff9b: return Key::PageDown;      // Next
    case 0xff63: case 0xff9e: return Key::Insert;
    case '=': case 0xffbd: return Key::Equal;            // equal, KP_Equal
    case '-': case 0xffad: return Key::Minus;            // minus, KP_Subtract
    case '+': case 0xffab: return Key::Plus;             // plus, KP_Add
    case ',': return Key::Comma;
    case '/': case 0xffaf: return Key::Slash;            // slash, KP_Divide
    case ' ': return Key::Space;
    default: return Key::None;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------

uint32_t KeysymToCodepoint(uint32_t keysym) {
    const uint32_t cp = RawCodepoint(keysym);
    if (cp < 0x20 || cp == 0x7f || (cp >= 0x80 && cp < 0xa0)) return 0;
    return cp;
}

uint32_t KeysymFromName(std::string_view name) {
    if (name.empty() || name.size() > 64) return 0;
    const KeysymName* end = kKeysymNames + sizeof(kKeysymNames) / sizeof(kKeysymNames[0]);
    const KeysymName* it = std::lower_bound(kKeysymNames, end, name, [](const KeysymName& e, std::string_view n) {
        return std::string_view(e.name) < n;
    });
    if (it != end && std::string_view(it->name) == name) return it->value;
    uint32_t v = 0;
    if (name[0] == 'U' && ParseHex(name.substr(1), 0x10ffff, v)) {
        if (v < 0x20 || (v > 0x7e && v < 0xa0)) return 0;
        return v < 0x100 ? v : kUnicodeOffset + v;
    }
    if (name.size() > 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X') &&
        ParseHex(name.substr(2), kKeysymMax, v))
        return v;
    return 0;
}

Keymap::Keymap(std::unique_ptr<Data> data) : data_(std::move(data)) {}
Keymap::~Keymap() = default;

std::unique_ptr<Keymap> Keymap::Parse(std::string_view text, std::string* error) {
    if (error) error->clear();
    while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
    if (text.size() > kMaxText) {
        if (error) *error = "keymap too large";
        return nullptr;
    }
    std::vector<Token> tokens;
    if (!Lex(text, tokens, error)) return nullptr;
    Builder builder(tokens, error);
    if (!builder.ParseKeymap()) return nullptr;
    std::unique_ptr<Data> data = builder.Finish();
    if (data->keys.empty()) {
        if (error) *error = "the keymap has no keys";
        return nullptr;
    }
    return std::unique_ptr<Keymap>(new Keymap(std::move(data)));
}

void Keymap::UpdateModifiers(uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    mods_ = depressed | latched | locked;
    group_ = group;
}

uint32_t Keymap::KeySym(uint32_t evdevKey) const {
    return KeySymAt(evdevKey, group_, mods_);
}

uint32_t Keymap::KeySymAt(uint32_t evdevKey, uint32_t group, uint32_t mods) const {
    if (evdevKey > kMaxKeycode) return 0;
    return data_->Lookup(evdevKey + 8, group, mods);
}

bool Keymap::Repeats(uint32_t evdevKey) const {
    if (evdevKey > kMaxKeycode) return false;
    const Data::KeyInfo* key = data_->Find(evdevKey + 8);
    return key && key->repeats;
}

Key Keymap::ShortcutKey(uint32_t evdevKey) const {
    if (evdevKey > kMaxKeycode) return Key::None;
    const Data::KeyInfo* key = data_->Find(evdevKey + 8);
    if (key) {
        const int g = data_->EffectiveGroup(*key, group_);
        if (g >= 0) {
            const uint32_t sym = data_->LevelOneSym(*key, g);
            Key k = LatinKey(sym);
            if (k == Key::None) k = NamedKey(sym);
            if (k != Key::None) return k;
        }
        for (size_t i = 0; i < key->groups.size(); ++i) {
            const Key k = LatinKey(data_->LevelOneSym(*key, static_cast<int>(i)));
            if (k != Key::None) return k;
        }
    }
    return UsKey(evdevKey);
}

uint8_t Keymap::Modifiers() const {
    uint8_t m = 0;
    if (mods_ & kControl) m |= kModCtrl;
    if (mods_ & kShift) m |= kModShift;
    if (mods_ & data_->altMask) m |= kModAlt;
    if (mods_ & data_->superMask) m |= kModSuper;
    return m;
}

} // namespace rgui::xkb
