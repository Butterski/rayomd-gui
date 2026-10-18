// SPDX-License-Identifier: Apache-2.0
#include "app/mainwindow.h"

#include "app/themeselect.h"
#include "pdf/reader.h"
#include "ui/settingsfile.h"
#include "ui/widgets.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace rgui {

namespace {

namespace fs = std::filesystem;
using ui::RectF;

constexpr float kPageMargin = 16;   // above the first page, below the last and between pages
constexpr float kFooterMargin = 12; // Omawrite's footer: 12 px from the edges, 12 px apart
constexpr float kIconHit = 28;
constexpr float kOptionsLabelWidth = 96;
constexpr double kFlashMs = 1200;   // how long the jump marker shows

constexpr const char* kEmbedTooltip =
    "Embeds the complete Markdown source, including comments, front matter and anything not visible on "
    "the pages; anyone with the PDF can read it.";

const ui::Rgba kWhite{255, 255, 255, 255};

std::vector<std::string> Names(const char* const* names, int count) {
    return std::vector<std::string>(names, names + count);
}

std::string Count(uint32_t n, const char* word) {
    return std::to_string(n) + " " + word + (n == 1 ? "" : "s");
}

const char* DialogTitle(FileDialog kind) {
    switch (kind) {
    case FileDialog::OpenDocument: return "Open Markdown or RayoMD PDF";
    case FileDialog::SaveMarkdown: return "Save Markdown";
    case FileDialog::ExportPdf: return "Export PDF";
    case FileDialog::ChooseTheme: return "Choose PDF theme file";
    case FileDialog::ChooseVault: return "Choose vault folder";
    }
    return "";
}

// The index of the ladder value nearest `value`.
template <size_t N>
int Nearest(const float (&ladder)[N], double value, int fallback) {
    if (!(value > 0)) return fallback;
    int best = fallback;
    double bestDistance = 1e300;
    for (size_t i = 0; i < N; i++) {
        const double d = ladder[i] > value ? ladder[i] - value : value - ladder[i];
        if (d < bestDistance) {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

std::string LowerExtension(const std::string& path) {
    std::string ext = fs::u8path(path).extension().u8string();
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

// `path` with the extension `.pdf` (any case) it has, else with `.pdf` appended.
std::string WithPdfExtension(std::string path) {
    if (LowerExtension(path) != ".pdf") path += ".pdf";
    return path;
}

// `value` within [lo, hi]; NaN gives lo.
double Within(double value, double lo, double hi) {
    if (!(value > lo)) return lo;
    if (!(value < hi)) return hi;
    return value;
}

} // namespace

MainWindow::MainWindow(Platform& platform, const text::FontIndex* fontIndex, Hooks hooks,
                       BuildWorker::BuildFunction build)
    : platform_(platform), hooks_(std::move(hooks)), link_(std::make_shared<Link>()) {
    link_->owner = this;
    editor_.SetOnChanged([this] {
        doc_.modified = true;
        MarkPreviewDirty();
    });
    export_.imageCacheDirectory = platform_.ImageCacheDir().u8string();
    worker_ = std::make_unique<BuildWorker>(
        platform_, [this](PreviewReady ready) { OnPreview(std::move(ready)); },
        [this](ExportFinished finished) { OnExport(std::move(finished)); }, std::move(build));
    // A note the preview embeds changed on disk: build it again.
    worker_->SetOnEmbedsChanged([this] { MarkPreviewDirty(); });
    preview_ = std::make_unique<PreviewPages>(platform_, fontIndex, [this] {
        if (hooks_.requestFrame) hooks_.requestFrame();
    });
    loader_ = std::make_unique<DocumentLoader>(
        platform_, [this](DocumentLoaded loaded) { OnLoaded(std::move(loaded)); },
        [this](ImageCopied copied) { OnImageCopied(std::move(copied)); },
        [this](NotesListed listed) { OnNotesListed(std::move(listed)); });
    SetOmarchyAvailable(platform_.OmarchyThemeDir().has_value());
}

MainWindow::~MainWindow() {
    link_->alive = false;
    if (budgetTimer_ != 0) platform_.CancelTimer(budgetTimer_);
    // The workers stop before the window they call back goes; the build in flight is cancelled.
    loader_.reset();
    preview_.reset();
    worker_.reset();
}

// ---- Settings ----

void MainWindow::LoadSettings(const ui::SettingsFile& file) {
    themeSetting_ = file.GetString("ui.theme", kThemeOmarchy);
    if (themeSetting_.empty()) themeSetting_ = kThemeOmarchy;
    editor_.SetZoomIndex(Nearest(ui::kEditorZoomLadder, file.GetDouble("ui.editorZoom", 1.0), ui::kEditorZoomDefault));
    previewZoom_ = Nearest(kPreviewZoomLadder, file.GetDouble("ui.previewZoom", 1.0), kPreviewZoomDefault);
    double ratio = file.GetDouble("ui.splitRatio", 0.5);
    if (!(ratio >= 0.2 && ratio <= 0.8)) ratio = ratio > 0.8 ? 0.8 : (ratio < 0.2 ? 0.2 : 0.5);   // NaN: 0.5
    splitRatio_ = static_cast<float>(ratio);
    optionsVisible_ = file.GetBool("ui.optionsVisible", false);
    syncScroll_ = file.GetBool("ui.syncScroll", true);
    lastDirectory_ = file.GetString("ui.lastDirectory");
    export_.Load(file);
    popup_.SetWikiLinks(export_.dialect == ExportSettings::kObsidian);
    export_.imageCacheDirectory = platform_.ImageCacheDir().u8string();
}

void MainWindow::StoreSettings(ui::SettingsFile& file) const {
    file.SetString("ui.theme", themeSetting_);
    file.SetDouble("ui.editorZoom", ui::kEditorZoomLadder[editor_.ZoomIndex()]);
    file.SetDouble("ui.previewZoom", kPreviewZoomLadder[previewZoom_]);
    file.SetDouble("ui.splitRatio", splitRatio_);
    file.SetBool("ui.optionsVisible", optionsVisible_);
    file.SetBool("ui.syncScroll", syncScroll_);
    if (lastDirectory_.empty() || !file.SetString("ui.lastDirectory", lastDirectory_)) file.Erase("ui.lastDirectory");
    export_.Store(file);
}

void MainWindow::SetOmarchyAvailable(bool available) {
    omarchy_ = available;
    themeItems_ = ThemeMenuItems(available);
}

void MainWindow::SetWikiLinks(bool on) {
    if (on == popup_.WikiLinks()) return;
    popup_.SetWikiLinks(on);
    if (!on) {
        popup_.SetHeadings({});
        popup_.SetNotes({});
        notesFolder_.clear();
    }
    // The next build collects the headings.
    MarkPreviewDirty();
}

void MainWindow::SetSyncScroll(bool on) {
    syncScroll_ = on;
    if (on) {
        leader_ = ScrollLeader::Editor;
        syncPreviewNow_ = true;
    }
    hooks_.settingsChanged();
    Dirty();
}

// ---- Status ----

void MainWindow::SetStatus(std::string text, StatusKind kind, bool fromPreview) {
    status_ = std::move(text);
    statusKind_ = kind;
    statusFromPreview_ = fromPreview;
    Dirty();
}

void MainWindow::SetNotice(std::string text) {
    notice_ = std::move(text);
    Dirty();
}

std::string MainWindow::ShownStatus(StatusKind* kind) const {
    StatusKind shownKind = statusKind_;
    std::string shown = status_;
    if (statusFromPreview_ || status_.empty()) {
        if (!export_.themeError.empty()) {
            shown = "PDF theme: " + export_.themeError;
            shownKind = StatusKind::Warning;
        } else if (!notice_.empty()) {
            shown = notice_;
            shownKind = StatusKind::Warning;
        }
    }
    if (kind) *kind = shownKind;
    return shown;
}

std::string MainWindow::Title() const {
    return (doc_.modified ? "* " : "") + doc_.displayName + " - RayoMD";
}

// ---- The frame ----

void MainWindow::Frame(ui::Context& ctx) {
    dirty_ = false;
    inFrame_ = true;
    RectF win = ctx.WindowRect();
    // Back in the window: the notes the preview embeds may have changed meanwhile.
    if (ctx.WindowFocused() != windowFocused_) {
        windowFocused_ = ctx.WindowFocused();
        worker_->SetFocused(windowFocused_);
    }
    const bool csd = platform_.NeedsClientDecorations();
    if (csd && !platform_.IsMaximized()) ResizeEdges(ctx);
    if (csd) Header(ctx, ui::CutTop(win, kHeaderHeight));
    Footer(ctx, ui::CutBottom(win, kFooterHeight));
    if (optionsVisible_) Options(ctx, ui::CutRight(win, kOptionsWidth));

    // Editor | splitter | preview. The panes keep clear of the splitter's 6 px grip, whose sides
    // take their colours.
    const ui::DerivedColors& colors = ctx.GetTokens().colors;
    bool splitMoved = false;
    const float split = ui::Splitter(ctx, "##split", win, splitRatio_, &splitMoved);
    if (splitMoved) hooks_.settingsChanged();
    const RectF editorRect{win.x, win.y, std::max(0.0f, split - 3 - win.x), win.h};
    const RectF previewRect{split + 4, win.y, std::max(0.0f, win.Right() - split - 4), win.h};
    ctx.FillRect({split - 3, win.y, 3, win.h}, colors.windowBackground);
    ctx.FillRect({split + 1, win.y, 3, win.h}, colors.previewBackground);

    // A double click on the preview last frame: the caret goes to its line, shown where the
    // click was unless the synced view shows it already.
    if (editorReveal_) {
        const ui::TextBuffer& buffer = editor_.Buffer();
        if (buffer.LineCount() > 0) {
            const size_t line =
                static_cast<size_t>(Within(editorReveal_->line, 0, static_cast<double>(buffer.LineCount() - 1)));
            // Before Select, which moves the view it holds (Frame takes it back).
            const float shown = editor_.ScrollY();
            editor_.Select(buffer.LineStart(line), buffer.LineStart(line));
            const float rowTop = editor_.ScrollYOfLine(static_cast<double>(line));
            const float view = editor_.ViewHeight();
            if (!(syncScroll_ && rowTop >= shown + 0.1f * view && rowTop + editor_.LineHeight() <= shown + 0.9f * view)) {
                editor_.ScrollTo(rowTop - (editorReveal_->viewY - ui::kEditorPadTop), true, ctx.NowMs());
            }
        }
        editorReveal_.reset();
        editorScrollNext_.reset();
    } else if (editorScrollNext_) {
        editor_.ScrollTo(*editorScrollNext_, false, ctx.NowMs());
        editorScrollNext_.reset();
    }
    if (focusEditor_) {
        ctx.SetFocus(ctx.MakeId("##texteditor"));
        focusEditor_ = false;
    }
    // The popup list at the caret takes its keys first; the editor keeps the focus.
    const bool editorKeys = ctx.IsFocused(editor_.WidgetId()) && !prompt_.open && !pathPrompt_.open;
    popup_.BeforeEditor(ctx, editor_, editorKeys);
    const int zoomBefore = editor_.ZoomIndex();
    editor_.Frame(ctx, editorRect);
    if (editor_.ZoomIndex() != zoomBefore) hooks_.settingsChanged();
    popup_.AfterEditor(ctx, editor_, editorRect, editorKeys);
    if (popup_.TakeWikiLinkOpened()) RefreshNoteNames();
    // Typing, a press, the wheel or keys in the editor: it leads. A press shows its line in the
    // preview, at the caret's height (the panes start at the same y).
    if (editor_.Revision() != revisionSeen_ || editor_.UserScrolled() || editor_.PressedThisFrame()) {
        leader_ = ScrollLeader::Editor;
    }
    if (editor_.PressedThisFrame() && !sourceMap_.Empty()) {
        const ui::TextBuffer& buffer = editor_.Buffer();
        previewReveal_ = PreviewReveal{
            sourceMap_.PageOfLine(buffer, static_cast<double>(buffer.LineOf(editor_.Caret()))), editor_.CaretRect().y};
    }
    if (syncScroll_ && leader_ == ScrollLeader::Editor && editor_.ScrollY() != lastEditorScroll_ && !sourceMap_.Empty()) {
        syncPreviewNow_ = true;
    }
    PreviewPane(ctx, previewRect);

    Prompts(ctx);
    Shortcuts(ctx);
    ctx.SetTitle(Title());
    inFrame_ = false;
    lastEditorScroll_ = editor_.ScrollY();
    revisionSeen_ = editor_.Revision();
    // This frame's edits are all in: build them.
    MaybeStartPreview();
    if (dirty_) ctx.RequestAnimation();
}

void MainWindow::ResizeEdges(ui::Context& ctx) {
    const float w = ctx.Width(), h = ctx.Height(), e = kResizeEdge, c = 2 * kResizeEdge;
    struct Zone {
        const char* id;
        RectF rect;
        Edge edge;
        Cursor cursor;
    };
    // Corners first: they take a press before the sides they overlap.
    const Zone zones[] = {
        {"##resize-tl", {0, 0, c, c}, Edge::TopLeft, Cursor::ResizeNWSE},
        {"##resize-tr", {w - c, 0, c, c}, Edge::TopRight, Cursor::ResizeNESW},
        {"##resize-bl", {0, h - c, c, c}, Edge::BottomLeft, Cursor::ResizeNESW},
        {"##resize-br", {w - c, h - c, c, c}, Edge::BottomRight, Cursor::ResizeNWSE},
        {"##resize-t", {0, 0, w, e}, Edge::Top, Cursor::ResizeV},
        {"##resize-b", {0, h - e, w, e}, Edge::Bottom, Cursor::ResizeV},
        {"##resize-l", {0, 0, e, h}, Edge::Left, Cursor::ResizeH},
        {"##resize-r", {w - e, 0, e, h}, Edge::Right, Cursor::ResizeH},
    };
    // Above the window's widgets, so they take no pointer in the edges.
    ctx.PushLayer(ui::kLayerPopup);
    for (const Zone& zone : zones) {
        const ui::Interaction in = ctx.Interact(ctx.MakeId(zone.id), zone.rect);
        if (in.hovered || in.held) ctx.SetCursor(zone.cursor);
        if (in.pressed) {
            platform_.StartResize(zone.edge);
            ctx.ClearActive();   // the compositor has the pointer now; no release may follow
        }
        ctx.Block(zone.rect);
    }
    ctx.PopLayer();
}

void MainWindow::Header(ui::Context& ctx, RectF rect) {
    const ui::Tokens& t = ctx.GetTokens();
    ctx.FillRect({rect.x, rect.Bottom() - 1, rect.w, 1}, t.colors.hairline);
    RectF row = rect;
    ui::CutRight(row, 4);
    auto button = [&](const char* id, ui::Icon icon) {
        const RectF strip = ui::CutRight(row, kIconHit);
        ui::CutRight(row, 4);
        return ui::IconButton(ctx, id, icon, {strip.x, rect.y + (rect.h - kIconHit) / 2, kIconHit, kIconHit}, {},
                              ui::IconStyle::Text);
    };
    if (button("##close", ui::Icon::Close)) RequestClose();
    if (button("##maximize", ui::Icon::Maximize)) platform_.ToggleMaximize();
    if (button("##minimize", ui::Icon::Minimize)) platform_.Minimize();
    // The rest drags the window; a double click maximizes.
    const ui::Interaction in = ctx.Interact(ctx.MakeId("##header"), row);
    if (in.pressed) {
        if (in.clickCount == 2) platform_.ToggleMaximize();
        else platform_.StartMove();
        ctx.ClearActive();
    }
    const float side = rect.Right() - row.Right();
    ctx.DrawTextInRect(Title(), {rect.x + side, rect.y, std::max(0.0f, rect.w - 2 * side), rect.h},
                       ctx.FontSize(t.font.body), t.colors.text, ui::Align::Center);
}

void MainWindow::Footer(ui::Context& ctx, RectF rect) {
    const ui::Tokens& t = ctx.GetTokens();
    ctx.FillRect({rect.x, rect.y, rect.w, 1}, t.colors.hairline);
    // 16 px icons 12 px apart from 12 px in: hit areas of 28 side by side.
    float x = rect.x + kFooterMargin - (kIconHit - 16) / 2;
    const float y = rect.y + (rect.h - kIconHit) / 2;
    auto next = [&] {
        const RectF r{x, y, kIconHit, kIconHit};
        x += kIconHit;
        return r;
    };
    if (ui::IconButton(ctx, "##open", ui::Icon::Open, next(), "Open (Ctrl+O)")) Guarded(Action::OpenDialog);
    if (ui::IconButton(ctx, "##save", ui::Icon::Save, next(), "Save (Ctrl+S)")) Save(false);
    if (ui::IconButton(ctx, "##export", ui::Icon::Export, next(), "Export PDF (Ctrl+E)", ui::IconStyle::Accent)) {
        ExportPdf(false);
    }
    if (ui::IconButton(ctx, "##insert", ui::Icon::Insert, next(), "Insert (Ctrl+I)")) {
        popup_.OpenInsert();
        focusEditor_ = true;
        Dirty();
    }
    if (ui::IconButton(ctx, "##options", ui::Icon::Options, next(), "Options (Ctrl+,)")) {
        optionsVisible_ = !optionsVisible_;
        hooks_.settingsChanged();
        Dirty();
    }
    const float statusX = x - (kIconHit - 16) / 2 + kFooterMargin;

    const float small = ctx.FontSize(t.font.bodySmall);
    const std::string counts = std::to_string(stats_.words) + " words \xC2\xB7 " + std::to_string(stats_.lines) + " lines";
    const float countsWidth = ctx.TextWidth(counts, small);
    const float countsX = rect.Right() - kFooterMargin - countsWidth;
    ctx.DrawTextInRect(counts, {countsX, rect.y, countsWidth, rect.h}, small, t.colors.mutedText);

    StatusKind kind = StatusKind::Info;
    const std::string status = ShownStatus(&kind);
    if (!status.empty()) {
        const ui::Rgba color = kind == StatusKind::Error     ? t.colors.error
                               : kind == StatusKind::Warning ? t.colors.warning
                                                             : t.colors.mutedText;
        ctx.DrawTextInRect(status, {statusX, rect.y, std::max(0.0f, countsX - kFooterMargin - statusX), rect.h}, small,
                           color);
    }
}

void MainWindow::Options(ui::Context& ctx, RectF rect) {
    const ui::Tokens& t = ctx.GetTokens();
    ctx.FillRect({rect.x, rect.y, 1, rect.h}, t.colors.hairline);
    const RectF viewport{rect.x + 1, rect.y, std::max(0.0f, rect.w - 1), rect.h};
    const float pad = static_cast<float>(t.spacing.panelPadding);
    const float offset = ui::BeginScrollArea(ctx, "##options", viewport);
    float height = 0;
    OptionsContent(ctx, {viewport.x + pad, viewport.y + pad - offset, std::max(0.0f, viewport.w - 2 * pad), 1e6f},
                   height);
    ui::EndScrollArea(ctx, height + 2 * pad);
}

void MainWindow::OptionsContent(ui::Context& ctx, RectF column, float& height) {
    const ui::Tokens& t = ctx.GetTokens();
    const float top = column.y;
    const float rowH = static_cast<float>(t.spacing.controlHeight);
    const float gap = static_cast<float>(t.spacing.rowGap);
    const float numberWidth = static_cast<float>(t.spacing.numberFieldWidth);
    bool changed = false;
    auto row = [&] {
        const RectF r = ui::CutTop(column, rowH);
        ui::CutTop(column, gap);
        return r;
    };
    auto labeled = [&](std::string_view label) {
        RectF r = row();
        ui::Label(ctx, label, ui::CutLeft(r, kOptionsLabelWidth));
        return r;
    };
    auto number = [&](RectF r) { return RectF{r.x, r.y, std::min(numberWidth, r.w), r.h}; };
    auto check = [&](const char* label, bool& value) { changed |= ui::Checkbox(ctx, label, value, row()); };

    static const std::vector<std::string> styles = Names(ExportSettings::kStyleNames, ExportSettings::kStyleCount);
    static const std::vector<std::string> margins = Names(ExportSettings::kMarginNames, ExportSettings::kMarginCount);
    static const std::vector<std::string> sizes = Names(ExportSettings::kPageSizeNames, ExportSettings::kPageSizeCount);
    static const std::vector<std::string> breaks = Names(ExportSettings::kPageBreakNames, ExportSettings::kPageBreakCount);
    static const std::vector<std::string> dialects = Names(ExportSettings::kDialectNames, ExportSettings::kDialectCount);
    static const std::vector<std::string> footnotes =
        Names(ExportSettings::kFootnotesNames, ExportSettings::kFootnotesCount);
    static const std::vector<std::string> embeds = Names(ExportSettings::kEmbedsNames, ExportSettings::kEmbedsCount);
    static const std::vector<std::string> linked =
        Names(ExportSettings::kLinkedNotesNames, ExportSettings::kLinkedNotesCount);
    bool exportOnly = false;  // an option the preview does not use changed
    const bool obsidian = export_.dialect == ExportSettings::kObsidian;

    // Document.
    ui::SectionHeader(ctx, "Document", column, true);
    if (ui::Dropdown(ctx, "##dialect", export_.dialect, dialects, labeled("Markdown"))) {
        SetWikiLinks(export_.dialect == ExportSettings::kObsidian);
        changed = true;
    }
    ctx.BeginDisabled(!obsidian);
    changed |= ui::Dropdown(ctx, "##embeds", export_.embeds, embeds, labeled("Embeds"));
    ctx.EndDisabled();
    // The vault: where embedded and linked notes are found (Obsidian's embeds, and linked notes
    // in either dialect).
    ctx.BeginDisabled(!obsidian && export_.linkedNotes == ExportSettings::kLinkedOff);
    {
        std::string name =
            export_.vaultFolder.empty() ? std::string() : fs::u8path(export_.vaultFolder).filename().u8string();
        if (name.empty()) name = export_.vaultFolder;
        ui::TextField(ctx, "##vaultFolder", name, labeled("Vault"), "Document's folder", true);
        RectF r = row();
        ui::CutLeft(r, kOptionsLabelWidth);
        const char* choose = "Choose\xE2\x80\xA6##vault";
        if (ui::Button(ctx, choose, ui::CutLeft(r, ui::ButtonWidth(ctx, choose)))) ChooseVaultFolder();
        ui::CutLeft(r, static_cast<float>(t.spacing.sm));
        if (!export_.vaultFolder.empty() &&
            ui::IconButton(ctx, "##clearVault", ui::Icon::Close, ui::CutLeft(r, kIconHit),
                           "Use the document's folder")) {
            export_.vaultFolder.clear();
            changed = true;
        }
    }
    ctx.EndDisabled();
    changed |= ui::Dropdown(ctx, "##style", export_.style, styles, labeled("Style"));
    changed |= ui::Dropdown(ctx, "##margin", export_.margin, margins, labeled("Margins"));
    if (export_.margin == ExportSettings::kCustomMargin) {
        changed |= ui::NumberField(ctx, "##marginInches", export_.customMarginInches, ExportSettings::kMinMarginInches,
                                   ExportSettings::kMaxMarginInches, 0.05, 2, " in", number(labeled("")));
    }
    changed |= ui::Dropdown(ctx, "##pageSize", export_.pageSize, sizes, labeled("Page size"));
    const bool custom = export_.pageSize == ExportSettings::kCustomPage;
    // One long page has no landscape.
    ctx.BeginDisabled(custom && export_.autoHeight);
    check("Landscape", export_.landscape);
    ctx.EndDisabled();
    if (custom) {
        changed |= ui::NumberField(ctx, "##pageWidth", export_.customWidthMm, ExportSettings::kMinPageMm,
                                   ExportSettings::kMaxPageMm, 1, 1, " mm", number(labeled("Width")));
        ctx.BeginDisabled(export_.autoHeight);
        changed |= ui::NumberField(ctx, "##pageHeight", export_.customHeightMm, ExportSettings::kMinPageMm,
                                   ExportSettings::kMaxPageMm, 1, 1, " mm", number(labeled("Height")));
        ctx.EndDisabled();
        check("Auto height", export_.autoHeight);
    }
    changed |= ui::Dropdown(ctx, "##footnotes", export_.footnotes, footnotes, labeled("Footnotes"));
    {
        std::string name = export_.themeFile.empty() ? std::string() : fs::u8path(export_.themeFile).filename().u8string();
        ui::TextField(ctx, "##themeFile", name, labeled("PDF theme"), "None", true);
        RectF r = row();
        ui::CutLeft(r, kOptionsLabelWidth);
        const char* choose = "Choose\xE2\x80\xA6##theme";
        if (ui::Button(ctx, choose, ui::CutLeft(r, ui::ButtonWidth(ctx, choose)))) ChooseThemeFile();
        ui::CutLeft(r, static_cast<float>(t.spacing.sm));
        if (!export_.themeFile.empty() &&
            ui::IconButton(ctx, "##clearTheme", ui::Icon::Close, ui::CutLeft(r, kIconHit), "Clear the PDF theme")) {
            export_.themeFile.clear();
            export_.themeError.clear();
            export_.theme = TinyPdf::PdfTheme();
            changed = true;
        }
        if (!export_.themeError.empty()) {
            const float size = ctx.FontSize(t.font.bodySmall);
            const float lineH = ctx.SnapToDevice(1.4f * size);
            for (std::string_view line : ui::WrapText(ctx, export_.themeError, size, column.w)) {
                ctx.DrawTextInRect(line, ui::CutTop(column, lineH), size, t.colors.error);
            }
            ui::CutTop(column, gap);
        }
    }

    // Contents.
    ui::SectionHeader(ctx, "Contents", column);
    check("Table of contents", export_.toc);
    ctx.BeginDisabled(!export_.toc);
    double depth = export_.tocDepth;
    if (ui::NumberField(ctx, "##tocDepth", depth, ExportSettings::kMinTocDepth, ExportSettings::kMaxTocDepth, 1, 0, "",
                        number(labeled("Depth")))) {
        export_.tocDepth = static_cast<int>(depth);
        changed = true;
    }
    ctx.EndDisabled();
    check("Number headings", export_.numberHeadings);
    ctx.BeginDisabled(!export_.numberHeadings);
    double numberDepth = export_.numberDepth;
    if (ui::NumberField(ctx, "##numberDepth", numberDepth, ExportSettings::kMinNumberDepth,
                        ExportSettings::kMaxNumberDepth, 1, 0, "", number(labeled("Depth")))) {
        export_.numberDepth = static_cast<int>(numberDepth);
        changed = true;
    }
    ctx.EndDisabled();
    changed |= ui::Dropdown(ctx, "##pageBreakBefore", export_.pageBreakBefore, breaks, labeled("Page breaks"));
    check("Page numbers", export_.pageNumbers);
    check("Highlight code", export_.highlightCode);
    check("Mermaid diagrams", export_.diagrams);

    // Output.
    ui::SectionHeader(ctx, "Output", column);
    check("PDF/A-3b", export_.pdfa);
    check("PDF/UA-1", export_.pdfua);
    check("Compress", export_.compress);
    check("Fast images", export_.fastImages);
    {
        const RectF r = row();
        changed |= ui::Checkbox(ctx, "Embed Markdown source", export_.embedSource, r);
        ui::Tooltip(ctx, ctx.MakeId("Embed Markdown source"), r, kEmbedTooltip);
    }
#ifdef _WIN32
    check("URL images", export_.urlImages);
#endif
    check("Open after export", export_.openAfterExport);
    // The export only: the preview stays the note itself.
    exportOnly |= ui::Dropdown(ctx, "##linkedNotes", export_.linkedNotes, linked, labeled("Linked notes"));
    ctx.BeginDisabled(export_.linkedNotes == ExportSettings::kLinkedOff);
    exportOnly |= ui::Checkbox(ctx, "Backlinks", export_.backlinks, row());
    ctx.EndDisabled();

    // Interface.
    ui::SectionHeader(ctx, "Interface", column);
    int theme = ThemeMenuIndex(themeSetting_);
    if (ui::Dropdown(ctx, "##theme", theme, themeItems_, labeled("Theme"))) {
        themeSetting_ = ThemeSettingOf(theme, omarchy_);
        hooks_.settingsChanged();
        if (hooks_.themeChanged) hooks_.themeChanged();
        Dirty();
    }
    bool sync = syncScroll_;
    if (ui::Checkbox(ctx, "Sync scrolling", sync, row())) SetSyncScroll(sync);

    height = column.y - top;
    if (changed) {
        OptionsChanged();
    } else if (exportOnly) {
        hooks_.settingsChanged();
        Dirty();
    }
}

void MainWindow::PreviewPane(ui::Context& ctx, RectF rect) {
    const ui::Tokens& t = ctx.GetTokens();
    ctx.FillRect(rect, t.colors.previewBackground);

    // Ctrl+wheel zooms, keeping the page at the top of the view where it is.
    float pixels = 0;
    previewZoomWheel_ += ctx.ConsumeWheel(rect, pixels, true) + pixels / ui::SmoothScroll::kNotchStep;
    int zoom = previewZoom_;
    while (previewZoomWheel_ <= -1) {
        zoom++;
        previewZoomWheel_ += 1;
    }
    while (previewZoomWheel_ >= 1) {
        zoom--;
        previewZoomWheel_ -= 1;
    }
    zoom = std::min(std::max(zoom, 0), kPreviewZoomCount - 1);
    if (zoom != previewZoom_) {
        if (!anchor_) anchor_ = ViewAnchor();
        previewZoom_ = zoom;
        hooks_.settingsChanged();
    }

    // The layout: pages centred, (pane width - 32) x zoom wide, 16 px around and between them.
    const int count = preview_->PageCount();
    const float pageW = std::max(16.0f, ctx.SnapToDevice((rect.w - 2 * kPageMargin) * kPreviewZoomLadder[previewZoom_]));
    pageTops_.resize(static_cast<size_t>(count));
    pageHeights_.resize(static_cast<size_t>(count));
    float y = kPageMargin;
    for (int i = 0; i < count; i++) {
        const double ratio = preview_->PageHeightPt(i) / preview_->PageWidthPt(i);
        const float h = std::max(1.0f, ctx.SnapToDevice(static_cast<float>(pageW * ratio)));
        pageTops_[static_cast<size_t>(i)] = y;
        pageHeights_[static_cast<size_t>(i)] = h;
        y += h + kPageMargin;
    }
    const float content = count > 0 ? y : 0;
    pageWidth_ = pageW;
    previewContent_ = content;
    previewView_ = rect.h;

    if (anchor_ && count > 0) {
        const int page = std::min(anchor_->page, count - 1);
        const size_t i = static_cast<size_t>(page);
        const float target = page == anchor_->page ? pageTops_[i] + anchor_->fraction * pageHeights_[i] : content;
        ui::SmoothScroll& scroll = ui::ScrollAreaScroll(ctx, "##preview");
        scroll.SetMax(content - rect.h);
        scroll.JumpTo(target);
    }
    anchor_.reset();

    // The source sync's moves, made before the scroll area reads the position, so that neither
    // counts as the user's (targetBefore): the editor's view at once, then a revealed line eased
    // into the view unless the synced view shows it.
    ui::SmoothScroll& scroll = ui::ScrollAreaScroll(ctx, "##preview");
    scroll.SetMax(content - rect.h);
    if (syncPreviewNow_) {
        if (!sourceMap_.Empty() && count > 0) scroll.JumpTo(PreviewOffsetForEditor());
        syncPreviewNow_ = false;
    }
    if (previewReveal_) {
        if (previewReveal_->page >= 0 && count > 0) {
            const float target = PreviewYOfPage(previewReveal_->page);
            const float shown = scroll.Target();
            if (!(syncScroll_ && target >= shown + 0.1f * rect.h && target <= shown + 0.9f * rect.h)) {
                scroll.EaseTo(target - previewReveal_->viewY, ctx.NowMs());
            }
            flashPage_ = previewReveal_->page;
            flashUntil_ = ctx.NowMs() + kFlashMs;
            ctx.WakeAt(flashUntil_);
        }
        previewReveal_.reset();
    }
    const float targetBefore = scroll.Target();

    const float offset = ui::BeginScrollArea(ctx, "##preview", rect, content);
    previewOffset_ = offset;
    const float pageX = ctx.SnapToDevice(rect.x + (rect.w - pageW) / 2);
    pageX_ = pageX;
    // The page of the jump marker, while it shows.
    const int flashIndex =
        ctx.NowMs() < flashUntil_ && count > 0 ? static_cast<int>(Within(flashPage_, 0, count - 1)) : -1;
    const int widthPx = std::max(1, static_cast<int>(pageW * ctx.Scale() + 0.5));
    // The rows of a page's image at widthPx in view, from its device rectangle: for a page out
    // of view, an empty range at its edge nearest the view.
    const gfx::RectI view = ctx.ToDevice(rect);
    const auto want = [&](int i, const gfx::RectI& d) {
        const int64_t height = std::max(0, d.Height()), width = std::max(1, d.Width());
        const auto row = [&](int at) {
            return static_cast<int>(std::min<int64_t>(std::max<int64_t>(at - d.y0, 0), height) * widthPx / width);
        };
        return PreviewPages::Want{i, row(view.y0), row(view.y1)};
    };
    std::vector<int> visible;
    wanted_.clear();
    for (int i = 0; i < count; i++) {
        const size_t k = static_cast<size_t>(i);
        const float top = rect.y - offset + pageTops_[k];
        if (top + pageHeights_[k] <= rect.y) continue;
        if (top >= rect.Bottom()) break;
        visible.push_back(i);
        const RectF page{pageX, top, pageW, pageHeights_[k]};
        ctx.StrokeRect({page.x - 1, page.y - 1, page.w + 2, page.h + 2}, 1, t.colors.hairline);
        ctx.FillRect(page, kWhite);
        if (const gfx::Image* image = preview_->Image(i)) ctx.DrawImage(*image, page);
        const gfx::RectI device = ctx.ToDevice(page);
        preview_->Strips(i, strips_);
        if (!strips_.empty()) DrawStrips(ctx, page, device, view);
        wanted_.push_back(want(i, device));
        if (i == flashIndex) {
            // A bar in the page's left margin beside the line, 16 points high (at least 12 px).
            const float at = static_cast<float>(Within(flashPage_ - i, 0, 1)) * pageHeights_[k];
            const float pointPx = static_cast<float>(pageW / preview_->PageWidthPt(i));
            ctx.FillRect({pageX + 6, top + at - 2, 4, std::max(12.0f, 16 * pointPx)}, t.colors.accent);
        }
    }
    ui::EndScrollArea(ctx, content);
    // The wheel or the thumb moved it: the preview leads.
    if (scroll.Target() != targetBefore) leader_ = ScrollLeader::Preview;
    // A double click on a page puts the editor's caret on the line shown there (the next frame).
    // After the scroll area, so that its thumb keeps its presses.
    const ui::Interaction pages = ctx.Interact(ctx.MakeId("##pages"), rect);
    if (pages.pressed && pages.clickCount == 2 && !sourceMap_.Empty()) {
        const ui::PointerInput& pointer = ctx.Pointer();
        for (const int i : visible) {
            const size_t k = static_cast<size_t>(i);
            const float top = rect.y - offset + pageTops_[k];
            if (!RectF{pageX, top, pageW, pageHeights_[k]}.Contains(pointer.pressX, pointer.pressY)) continue;
            const double page = i + (pointer.pressY - top) / pageHeights_[k];
            editorReveal_ = EditorReveal{sourceMap_.LineOfPage(editor_.Buffer(), page), pointer.pressY - rect.y};
            leader_ = ScrollLeader::Preview;
            focusEditor_ = true;
            Dirty();
            break;
        }
    }

    // The visible pages, the one nearest the middle of the view first, then their neighbours.
    const float middle = offset + rect.h / 2;
    std::stable_sort(wanted_.begin(), wanted_.end(), [&](const PreviewPages::Want& a, const PreviewPages::Want& b) {
        const auto distance = [&](int i) {
            const size_t k = static_cast<size_t>(i);
            const float center = pageTops_[k] + pageHeights_[k] / 2;
            return center > middle ? center - middle : middle - center;
        };
        return distance(a.page) < distance(b.page);
    });
    const size_t shown = wanted_.size();
    if (!visible.empty()) {
        const auto neighbour = [&](int i) {
            const size_t k = static_cast<size_t>(i);
            wanted_.push_back(want(i, ctx.ToDevice({pageX, rect.y - offset + pageTops_[k], pageW, pageHeights_[k]})));
        };
        for (int d = 1; d <= 2; d++) {
            if (visible.front() - d >= 0) neighbour(visible.front() - d);
            if (visible.back() + d < count) neighbour(visible.back() + d);
        }
    }
    preview_->Request(wanted_, widthPx);

    bool current = count > 0 && !visible.empty() && !previewDirty_ && !previewInFlight_ && !loading_ &&
                   shownGeneration_ == requestedGeneration_ && preview_->Generation() == shownGeneration_;
    for (size_t v = 0; v < shown; v++) {
        const PreviewPages::Want& w = wanted_[v];
        current = current && preview_->Current(w.page, widthPx, w.top, w.bottom);
    }
    settled_ = current;
    // The preview leads: the editor follows in the next frame.
    if (syncScroll_ && leader_ == ScrollLeader::Preview && !sourceMap_.Empty() && offset != lastPreviewOffset_) {
        editorScrollNext_ = EditorScrollForPreview(offset);
        Dirty();
    }
    lastPreviewOffset_ = offset;
}

void MainWindow::DrawStrips(ui::Context& ctx, RectF page, const gfx::RectI& device, const gfx::RectI& view) {
    // A strip's rows at its width map to device rows by the page's device width: at the width
    // drawn now one to one, so strips meet on whole pixels with no seam.
    const double scale = ctx.Scale();
    bool clipped = false;
    for (const PreviewPages::Strip& strip : strips_) {
        const int64_t across = device.Width();
        const int y0 = device.y0 + static_cast<int>(int64_t(strip.top) * across / strip.widthPx);
        const int y1 = device.y0 + static_cast<int>(int64_t(strip.top + strip.image->height) * across / strip.widthPx);
        if (y1 <= std::max(view.y0, device.y0) || y0 >= std::min(view.y1, device.y1)) continue;
        // The last strip of a taller page of an earlier build stops at the page's bottom.
        if (y1 > device.y1 && !clipped) {
            ctx.PushClip(page);
            clipped = true;
        }
        ctx.DrawImage(*strip.image, {page.x, static_cast<float>(y0 / scale), page.w, static_cast<float>((y1 - y0) / scale)});
    }
    if (clipped) ctx.PopClip();
}

void MainWindow::Prompts(ui::Context& ctx) {
    if (prompt_.open) {
        const char* when = prompt_.action == Action::Close ? "closing"
                           : prompt_.action == Action::New ? "starting a new document"
                                                           : "opening another file";
        const std::string body = "Save changes to " + doc_.displayName + " before " + when + "?";
        const ui::ModalButton buttons[] = {
            {"Save", true, Key::S, true, false},
            {"Discard", false, Key::D, false, false},
            {"Cancel", false, Key::C, false, true},
        };
        const int chosen = ui::MessageModal(ctx, "##unsaved", "Unsaved changes", body, buttons, 3);
        if (chosen >= 0) {
            const Action action = prompt_.action;
            const std::string path = prompt_.path;
            prompt_ = {};
            Dirty();
            if (chosen == 0) {
                Save(false, [this, action, path] { Proceed(action, path); });
            } else if (chosen == 1) {
                Proceed(action, path);
            }
        }
    }
    if (pathPrompt_.open) {
        const RectF body = ui::BeginModal(ctx, "##pathPrompt", pathPrompt_.title, ui::TextFieldHeight(ctx));
        if (pathPrompt_.focus) {
            ctx.SetFocus(ctx.MakeId("##path"));
            pathPrompt_.focus = false;
        }
        ui::TextField(ctx, "##path", pathPrompt_.text, body);
        const ui::ModalButton buttons[] = {
            {"OK", true, Key::None, true, false},
            {"Cancel", false, Key::None, false, true},
        };
        const int chosen = ui::EndModal(ctx, buttons, 2);
        if (chosen == 1 || (chosen == 0 && !pathPrompt_.text.empty())) {
            std::function<void(std::string)> onChosen = std::move(pathPrompt_.onChosen);
            std::string path = std::move(pathPrompt_.text);
            pathPrompt_ = {};
            focusEditor_ = true;
            Dirty();
            if (chosen == 0 && onChosen) onChosen(std::move(path));
        }
    }
}

void MainWindow::Shortcuts(ui::Context& ctx) {
    constexpr uint8_t ctrl = kModCtrl;
    constexpr uint8_t ctrlShift = kModCtrl | kModShift;
    // Exact modifiers: Ctrl+Alt (AltGr) never matches.
    if (ctx.ConsumeKey(Key::O, ctrl)) Guarded(Action::OpenDialog);
    if (ctx.ConsumeKey(Key::N, ctrl)) Guarded(Action::New);
    if (ctx.ConsumeKey(Key::S, ctrl)) Save(false);
    if (ctx.ConsumeKey(Key::S, ctrlShift)) Save(true);
    if (ctx.ConsumeKey(Key::E, ctrl)) ExportPdf(false);
    if (ctx.ConsumeKey(Key::E, ctrlShift)) ExportPdf(true);
    if (ctx.ConsumeKey(Key::Comma, ctrl)) {
        optionsVisible_ = !optionsVisible_;
        hooks_.settingsChanged();
        Dirty();
    }
    int zoom = editor_.ZoomIndex();
    if (ctx.ConsumeKey(Key::Equal, ctrl) || ctx.ConsumeKey(Key::Equal, ctrlShift) || ctx.ConsumeKey(Key::Plus, ctrl) ||
        ctx.ConsumeKey(Key::Plus, ctrlShift)) {
        zoom++;
    }
    if (ctx.ConsumeKey(Key::Minus, ctrl)) zoom--;
    if (ctx.ConsumeKey(Key::Digit0, ctrl)) {
        zoom = ui::kEditorZoomDefault;
        if (previewZoom_ != kPreviewZoomDefault) {
            previewZoom_ = kPreviewZoomDefault;
            hooks_.settingsChanged();
            Dirty();
        }
    }
    zoom = std::min(std::max(zoom, 0), ui::kEditorZoomCount - 1);
    if (zoom != editor_.ZoomIndex()) {
        editor_.SetZoomIndex(zoom);
        hooks_.settingsChanged();
        Dirty();
    }
    if (ctx.ConsumeKey(Key::Q, ctrl)) RequestClose();
}

// ---- Actions ----

void MainWindow::RequestClose() { Guarded(Action::Close); }

void MainWindow::DropFile(const std::string& path) {
    const std::string ext = LowerExtension(path);
    const std::string name = fs::u8path(path).filename().u8string();
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".svg") {
        if (doc_.filePath.empty()) {
            SetStatus("Save the document first: a dropped image goes into the images folder beside it",
                      StatusKind::Warning);
        } else if (loader_->CopyImage(path, doc_.filePath) == 0) {
            SetStatus("Cannot copy " + name + ": too many images are being copied", StatusKind::Warning);
        } else {
            SetStatus("Copying " + name + "\xE2\x80\xA6", StatusKind::Info);
        }
        if (hooks_.requestFrame) hooks_.requestFrame();
        return;
    }
    if (ext != ".md" && ext != ".markdown" && ext != ".txt" && ext != ".pdf") {
        SetStatus("Not a Markdown, text, PDF or image file: " + name, StatusKind::Warning);
        if (hooks_.requestFrame) hooks_.requestFrame();
        return;
    }
    Guarded(Action::OpenPath, path);
}

void MainWindow::Guarded(Action action, std::string path) {
    if (prompt_.open || pathPrompt_.open) return;
    if (doc_.modified) {
        prompt_.open = true;
        prompt_.action = action;
        prompt_.path = std::move(path);
        Dirty();
        if (hooks_.requestFrame) hooks_.requestFrame();
        return;
    }
    Proceed(action, path);
}

void MainWindow::Proceed(Action action, const std::string& path) {
    switch (action) {
    case Action::Close:
        if (hooks_.quit) hooks_.quit();
        break;
    case Action::OpenDialog:
        ShowOpenDialog();
        break;
    case Action::OpenPath:
        Open(path);
        break;
    case Action::New:
        NewDocument();
        break;
    }
}

void MainWindow::NewDocument() {
    // A file still opening is not wanted any more.
    loadId_ = 0;
    loading_ = false;
    DropPreviewBuild();
    doc_.New();
    editor_.SetText({});
    popup_.Close();
    popup_.SetHeadings({});
    syncedRevision_ = editor_.Revision();
    stats_ = DocumentStats();
    ResetSourceSync();
    exportTargets_.erase(std::string());
    SetStatus({}, StatusKind::Info);
    focusEditor_ = true;
    PreviewNow();
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::Open(const std::string& path) {
    DropPreviewBuild();
    loading_ = true;
    loadId_ = loader_->Load(path);
    SetStatus("Opening " + fs::u8path(path).filename().u8string() + "\xE2\x80\xA6", StatusKind::Info);
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::OnLoaded(DocumentLoaded loaded) {
    if (loaded.id != loadId_) return;  // a later open or a new document replaced it
    loading_ = false;
    if (!loaded.ok) {
        SetStatus(loaded.status, StatusKind::Error);
        // The document stays; its preview build was dropped for the open.
        PreviewNow();
        if (hooks_.requestFrame) hooks_.requestFrame();
        return;
    }
    trace_.opened = std::chrono::steady_clock::now();
    doc_ = std::move(loaded.document);
    editor_.SetText(doc_.text);
    popup_.Close();
    popup_.SetHeadings({});
    syncedRevision_ = editor_.Revision();
    stats_ = loaded.stats;
    ResetSourceSync();
    exportTargets_.erase(std::string());
    Remember(loaded.path);
    SetStatus(loaded.status, StatusKind::Info);
    focusEditor_ = true;
    PreviewNow();
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::OnImageCopied(ImageCopied copied) {
    if (!copied.ok) {
        SetStatus(std::move(copied.status), StatusKind::Error);
    } else if (copied.documentPath != doc_.filePath || loading_) {
        // Another document is open now: the copy stays, the link is not put into it.
        SetStatus(copied.status + "; the document changed, so no link was inserted", StatusKind::Warning);
    } else {
        // At the caret; a selection made since the drop stays text.
        editor_.Replace(editor_.Caret(), 0, copied.markdown, copied.markdown.size());
        SetStatus(std::move(copied.status), StatusKind::Info);
    }
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::ShowOpenDialog() {
    AskPath(FileDialog::OpenDocument, SuggestedFolder(), [this](std::string path) { Open(path); });
}

void MainWindow::Save(bool ask, std::function<void()> then) {
    if (!ask && !doc_.filePath.empty()) {
        if (SaveTo(doc_.filePath) && then) then();
        return;
    }
    AskPath(FileDialog::SaveMarkdown, SuggestedSavePath(), [this, then = std::move(then)](std::string path) {
        if (SaveTo(path) && then) then();
    });
}

bool MainWindow::SaveTo(const std::string& path) {
    SyncText();
    const std::string before = doc_.filePath;
    std::string status;
    const bool ok = doc_.Save(path, status);
    SetStatus(status, ok ? StatusKind::Info : StatusKind::Error);
    if (ok) {
        Remember(path);
        // Relative images now resolve from the new place.
        if (before != doc_.filePath) MarkPreviewDirty();
    }
    if (hooks_.requestFrame) hooks_.requestFrame();
    return ok;
}

void MainWindow::ExportPdf(bool ask) {
    const std::string key = doc_.filePath;
    const auto found = exportTargets_.find(key);
    if (!ask && found != exportTargets_.end()) {
        StartExport(found->second);
        return;
    }
    AskPath(FileDialog::ExportPdf, found != exportTargets_.end() ? found->second : SuggestedExportPath(),
            [this, key](std::string path) {
                path = WithPdfExtension(std::move(path));
                exportTargets_[key] = path;
                StartExport(path);
            });
}

void MainWindow::StartExport(const std::string& path) {
    SyncText();
    TinyPdf::PdfOptions options = export_.ToPdfOptions(doc_.filePath, false);
    if (export_.themeFile.empty()) options.theme = TinyPdf::PdfTheme();
    // The worker reads the theme file; the preview build in flight, if any, waits for this.
    worker_->RequestExport(doc_.text, doc_.filePath, std::move(options), WithPdfExtension(path),
                           export_.openAfterExport, export_.themeFile, export_.LinkDepth());
}

void MainWindow::ChooseThemeFile() {
    AskPath(FileDialog::ChooseTheme, export_.themeFile.empty() ? SuggestedFolder() : export_.themeFile,
            [this](std::string path) {
                export_.themeFile = std::move(path);
                export_.themeError.clear();  // the next build reads the file and says
                OptionsChanged();
            });
}

void MainWindow::ChooseVaultFolder() {
    std::string suggested = export_.vaultFolder;
    if (suggested.empty() && !doc_.filePath.empty()) suggested = fs::u8path(doc_.filePath).parent_path().u8string();
    // A folder's suggestion ends with a separator: the dialog opens in it.
    suggested = suggested.empty() ? SuggestedFolder() : (fs::u8path(suggested) / "").u8string();
    AskPath(FileDialog::ChooseVault, suggested, [this](std::string path) {
        export_.vaultFolder = std::move(path);
        OptionsChanged();
    });
}

std::string MainWindow::NotesFolder() const {
    if (!export_.vaultFolder.empty()) return export_.vaultFolder;
    if (doc_.filePath.empty()) return {};
    return fs::u8path(doc_.filePath).parent_path().u8string();
}

void MainWindow::RefreshNoteNames() {
    const std::string folder = NotesFolder();
    if (folder != notesFolder_) {
        // Another folder's names are not this one's.
        popup_.SetNotes({});
        notesFolder_ = folder;
        notesListedAt_.reset();
    }
    if (folder.empty() || notesPending_) return;
    const auto now = std::chrono::steady_clock::now();
    if (notesListedAt_ && now - *notesListedAt_ < std::chrono::milliseconds(kNotesMaxAgeMs)) return;
    notesPending_ = true;
    loader_->ListNotes(folder);
}

void MainWindow::OnNotesListed(NotesListed listed) {
    notesPending_ = false;
    // The folder changed meanwhile: the next completion asks again.
    if (listed.folder != notesFolder_ || !popup_.WikiLinks()) return;
    notesListedAt_ = std::chrono::steady_clock::now();
    popup_.SetNotes(std::move(listed.names));
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::AskPath(FileDialog kind, std::string suggested, std::function<void(std::string)> onChosen) {
    platform_.ShowFileDialog(kind, suggested,
                             [link = link_, kind, suggested, onChosen = std::move(onChosen)](FileDialogResult result) mutable {
        if (!link->alive) return;
        MainWindow& window = *link->owner;
        switch (result.kind) {
        case FileDialogResult::Chosen:
            if (!result.path.empty() && onChosen) onChosen(std::move(result.path));
            break;
        case FileDialogResult::Cancelled:
            break;
        case FileDialogResult::Unavailable:
            if (!window.pathPrompt_.open) {
                window.pathPrompt_.open = true;
                window.pathPrompt_.focus = true;
                window.pathPrompt_.title = DialogTitle(kind);
                window.pathPrompt_.text = std::move(suggested);
                window.pathPrompt_.onChosen = std::move(onChosen);
            }
            break;
        case FileDialogResult::Failed:
            window.SetStatus("File dialog failed", StatusKind::Error);
            break;
        }
        if (window.hooks_.requestFrame) window.hooks_.requestFrame();
    });
}

void MainWindow::OptionsChanged() {
    hooks_.settingsChanged();
    MarkPreviewDirty();
    Dirty();
}

void MainWindow::MarkPreviewDirty() {
    previewDirty_ = true;
    // Within a frame the build starts at its end; else a frame comes for it.
    if (!inFrame_ && hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::PreviewNow() {
    previewDirty_ = true;
    MaybeStartPreview();
}

void MainWindow::MaybeStartPreview() {
    if (!previewDirty_ || previewInFlight_ || loading_) return;
    previewDirty_ = false;
    previewInFlight_ = true;
    // The one copy of the text this build makes; doc_.text is synced only where it is needed
    // (saving, exporting).
    std::string text = editor_.Revision() == syncedRevision_ ? doc_.text : editor_.CopyText();
    TinyPdf::PdfOptions options = export_.ToPdfOptions(doc_.filePath, true);
    if (export_.themeFile.empty()) options.theme = TinyPdf::PdfTheme();
    const int generation = ++requestedGeneration_;
    requestedAt_ = std::chrono::steady_clock::now();
    worker_->RequestPreview(std::move(text), doc_.filePath, std::move(options), generation, export_.themeFile,
                            popup_.WikiLinks());
    if (budgetTimer_ != 0) platform_.CancelTimer(budgetTimer_);
    budgetTimer_ = platform_.StartTimer(kPreviewBudgetMs, [link = link_, generation] {
        if (!link->alive) return;
        MainWindow& window = *link->owner;
        window.budgetTimer_ = 0;
        if (!window.previewInFlight_ || window.requestedGeneration_ != generation) return;
        // Not built again before the next change.
        window.worker_->CancelPreview();
        window.SetStatus("Preview stopped: the document takes too long to build", StatusKind::Warning);
        if (window.hooks_.requestFrame) window.hooks_.requestFrame();
    });
}

void MainWindow::DropPreviewBuild() {
    if (!previewInFlight_) return;
    worker_->CancelPreview();
    // Its result, cancelled or not (it may have finished first), is not shown.
    discardThrough_ = requestedGeneration_;
}

void MainWindow::SyncText() {
    if (editor_.Revision() == syncedRevision_) return;
    doc_.text = editor_.CopyText();
    syncedRevision_ = editor_.Revision();
}

void MainWindow::Remember(const std::string& path) {
    const std::string dir = fs::u8path(path).parent_path().u8string();
    if (!dir.empty() && dir != lastDirectory_) {
        lastDirectory_ = dir;
        hooks_.settingsChanged();
    }
}

std::string MainWindow::SuggestedFolder() const {
    const fs::path dir = lastDirectory_.empty() ? platform_.DocumentsDir() : fs::u8path(lastDirectory_);
    return (dir / "").u8string();
}

std::string MainWindow::SuggestedSavePath() const {
    if (!doc_.filePath.empty()) return doc_.filePath;
    const fs::path dir = lastDirectory_.empty() ? platform_.DocumentsDir() : fs::u8path(lastDirectory_);
    return (dir / fs::u8path(doc_.displayName)).u8string();
}

std::string MainWindow::SuggestedExportPath() const {
    if (!doc_.filePath.empty()) {
        const fs::path source = fs::u8path(doc_.filePath);
        return (source.parent_path() / source.stem()).u8string() + ".pdf";
    }
    return (platform_.DocumentsDir() / fs::u8path(doc_.displayName).stem()).u8string() + ".pdf";
}

// ---- Results ----

std::optional<MainWindow::Anchor> MainWindow::ViewAnchor() const {
    if (pageTops_.empty()) return std::nullopt;
    // The first page whose bottom margin reaches below the top of the view.
    size_t i = 0;
    while (i + 1 < pageTops_.size() && pageTops_[i] + pageHeights_[i] + kPageMargin <= previewOffset_) i++;
    Anchor anchor;
    anchor.page = static_cast<int>(i);
    anchor.fraction = pageHeights_[i] > 0 ? (previewOffset_ - pageTops_[i]) / pageHeights_[i] : 0;
    return anchor;
}

// ---- Source sync ----

void MainWindow::ResetSourceSync() {
    sourceMap_ = SourceMap();
    leader_ = ScrollLeader::Editor;
    lastEditorScroll_ = -1;
    editorReveal_.reset();
    previewReveal_.reset();
    editorScrollNext_.reset();
    flashPage_ = -1;
}

float MainWindow::PreviewYOfPage(double page) const {
    if (pageTops_.empty()) return 0;
    const size_t count = pageTops_.size();
    const double at = Within(page, 0, static_cast<double>(count));
    const size_t p = std::min(static_cast<size_t>(at), count - 1);
    return pageTops_[p] + static_cast<float>(Within(at - static_cast<double>(p), 0, 1)) * pageHeights_[p];
}

double MainWindow::PageOfPreviewY(float y) const {
    if (pageTops_.empty()) return 0;
    // The last page whose top is at or above y; the first when y is above it.
    const auto above = std::upper_bound(pageTops_.begin(), pageTops_.end(), y);
    const size_t p = above == pageTops_.begin() ? 0 : static_cast<size_t>(above - pageTops_.begin()) - 1;
    const double into = pageHeights_[p] > 0 ? (y - pageTops_[p]) / pageHeights_[p] : 0;
    return static_cast<double>(p) + Within(into, 0, 1);
}

float MainWindow::PreviewOffsetForEditor() const {
    const ui::TextBuffer& buffer = editor_.Buffer();
    const float reference = PreviewYOfPage(sourceMap_.PageOfLine(buffer, 0.0));
    const float scrollY = editor_.ScrollY();
    float target = PreviewYOfPage(sourceMap_.PageOfLine(buffer, editor_.LineAtScrollY(scrollY))) - reference;
    const float previewMax = std::max(0.0f, previewContent_ - previewView_);
    const float editorMax = editor_.MaxScrollY();
    if (editorMax > 0 && scrollY > editorMax - editor_.ViewHeight()) {
        const float from = std::max(0.0f, editorMax - editor_.ViewHeight());
        target += (previewMax - target) * ((scrollY - from) / (editorMax - from));
    }
    return std::min(std::max(target, 0.0f), previewMax);
}

float MainWindow::EditorScrollForPreview(float offset) const {
    const ui::TextBuffer& buffer = editor_.Buffer();
    const float reference = PreviewYOfPage(sourceMap_.PageOfLine(buffer, 0.0));
    float target = editor_.ScrollYOfLine(sourceMap_.LineOfPage(buffer, PageOfPreviewY(offset + reference)));
    const float previewMax = std::max(0.0f, previewContent_ - previewView_);
    const float editorMax = editor_.MaxScrollY();
    if (previewMax > 0 && offset > previewMax - previewView_) {
        const float from = std::max(0.0f, previewMax - previewView_);
        target += (editorMax - target) * ((offset - from) / (previewMax - from));
    }
    return std::min(std::max(target, 0.0f), editorMax);
}

void MainWindow::OnPreview(PreviewReady ready) {
    if (ready.generation == requestedGeneration_) {
        previewInFlight_ = false;
        if (budgetTimer_ != 0) {
            platform_.CancelTimer(budgetTimer_);
            budgetTimer_ = 0;
        }
    }
    // Cancelled, dropped, or older than the one shown: changes nothing but lets the next build
    // start.
    if (ready.buildError == TinyPdf::BuildError::Cancelled || ready.generation <= discardThrough_ ||
        ready.generation <= shownGeneration_) {
        MaybeStartPreview();
        return;
    }
    shownGeneration_ = ready.generation;
    shownBuildMs_ = ready.ms;
    if (ready.generation == requestedGeneration_) trace_.buildRequested = requestedAt_;
    trace_.shown = std::chrono::steady_clock::now();
    stats_ = ready.stats;
    if (popup_.WikiLinks()) popup_.SetHeadings(std::move(ready.headings));
    if (ready.themeFile == export_.themeFile) export_.themeError = std::move(ready.themeError);
    if (ready.pdf) {
        // Keep the view on the page at its top, at the same place within it.
        if (!anchor_) anchor_ = ViewAnchor();
        preview_->SetDocument(std::move(ready.pdf), ready.generation);
        sourceMap_ = std::move(ready.sourceMap);
        // The anchor keeps the view first; the editor, when it leads, then sets it.
        if (syncScroll_ && leader_ == ScrollLeader::Editor) syncPreviewNow_ = true;
        std::string status = "Preview: " + Count(ready.pages, "page") + " in " + FormatMs(ready.ms);
        if (ready.missingCharacters > 0) status += "; " + Count(ready.missingCharacters, "character") + " missing";
        if (ready.failedImages > 0) {
            status += "; " + Count(ready.failedImages, "image") + " failed";
            if (doc_.filePath.empty()) status += "; save the document to show its relative images";
        }
        SetStatus(std::move(status), StatusKind::Info, true);
    } else {
        SetStatus("Export failed: " + ready.error, StatusKind::Error, true);
    }
    // Typed while it built: the next build starts now.
    MaybeStartPreview();
    if (hooks_.requestFrame) hooks_.requestFrame();
}

void MainWindow::OnExport(ExportFinished finished) {
    if (finished.themeFile == export_.themeFile) export_.themeError = std::move(finished.themeError);
    SetStatus(std::move(finished.message), finished.ok ? StatusKind::Info : StatusKind::Error);
    if (hooks_.requestFrame) hooks_.requestFrame();
}

} // namespace rgui
