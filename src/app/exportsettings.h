// SPDX-License-Identifier: Apache-2.0
// The export options the user picks in the options panel, kept in the settings file under
// `export.<name>`, and the engine's PdfOptions made from them.
#pragma once

#include "rayomd/tiny_pdf.h"

#include <string>

namespace rgui {

namespace ui {
class SettingsFile;
}

struct ExportSettings {
    // Dropdown indices, in the order of the item names below.
    enum Style : int { kElegant, kModern, kTech, kStyleCount };
    enum Margin : int { kCompact, kNormal, kWide, kCustomMargin, kMarginCount };
    enum PageSize : int { kA4, kA3, kA5, kLetter, kLegal, kCustomPage, kPageSizeCount };
    // The index is the engine's PdfOptions::pageBreakBeforeLevel.
    enum PageBreak : int { kNoPageBreak, kBreakBeforeH1, kBreakBeforeH2, kBreakBeforeH3, kPageBreakCount };
    enum Dialect : int { kGitHub, kObsidian, kDialectCount };
    enum Footnotes : int { kFootnotesEnd, kFootnotesPage, kFootnotesCount };
    // Obsidian's `![[note]]` alone on a line: the note printed in its place, or a link to it.
    enum Embeds : int { kEmbedContent, kEmbedLink, kEmbedsCount };
    // Export: the note alone, or a book of it and the notes it links to, this many links deep.
    enum LinkedNotes : int { kLinkedOff, kLinked1, kLinked2, kLinked3, kLinkedNotesCount };
    static const char* const kStyleNames[kStyleCount];        // Elegant, Modern, Tech
    static const char* const kMarginNames[kMarginCount];      // Compact, Normal, Wide, Custom
    static const char* const kPageSizeNames[kPageSizeCount];  // A4, A3, A5, Letter, Legal, Custom
    static const char* const kPageBreakNames[kPageBreakCount];  // None, Before H1, ... H1-H2, ... H1-H3
    static const char* const kDialectNames[kDialectCount];      // GitHub, Obsidian
    static const char* const kFootnotesNames[kFootnotesCount];  // End of document, Foot of page
    static const char* const kEmbedsNames[kEmbedsCount];        // Content, Link
    static const char* const kLinkedNotesNames[kLinkedNotesCount];  // Off, 1, 2, 3

    static constexpr double kMinMarginInches = 0.25;
    static constexpr double kMaxMarginInches = 2.0;
    static constexpr double kMinPageMm = 50.8;    // 144 pt, the engine's smallest side
    static constexpr double kMaxPageMm = 5080.0;  // 14,400 pt, its largest
    static constexpr int kMinTocDepth = 1;
    static constexpr int kMaxTocDepth = 6;
    static constexpr int kMinNumberDepth = 1;
    static constexpr int kMaxNumberDepth = 6;
    static constexpr size_t kMaxThemeFileBytes = 1024 * 1024;

    // Stored: export.<name>.
    int style = kElegant;
    int margin = kNormal;
    double customMarginInches = 1.0;
    int pageSize = kA4;
    bool landscape = false;  // not with autoHeight
    double customWidthMm = 210.0;
    double customHeightMm = 297.0;
    // The custom page size only: one long page for screens, its height grown to the content
    // (PdfPageSize::autoHeight); customHeightMm is not used then.
    bool autoHeight = false;
    bool toc = false;
    int tocDepth = 3;
    bool numberHeadings = false;  // 1, 1.1, 1.1.1 before the headings, numberDepth levels deep
    int numberDepth = 3;
    int pageBreakBefore = kNoPageBreak;
    int dialect = kGitHub;  // the Markdown the document is written in; Obsidian also completes `[[`
    int footnotes = kFootnotesEnd;
    int embeds = kEmbedContent;  // Obsidian's dialect only
    bool hideTags = false;       // Obsidian's dialect only: its #tags left out
    // UTF-8 folder the notes a document embeds or links to are found in (PdfOptions::vaultRoot);
    // empty for the document's folder.
    std::string vaultFolder;
    int linkedNotes = kLinkedOff;  // export only
    bool backlinks = false;        // export only, with linkedNotes: "Linked from:" ends each note
    bool pageNumbers = false;
    bool compress = false;
    bool fastImages = false;
    bool pdfa = false;
    bool pdfua = false;
    bool blackText = false;  // for printing: black text, fills in light greys
    bool embedSource = false;
    bool highlightCode = true;
    bool diagrams = true;    // ```mermaid blocks drawn as diagrams; false shows their source
    bool urlImages = false;  // Windows only: the engine has no URL images on Linux
    std::string themeFile;   // UTF-8 path of a PDF theme file; empty for none
    bool openAfterExport = true;

    // Not stored.
    // Platform::ImageCacheDir() as UTF-8, set by the app; empty keeps no image cache.
    std::string imageCacheDirectory;
    // Why themeFile could not be used (`cannot read <path>`, or ParseTheme's error); empty when
    // it was read, or there is none.
    std::string themeError;
    // themeFile parsed by the last ReloadTheme; the default (no theme) without one or on error.
    TinyPdf::PdfTheme theme;

    // Replaces the stored fields with the file's values: missing or unparsable ones get their
    // defaults, numbers are clamped to their ranges (an index out of range gets its default).
    // Then ReloadTheme.
    void Load(const ui::SettingsFile& file);
    // Writes every stored field to `file`.
    void Store(ui::SettingsFile& file) const;
    // Reads themeFile (at most kMaxThemeFileBytes) and parses it with the engine's theme reader;
    // sets `theme` and `themeError`. Call it after changing themeFile, and before a build to
    // pick up edits of the file. False when the file cannot be read or parsed.
    bool ReloadTheme();

    // The engine's options for a document at `sourcePath` (UTF-8; relative images resolve from
    // it). A preview never compresses and never embeds the source. `backlinks` is set only with
    // linked notes, whose book (LinkDepth) BuildBookPdf exports.
    TinyPdf::PdfOptions ToPdfOptions(const std::string& sourcePath, bool forPreview) const;
    // How many links deep an export follows (ReadLinkedBook); 0 exports the note alone.
    int LinkDepth() const { return linkedNotes; }
};

} // namespace rgui
