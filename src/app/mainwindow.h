// SPDX-License-Identifier: Apache-2.0
// The main window (plan Phase 8 step 6), built every frame with RectCut in logical pixels: the
// client-side header when the platform needs one, the footer (Omawrite's: icon buttons, status,
// counts), the options panel, and the editor | splitter | preview content area. It owns the
// document and everything done to it: opening, saving, exporting, the unsaved-changes prompt,
// the live preview, the shortcuts, and the `ui.` and `export.` settings except the window's
// size.
//
// The live preview is pulled, not debounced. A change of the text or of an export option marks
// the preview dirty. A build starts at the end of the frame that saw the change (after all of
// that frame's input), when no preview build is in flight; the text is copied then, once per
// build. When the build's result comes back and the preview is dirty again, the next build
// starts at once. So at most one preview build is in flight and none waits behind it, at most
// one starts per frame, an idle engine gets a keystroke within its frame, and the keystrokes
// typed during a build all go into the one after it. The word and line counts come back with
// the build's result (the worker counts them), and the worker reads the PDF theme file: the UI
// thread does no O(n) or file work per keystroke beyond the one copy of the text.
//
// The build in flight is cancelled (PdfOptions::cancel) when the document changes (open, new),
// when an export is requested (the worker builds it again after the export), when it runs over
// kPreviewBudgetMs (status `Preview stopped: the document takes too long to build`; no retry
// before the next change), and when the window goes. Cancelled results are dropped silently.
// Files open on the document loader's thread, the status saying `Opening <name>…` meanwhile.
#pragma once

#include "app/buildworker.h"
#include "app/document.h"
#include "app/documentloader.h"
#include "app/exportsettings.h"
#include "app/platform.h"
#include "app/previewpages.h"
#include "ui/texteditor.h"
#include "ui/ui.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rgui {

namespace text {
class FontIndex;
}
namespace ui {
class SettingsFile;
}

// The preview zoom ladder (times the pane width less 32 px).
constexpr float kPreviewZoomLadder[] = {0.5f, 0.67f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 2.0f, 3.0f};
constexpr int kPreviewZoomCount = 10;
constexpr int kPreviewZoomDefault = 4;

enum class StatusKind : uint8_t { Info, Warning, Error };

class MainWindow {
public:
    // A preview build running longer is cancelled.
    static constexpr int kPreviewBudgetMs = 10000;
    static constexpr float kHeaderHeight = 36;
    static constexpr float kFooterHeight = 32;
    static constexpr float kOptionsWidth = 300;
    static constexpr float kResizeEdge = 6;

    struct Hooks {
        std::function<void()> requestFrame;     // something changed between frames
        std::function<void()> settingsChanged;  // a stored setting changed
        std::function<void()> themeChanged;     // ThemeSetting() changed: wear it
        std::function<void()> quit;             // closing was confirmed
    };

    // `fontIndex` (outliving the window) serves the preview's standard PDF fonts. `build`
    // replaces TinyPdf::BuildPdf (tests).
    MainWindow(Platform& platform, const text::FontIndex* fontIndex, Hooks hooks,
               BuildWorker::BuildFunction build = nullptr);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // The `ui.` (theme, editorZoom, previewZoom, splitRatio, optionsVisible, lastDirectory) and
    // `export.` settings.
    void LoadSettings(const ui::SettingsFile& file);
    void StoreSettings(ui::SettingsFile& file) const;

    // Builds the window for one frame.
    void Frame(ui::Context& ctx);

    // ---- Actions ----
    // Opens `path` without the unsaved prompt: on the loader's thread, then shows it and builds
    // its preview. When it cannot be opened the status says why and the document stays.
    void Open(const std::string& path);
    // Builds the preview of the current text: now, or when the build in flight ends.
    void PreviewNow();
    // The window's close button, Ctrl+Q, the platform's close request: through the unsaved
    // prompt, then Hooks::quit.
    void RequestClose();
    // A dropped file: .md .markdown .txt .pdf open through the unsaved prompt.
    void DropFile(const std::string& path);
    // The status line; a preview's status gives way to the PDF theme's error and to the notice.
    void SetStatus(std::string text, StatusKind kind, bool fromPreview = false);
    // A lasting warning (the interface font, an unreadable Omarchy theme) shown instead of a
    // preview's status; empty clears it.
    void SetNotice(std::string text);
    // Whether the Theme menu offers `Follow Omarchy` (else `Automatic`) first.
    void SetOmarchyAvailable(bool available);

    // ---- State ----
    // `ui.theme`: `omarchy`, `auto` or a bundled slug.
    const std::string& ThemeSetting() const { return themeSetting_; }
    // The status line as shown, and its kind.
    std::string ShownStatus(StatusKind* kind = nullptr) const;
    std::string Title() const;
    // The preview shows pages of the newest build, no build is pending, no file is opening, and
    // every visible page shows its current image (snapshot mode's condition).
    bool PreviewSettled() const { return settled_; }
    // A file is being opened.
    bool Loading() const { return loading_; }
    // A preview build is in flight.
    bool PreviewBuilding() const { return previewInFlight_; }
    bool UnsavedPromptOpen() const { return prompt_.open; }
    bool PathPromptOpen() const { return pathPrompt_.open; }
    bool OptionsVisible() const { return optionsVisible_; }
    int PreviewZoomIndex() const { return previewZoom_; }
    // The preview's scroll offset in the last frame, logical px.
    float PreviewScroll() const { return previewOffset_; }
    int RequestedGeneration() const { return requestedGeneration_; }
    int ShownGeneration() const { return shownGeneration_; }
    const DocumentStats& Stats() const { return stats_; }

