// SPDX-License-Identifier: Apache-2.0
#include "app/exportsettings.h"

#include "app/safefile.h"
#include "core/export_options.h"
#include "ui/settingsfile.h"

#include <filesystem>
#include <string_view>
#include <utility>

namespace rgui {
namespace {

constexpr std::string_view kPrefix = "export.";

std::string Key(std::string_view name) {
    std::string key(kPrefix);
    key += name;
    return key;
}

int Index(const ui::SettingsFile& file, std::string_view name, int fallback, int count) {
    const int value = file.GetInt(Key(name), fallback);
    return value >= 0 && value < count ? value : fallback;
}

int ClampInt(const ui::SettingsFile& file, std::string_view name, int fallback, int lo, int hi) {
    const int value = file.GetInt(Key(name), fallback);
    return value < lo ? lo : value > hi ? hi : value;
}

double ClampDouble(const ui::SettingsFile& file, std::string_view name, double fallback, double lo, double hi) {
    const double value = file.GetDouble(Key(name), fallback);
    return value < lo ? lo : value > hi ? hi : value;
}

bool Bool(const ui::SettingsFile& file, std::string_view name, bool fallback) {
    return file.GetBool(Key(name), fallback);
}

constexpr double kPointsPerMm = 72.0 / 25.4;
constexpr double kSqrt2 = 1.4142135623730951;

} // namespace

const char* const ExportSettings::kStyleNames[kStyleCount] = {"Elegant", "Modern", "Tech"};
const char* const ExportSettings::kMarginNames[kMarginCount] = {"Compact", "Normal", "Wide", "Custom"};
const char* const ExportSettings::kPageSizeNames[kPageSizeCount] = {"A4", "A3", "A5", "Letter", "Legal", "Custom"};
const char* const ExportSettings::kPageBreakNames[kPageBreakCount] = {"None", "Before H1", "Before H1\xE2\x80\x93H2",
                                                                      "Before H1\xE2\x80\x93H3"};
const char* const ExportSettings::kDialectNames[kDialectCount] = {"GitHub", "Obsidian"};
const char* const ExportSettings::kFootnotesNames[kFootnotesCount] = {"End of document", "Foot of page"};
const char* const ExportSettings::kEmbedsNames[kEmbedsCount] = {"Content", "Link"};
const char* const ExportSettings::kLinkedNotesNames[kLinkedNotesCount] = {"Off", "1 link deep", "2 links deep",
                                                                          "3 links deep"};

void ExportSettings::Load(const ui::SettingsFile& file) {
    const ExportSettings defaults;
    style = Index(file, "style", defaults.style, kStyleCount);
    margin = Index(file, "margin", defaults.margin, kMarginCount);
    customMarginInches = ClampDouble(file, "customMarginInches", defaults.customMarginInches, kMinMarginInches,
                                     kMaxMarginInches);
    pageSize = Index(file, "pageSize", defaults.pageSize, kPageSizeCount);
    landscape = Bool(file, "landscape", defaults.landscape);
    customWidthMm = ClampDouble(file, "customWidthMm", defaults.customWidthMm, kMinPageMm, kMaxPageMm);
    customHeightMm = ClampDouble(file, "customHeightMm", defaults.customHeightMm, kMinPageMm, kMaxPageMm);
    autoHeight = Bool(file, "autoHeight", defaults.autoHeight);
    toc = Bool(file, "toc", defaults.toc);
    tocDepth = ClampInt(file, "tocDepth", defaults.tocDepth, kMinTocDepth, kMaxTocDepth);
    numberHeadings = Bool(file, "numberHeadings", defaults.numberHeadings);
    numberDepth = ClampInt(file, "numberDepth", defaults.numberDepth, kMinNumberDepth, kMaxNumberDepth);
    pageBreakBefore = Index(file, "pageBreakBefore", defaults.pageBreakBefore, kPageBreakCount);
    dialect = Index(file, "dialect", defaults.dialect, kDialectCount);
    footnotes = Index(file, "footnotes", defaults.footnotes, kFootnotesCount);
    embeds = Index(file, "embeds", defaults.embeds, kEmbedsCount);
    vaultFolder = file.GetString(Key("vaultFolder"));
    linkedNotes = Index(file, "linkedNotes", defaults.linkedNotes, kLinkedNotesCount);
    backlinks = Bool(file, "backlinks", defaults.backlinks);
    pageNumbers = Bool(file, "pageNumbers", defaults.pageNumbers);
    compress = Bool(file, "compress", defaults.compress);
    fastImages = Bool(file, "fastImages", defaults.fastImages);
    pdfa = Bool(file, "pdfa", defaults.pdfa);
    pdfua = Bool(file, "pdfua", defaults.pdfua);
    embedSource = Bool(file, "embedSource", defaults.embedSource);
    highlightCode = Bool(file, "highlightCode", defaults.highlightCode);
    diagrams = Bool(file, "diagrams", defaults.diagrams);
    urlImages = Bool(file, "urlImages", defaults.urlImages);
    themeFile = file.GetString(Key("themeFile"));
    openAfterExport = Bool(file, "openAfterExport", defaults.openAfterExport);
    ReloadTheme();
}

void ExportSettings::Store(ui::SettingsFile& file) const {
    file.SetInt(Key("style"), style);
    file.SetInt(Key("margin"), margin);
    file.SetDouble(Key("customMarginInches"), customMarginInches);
    file.SetInt(Key("pageSize"), pageSize);
    file.SetBool(Key("landscape"), landscape);
    file.SetDouble(Key("customWidthMm"), customWidthMm);
    file.SetDouble(Key("customHeightMm"), customHeightMm);
    file.SetBool(Key("autoHeight"), autoHeight);
    file.SetBool(Key("toc"), toc);
    file.SetInt(Key("tocDepth"), tocDepth);
    file.SetBool(Key("numberHeadings"), numberHeadings);
    file.SetInt(Key("numberDepth"), numberDepth);
    file.SetInt(Key("pageBreakBefore"), pageBreakBefore);
    file.SetInt(Key("dialect"), dialect);
    file.SetInt(Key("footnotes"), footnotes);
    file.SetInt(Key("embeds"), embeds);
    file.SetString(Key("vaultFolder"), vaultFolder);
    file.SetInt(Key("linkedNotes"), linkedNotes);
    file.SetBool(Key("backlinks"), backlinks);
    file.SetBool(Key("pageNumbers"), pageNumbers);
    file.SetBool(Key("compress"), compress);
    file.SetBool(Key("fastImages"), fastImages);
    file.SetBool(Key("pdfa"), pdfa);
    file.SetBool(Key("pdfua"), pdfua);
    file.SetBool(Key("embedSource"), embedSource);
    file.SetBool(Key("highlightCode"), highlightCode);
    file.SetBool(Key("diagrams"), diagrams);
    file.SetBool(Key("urlImages"), urlImages);
    file.SetString(Key("themeFile"), themeFile);
    file.SetBool(Key("openAfterExport"), openAfterExport);
}

bool ExportSettings::ReloadTheme() {
    theme = TinyPdf::PdfTheme();
    themeError.clear();
    if (themeFile.empty()) return true;
    const std::filesystem::path path = std::filesystem::u8path(themeFile);
    std::string text;
    if (!ReadRegularFile(path, kMaxThemeFileBytes, text, nullptr)) {
        themeError = "cannot read " + themeFile;
        return false;
    }
    TinyPdf::PdfTheme parsed;
    std::string error;
    if (!TinyPdf::Internal::ParseTheme(text, path.parent_path().u8string(), parsed, error)) {
        themeError = std::move(error);
        return false;
    }
    theme = std::move(parsed);
    return true;
}

TinyPdf::PdfOptions ExportSettings::ToPdfOptions(const std::string& sourcePath, bool forPreview) const {
    TinyPdf::PdfOptions options;
    options.style = style == kModern ? TinyPdf::PdfStyle::Modern
                  : style == kTech   ? TinyPdf::PdfStyle::Tech
                                     : TinyPdf::PdfStyle::Elegant;
    switch (margin) {
    case kCompact: options.margin = TinyPdf::PdfMargin::Compact(); break;
    case kWide: options.margin = TinyPdf::PdfMargin::Wide(); break;
    case kCustomMargin: options.margin = TinyPdf::PdfMargin::CustomPoints(customMarginInches * 72.0); break;
    default: options.margin = TinyPdf::PdfMargin::Normal(); break;
    }
    switch (pageSize) {
    case kA3: options.pageSize = TinyPdf::PdfPageSize::A3(); break;
    case kA5: options.pageSize = TinyPdf::PdfPageSize::A5(); break;
    case kLetter: options.pageSize = TinyPdf::PdfPageSize::Letter(); break;
    case kLegal: options.pageSize = TinyPdf::PdfPageSize::Legal(); break;
    case kCustomPage:
        options.pageSize = {customWidthMm * kPointsPerMm, customHeightMm * kPointsPerMm};
        if (autoHeight) {
            // As ParsePageSize puts it: what is fitted to "a page" fits width * sqrt(2).
            options.pageSize.height = options.pageSize.width * kSqrt2;
            options.pageSize.autoHeight = true;
        }
        break;
    default: options.pageSize = TinyPdf::PdfPageSize::A4(); break;
    }
    if (landscape && !options.pageSize.autoHeight) std::swap(options.pageSize.width, options.pageSize.height);
    options.sourcePath = sourcePath;
#ifdef _WIN32
    options.enableUrlImages = urlImages;
#else
    options.enableUrlImages = false;
#endif
    options.embedSource = forPreview ? false : embedSource;
    options.imageCacheDirectory = imageCacheDirectory;
    options.pageNumbers = pageNumbers;
    options.compress = forPreview ? false : compress;
    options.fastImages = fastImages;
    options.pdfa = pdfa;
    options.pdfua = pdfua;
    options.highlightCode = highlightCode;
    options.diagrams = diagrams;
    options.toc = toc;
    options.tocDepth = tocDepth;
    options.numberHeadings = numberHeadings ? numberDepth : 0;
    options.pageBreakBeforeLevel = pageBreakBefore;
    options.dialect = dialect == kObsidian ? TinyPdf::MarkdownDialect::Obsidian : TinyPdf::MarkdownDialect::GitHub;
    options.footnotes = footnotes == kFootnotesPage ? TinyPdf::FootnotePlacement::Page : TinyPdf::FootnotePlacement::End;
    options.embedsAsLinks = embeds == kEmbedLink;
    options.vaultRoot = vaultFolder;
    options.backlinks = !forPreview && backlinks && linkedNotes != kLinkedOff;
    options.theme = theme;
    return options;
}

} // namespace rgui