    Document& Doc() { return doc_; }
    ui::TextEditor& Editor() { return editor_; }
    ExportSettings& Export() { return export_; }
    PreviewPages& Preview() { return *preview_; }

private:
    enum class Action : uint8_t { Close, OpenDialog, OpenPath, New };
    struct UnsavedPrompt {
        bool open = false;
        Action action = Action::Close;
        std::string path;
    };
    struct PathPrompt {
        bool open = false;
        bool focus = false;
        std::string title;
        std::string text;
        std::function<void(std::string)> onChosen;
    };
    struct Anchor {
        int page = 0;
        float fraction = 0;
    };
    // Shared with platform callbacks, which may come after the window is gone.
    struct Link {
        bool alive = true;
        MainWindow* owner = nullptr;
    };

    // Frame parts.
    void ResizeEdges(ui::Context& ctx);
    void Header(ui::Context& ctx, ui::RectF rect);
    void Footer(ui::Context& ctx, ui::RectF rect);
    void Options(ui::Context& ctx, ui::RectF rect);
    void OptionsContent(ui::Context& ctx, ui::RectF column, float& height);
    void PreviewPane(ui::Context& ctx, ui::RectF rect);
    void Prompts(ui::Context& ctx);
    void Shortcuts(ui::Context& ctx);

    // Actions.
    void Guarded(Action action, std::string path = {});
    void Proceed(Action action, const std::string& path);
    void NewDocument();
    void ShowOpenDialog();
    // Saves to the document's path, else asks; `then` runs after a successful save.
    void Save(bool ask, std::function<void()> then = {});
    bool SaveTo(const std::string& path);
    void ExportPdf(bool ask);
    void StartExport(const std::string& path);
    void ChooseThemeFile();
    // Asks for a path with the system dialog (or the fallback prompt when it is unavailable).
    void AskPath(FileDialog kind, std::string suggested, std::function<void(std::string)> onChosen);
    void OptionsChanged();
    // The text or an option changed: build the preview again (see the policy above).
    void MarkPreviewDirty();
    // Starts the preview build when it is dirty and none is in flight (nor a file opening).
    void MaybeStartPreview();
    // The document is about to change: the preview build in flight is no longer wanted.
    void DropPreviewBuild();
    // Copies the editor's text into doc_.text when it changed.
    void SyncText();
    void Remember(const std::string& path);
    void Dirty() { dirty_ = true; }

    std::string SuggestedFolder() const;
    std::string SuggestedSavePath() const;
    std::string SuggestedExportPath() const;

    void OnPreview(PreviewReady ready);
    void OnExport(ExportFinished finished);
    void OnLoaded(DocumentLoaded loaded);

    // The page at the top of the preview and how far into it the view is, by the last frame.
    std::optional<Anchor> ViewAnchor() const;

    Platform& platform_;
    Hooks hooks_;
    std::shared_ptr<Link> link_;

    Document doc_;
    ui::TextEditor editor_;
    uint64_t syncedRevision_ = 0;
    ExportSettings export_;
    DocumentStats stats_;

    // Settings.
    std::string themeSetting_ = "omarchy";
    int previewZoom_ = kPreviewZoomDefault;
    float splitRatio_ = 0.5f;
    bool optionsVisible_ = false;
    std::string lastDirectory_;
    bool omarchy_ = false;
    std::vector<std::string> themeItems_;

    // Status.
    std::string status_;
    StatusKind statusKind_ = StatusKind::Info;
    bool statusFromPreview_ = false;
    std::string notice_;

    // Prompts and exports.
    UnsavedPrompt prompt_;
    PathPrompt pathPrompt_;
    std::map<std::string, std::string> exportTargets_;   // document path -> PDF, this session

    // Preview.
    std::unique_ptr<BuildWorker> worker_;
    std::unique_ptr<PreviewPages> preview_;
    std::unique_ptr<DocumentLoader> loader_;
    bool previewDirty_ = false;     // changed since the last build started
    bool previewInFlight_ = false;  // a build was started and its result has not come back
    int discardThrough_ = 0;        // results of these generations and older are dropped
    uint64_t budgetTimer_ = 0;      // the build in flight's kPreviewBudgetMs
    bool inFrame_ = false;
    bool loading_ = false;
    uint64_t loadId_ = 0;           // the open whose result is wanted
    int requestedGeneration_ = 0;
    int shownGeneration_ = 0;
    std::optional<Anchor> anchor_;
    std::vector<float> pageTops_;     // the last frame's layout, logical px from the content top
    std::vector<float> pageHeights_;
    float pageWidth_ = 0;
    float previewOffset_ = 0;
    float previewZoomWheel_ = 0;
    std::vector<int> wanted_;
    bool settled_ = false;

    bool focusEditor_ = true;
    bool dirty_ = false;
};

} // namespace rgui
