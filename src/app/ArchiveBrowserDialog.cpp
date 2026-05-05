#include "stdafx.h"
#include "ArchiveBrowserDialog.h"
#include "CommandLine.h"
#include "ExtractDialog.h"
#include "flat_button.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <CommCtrl.h>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <ole2.h>
#include <oleidl.h>
#include <set>
#include <shellapi.h>
#include <shlobj_core.h>
#include <shlwapi.h>
#include <system_error>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// ─── Geometry ─────────────────────────────────────────────────────────
// Layout per row in the Name column:
//   [ depth*kIndentPerDepth ] [ kArrowWidth ] [ kIconWidth ] [ kLabelGap ] [ leaf text ]
// The arrow is drawn for folders only; files leave the arrow zone blank.
constexpr int kIndentPerDepth = 16;
constexpr int kArrowWidth     = 16;
constexpr int kIconWidth      = 18;
constexpr int kLabelGap       = 4;

// ─── Listview palette ─────────────────────────────────────────────────
// Owner-drawn in BOTH modes for visual consistency with flat_button —
// the rendering path doesn't change when light/dark is toggled, only
// the color choices below.
//
// Dark tier: dialog 32 → list body 38 → header 43 (subtle elevation).
// Light tier: dialog 240 (sys 3DFACE) → list body 255 (white) → header 247.
constexpr COLORREF kListBodyDark      = RGB(38, 38, 38);
constexpr COLORREF kListFgDark        = RGB(220, 220, 220);
constexpr COLORREF kListSelFocBgDark  = RGB(38, 79, 120);
constexpr COLORREF kListSelFocFgDark  = RGB(255, 255, 255);
constexpr COLORREF kListSelUnfBgDark  = RGB(60, 60, 60);
constexpr COLORREF kListSelUnfFgDark  = RGB(220, 220, 220);

constexpr COLORREF kListBodyLight     = RGB(255, 255, 255);
constexpr COLORREF kListFgLight       = RGB(28, 28, 28);
constexpr COLORREF kListSelFocBgLight = RGB(0, 120, 212);   // Win11 accent
constexpr COLORREF kListSelFocFgLight = RGB(255, 255, 255);
constexpr COLORREF kListSelUnfBgLight = RGB(225, 225, 225);
constexpr COLORREF kListSelUnfFgLight = RGB(28, 28, 28);

// ─── Format helpers ──────────────────────────────────────────────────

// 1024-base human readable. "0 B" for empty so column doesn't look broken.
std::wstring FormatSize(uint64_t bytes) {
    constexpr uint64_t K = 1024;
    constexpr uint64_t M = K * K;
    constexpr uint64_t G = M * K;
    constexpr uint64_t T = G * K;
    wchar_t buf[32];
    if (bytes == 0)            ::swprintf_s(buf, L"0 B");
    else if (bytes < K)        ::swprintf_s(buf, L"%llu B",  static_cast<unsigned long long>(bytes));
    else if (bytes < M)        ::swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / K);
    else if (bytes < G)        ::swprintf_s(buf, L"%.2f MB", static_cast<double>(bytes) / M);
    else if (bytes < T)        ::swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / G);
    else                       ::swprintf_s(buf, L"%.2f TB", static_cast<double>(bytes) / T);
    return buf;
}

// Local time, YYYY-MM-DD HH:MM:SS. Empty string when t is 0/invalid.
std::wstring FormatDateTime(std::time_t t) {
    if (t <= 0) return L"";
    std::tm lt{};
    if (::localtime_s(&lt, &t) != 0) return L"";
    wchar_t buf[32];
    ::swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
                 lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                 lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

int LookupShellIconIndex(const std::wstring& name, bool is_dir) {
    SHFILEINFOW sfi{};
    DWORD attrs = is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (::SHGetFileInfoW(name.c_str(), attrs, &sfi, sizeof(sfi),
                         SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        return sfi.iIcon;
    }
    return 0;
}

std::wstring NormalizePath(std::wstring s) {
    for (auto& ch : s) if (ch == L'\\') ch = L'/';
    return s;
}

// ─── Temp-preview directory ──────────────────────────────────────────
//
// Both "double-click → open with default handler" and "drag-out to Explorer"
// extract on demand into a private subfolder under %TEMP%/openzip-preview/.
// We can't delete the files at the moment they're consumed (the launched
// app or the drop target keeps them open for an unknown amount of time),
// so cleanup runs lazily at dialog launch — anything older than 24 hours
// gets removed.
constexpr wchar_t kPreviewRootName[] = L"openzip-preview";

std::filesystem::path PreviewRoot() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(buf) / kPreviewRootName;
}

std::filesystem::path MakeFreshPreviewDir() {
    auto root = PreviewRoot();
    if (root.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    // Random 8-hex suffix is enough collision protection; the parent is
    // per-user and the folder lifetime is bounded by CleanupOldPreviewDirs.
    GUID g{};
    if (FAILED(::CoCreateGuid(&g))) return {};
    wchar_t suffix[32];
    ::swprintf_s(suffix, L"%08lX-%04hX", g.Data1, g.Data2);
    auto dir = root / suffix;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    return dir;
}

// ─── Drag-source COM objects ─────────────────────────────────────────
//
// These two classes are the minimal surface area required by DoDragDrop:
//
//   * CHDropDataObject — exposes a single CF_HDROP / TYMED_HGLOBAL format,
//     handing the drop target a list of absolute file paths via the standard
//     DROPFILES blob. Drop targets that understand CF_HDROP (Explorer,
//     practically every Windows app) treat this exactly like a drag from
//     File Explorer.
//
//   * CDropSource — feedback / cancellation glue. Cancel on Esc, drop on
//     left-button release, default cursors otherwise. No fancy effect logic
//     — DROPEFFECT_COPY is the only operation we offer.

HGLOBAL BuildHDropGlobal(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return nullptr;
    size_t chars = 1;  // trailing extra NUL terminating the double-NUL list
    for (const auto& p : paths) chars += p.size() + 1;
    SIZE_T bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
    HGLOBAL h = ::GlobalAlloc(GHND, bytes);
    if (!h) return nullptr;
    auto* df = static_cast<DROPFILES*>(::GlobalLock(h));
    if (!df) { ::GlobalFree(h); return nullptr; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide  = TRUE;
    auto* p = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES));
    for (const auto& s : paths) {
        std::memcpy(p, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        p += s.size() + 1;
    }
    *p = L'\0';  // double-NUL terminator
    ::GlobalUnlock(h);
    return h;
}

// CHDropDataObject — lazy CF_HDROP source.
//
// The straightforward implementation extracts before DoDragDrop and hands a
// pre-built CF_HDROP to the data object. That means the user feels a stall
// at drag-start while extraction runs, AND password-protected archives can't
// participate in drag-out (a modal password prompt mid-gesture would steal
// the mouse capture and break the drag).
//
// Lazy variant: at drag-start we only remember the *intent* — which entry
// names to extract, where the temp directory will live, and a callback that
// performs the extraction with full UI (CExtractDialog: password prompt,
// conflict prompt, progress bar). The callback runs on the FIRST GetData
// call, which by Win32 conventions only happens at drop time (drop targets
// use QueryGetData / EnumFormatEtc during the drag loop and only call
// GetData(CF_HDROP) once the user releases the mouse over a willing
// target). By that point the drag gesture has ended and we're back in a
// regular UI message pump, so a modal password dialog is safe.
//
// Subsequent GetData calls reuse the cached HDROP so a target that calls
// twice (rare, but happens) doesn't kick off a second extraction.
class CHDropDataObject : public IDataObject {
public:
    // ExtractFn returns the absolute paths to be advertised in CF_HDROP, or
    // false if extraction failed / was cancelled (drag aborts cleanly).
    using ExtractFn = std::function<bool(std::vector<std::wstring>*)>;

    explicit CHDropDataObject(ExtractFn fn) : extract_(std::move(fn)) {}
    ~CHDropDataObject() { if (hdrop_) ::GlobalFree(hdrop_); }
    CHDropDataObject(const CHDropDataObject&) = delete;
    CHDropDataObject& operator=(const CHDropDataObject&) = delete;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ::InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = ::InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IDataObject — only CF_HDROP / TYMED_HGLOBAL is offered.
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* m) override {
        if (!fe || !m) return E_POINTER;
        if (fe->cfFormat != CF_HDROP || !(fe->tymed & TYMED_HGLOBAL))
            return DV_E_FORMATETC;

        if (state_ == State::kInitial) {
            std::vector<std::wstring> paths;
            bool ok = extract_ && extract_(&paths);
            if (ok) hdrop_ = BuildHDropGlobal(paths);
            state_ = (ok && hdrop_) ? State::kReady : State::kFailed;
        }
        if (state_ != State::kReady || !hdrop_) return E_FAIL;

        // Hand the target its own copy — once GetData returns, the medium
        // belongs to the caller (it'll call ReleaseStgMedium).
        SIZE_T sz = ::GlobalSize(hdrop_);
        HGLOBAL clone = ::GlobalAlloc(GHND, sz);
        if (!clone) return E_OUTOFMEMORY;
        void* src = ::GlobalLock(hdrop_);
        void* dst = ::GlobalLock(clone);
        if (src && dst) std::memcpy(dst, src, sz);
        if (dst) ::GlobalUnlock(clone);
        if (src) ::GlobalUnlock(hdrop_);
        m->tymed          = TYMED_HGLOBAL;
        m->hGlobal        = clone;
        m->pUnkForRelease = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (!fe) return E_POINTER;
        if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** ppe) override {
        if (!ppe) return E_POINTER;
        if (dir != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fe{};
        fe.cfFormat = CF_HDROP;
        fe.dwAspect = DVASPECT_CONTENT;
        fe.lindex   = -1;
        fe.tymed    = TYMED_HGLOBAL;
        return ::SHCreateStdEnumFmtEtc(1, &fe, ppe);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    enum class State { kInitial, kReady, kFailed };
    LONG      ref_   = 1;
    State     state_ = State::kInitial;
    ExtractFn extract_;
    HGLOBAL   hdrop_ = nullptr;
};

class CDropSource : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IDropSource) {
            *ppv = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef()  override { return ::InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = ::InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape)              return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    LONG ref_ = 1;
};

}  // anonymous namespace

IMPLEMENT_DYNAMIC(CArchiveBrowserDialog, CDialogEx)

CArchiveBrowserDialog::CArchiveBrowserDialog(CWnd* p)
    : CDialogEx(IDD, p) {}

BEGIN_MESSAGE_MAP(CArchiveBrowserDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BROWSE_EXTRACT_ALL, &CArchiveBrowserDialog::OnExtractAll)
    ON_WM_CTLCOLOR()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_WM_PAINT()
    ON_NOTIFY(NM_CLICK,         IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListClick)
    ON_NOTIFY(NM_DBLCLK,        IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED,  IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListItemChanged)
    ON_NOTIFY(NM_CUSTOMDRAW,    IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListCustomDraw)
    ON_NOTIFY(NM_RCLICK,        IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListRClick)
    ON_NOTIFY(LVN_COLUMNCLICK,  IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListColumnClick)
    ON_NOTIFY(LVN_BEGINDRAG,    IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListBeginDrag)
END_MESSAGE_MAP()

void CArchiveBrowserDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_BROWSE_LIST, list_);
}

BOOL CArchiveBrowserDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
    }
    openzip::flat::ApplyToDialog(GetSafeHwnd());
    openzip::icons::ApplyDialogIcon(GetSafeHwnd(), AfxGetResourceHandle(), IDR_MAINFRAME);

    CString s;
    s.LoadString(IDS_DIALOG_BROWSE_TITLE);  SetWindowText(s);
    s.LoadString(IDS_BUTTON_CLOSE);         SetDlgItemText(IDCANCEL, s);
    UpdateButtonLabel();

    SetDlgItemText(IDC_BROWSE_ZIPPATH, zip_path.wstring().c_str());

    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // Match Win11 Explorer's borderless file-list pane:
    //   * strip WS_EX_CLIENTEDGE / WS_EX_STATICEDGE (3D etched outline).
    //   * strip WS_BORDER (even the 1-px outline reads as a sunken inset
    //     under dark-mode theming, where the inner client is themed but the
    //     non-client frame is drawn by the OS in a default sys color).
    // The dark_theme module already calls SetWindowTheme(L"DarkMode_Explorer")
    // on this listview during EnableForWindow, which gives us modern
    // selection/hover rendering and the matching header look — re-themeing
    // the header here ourselves regressed text contrast on the column
    // headers, so we leave that to dark_theme's pass.
    if (HWND lh = list_.GetSafeHwnd()) {
        LONG_PTR ex = ::GetWindowLongPtr(lh, GWL_EXSTYLE);
        ex &= ~(WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
        ::SetWindowLongPtr(lh, GWL_EXSTYLE, ex);

        LONG_PTR st = ::GetWindowLongPtr(lh, GWL_STYLE);
        st &= ~WS_BORDER;
        ::SetWindowLongPtr(lh, GWL_STYLE, st);

        ::SetWindowPos(lh, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // Set the listview's nominal palette in BOTH modes. These are
        // necessary but not sufficient on Win11 — DarkMode_Explorer +
        // LVS_EX_DOUBLEBUFFER routes painting through a buffered code path
        // that ignores LVM_SETBKCOLOR for the row strip and empty body
        // area. The actual fill comes from CDDS_PREPAINT (full client) and
        // per-row clrTextBk in CDDS_ITEMPREPAINT (default-drawn subitems);
        // these calls just give the control a sane fallback bg color for
        // any code path that *does* honor SetBkColor (in-place rename
        // edit overlay, etc).
        const bool dark = openzip::dark_theme::IsDarkModeActive();
        list_.SetBkColor(dark ? kListBodyDark : kListBodyLight);
        list_.SetTextBkColor(dark ? kListBodyDark : kListBodyLight);
        list_.SetTextColor(dark ? kListFgDark : kListFgLight);
    }

    // Cache the system small-icon image list so we can ImageList_Draw from
    // custom-draw. The handle is owned by the shell — never free it.
    SHFILEINFOW sfi{};
    sys_images_ = reinterpret_cast<HIMAGELIST>(::SHGetFileInfoW(
        L"", 0, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    if (sys_images_) {
        ::ImageList_GetIconSize(sys_images_, &icon_w_, &icon_h_);
        // Note: we do NOT attach this list to the listview anymore — custom
        // draw bypasses the system icon column entirely so we can put the
        // arrow to its left.
    }

    CString name, size, packed, modified, created;
    name.LoadString(IDS_LABEL_BROWSE_NAME);
    size.LoadString(IDS_LABEL_BROWSE_SIZE);
    packed.LoadString(IDS_LABEL_BROWSE_PACKED);
    modified.LoadString(IDS_LABEL_BROWSE_MODIFIED);
    created.LoadString(IDS_LABEL_BROWSE_CREATED);
    list_.InsertColumn(0, name,     LVCFMT_LEFT,  300);
    list_.InsertColumn(1, size,     LVCFMT_RIGHT,  90);
    list_.InsertColumn(2, packed,   LVCFMT_RIGHT,  90);
    list_.InsertColumn(3, modified, LVCFMT_LEFT,  140);
    list_.InsertColumn(4, created,  LVCFMT_LEFT,  140);

    // Subclass the column header for owner-drawing in BOTH light and dark
    // modes. Done AFTER InsertColumn so the header is fully populated
    // before our paint runs (if attached earlier the system theme can
    // sometimes prepaint the header before our subclass installs, and
    // because the listview's first WM_PAINT may have already shipped to
    // its window cache, we'd see a flash of the system header look first).
    // EnableForWindow may have already subclassed in dark mode, but
    // SubclassHeader is idempotent — it short-circuits on second call.
    if (HWND lv_hdr = ListView_GetHeader(list_.GetSafeHwnd())) {
        openzip::dark_theme::SubclassHeader(lv_hdr);
        ::InvalidateRect(lv_hdr, nullptr, TRUE);
    }

    // OLE drag-source needs an STA. AfxOleInit() is the canonical MFC entry
    // point — calling it more than once is harmless (it short-circuits on
    // already-initialized threads). DoDragDrop in OnListBeginDrag depends
    // on this.
    ::AfxOleInit();

    // Preview-temp housekeeping. Sweeping at dialog launch is good enough —
    // the user must open a browser before they can produce new preview files,
    // so this naturally bounds the on-disk footprint.
    CleanupOldPreviewDirs();

    entries_ = openzip::Extractor::ListEntries(zip_path);
    expanded_folders_.clear();

    RebuildVisibleRows();
    RenderRows();
    UpdateSortIndicator();
    return TRUE;
}

// ─── Tree model construction ──────────────────────────────────────────

void CArchiveBrowserDialog::RebuildVisibleRows() {
    visible_rows_.clear();

    struct FolderAgg {
        uint64_t uncompressed_total = 0;
        uint64_t compressed_total = 0;
        std::time_t latest_mtime = 0;
        std::time_t latest_ctime = 0;
    };
    std::map<std::wstring, FolderAgg> folders;

    auto add_ancestors = [&](const std::wstring& norm_path,
                             const openzip::Extractor::Entry& e) {
        size_t pos = 0;
        while (true) {
            size_t sl = norm_path.find(L'/', pos);
            if (sl == std::wstring::npos) break;
            std::wstring folder = norm_path.substr(0, sl + 1);
            auto& agg = folders[folder];
            if (!e.is_dir) {
                agg.uncompressed_total += e.uncompressed_size;
                agg.compressed_total   += e.compressed_size;
            }
            if (e.modified_time > agg.latest_mtime) agg.latest_mtime = e.modified_time;
            if (e.created_time  > agg.latest_ctime) agg.latest_ctime = e.created_time;
            pos = sl + 1;
        }
    };

    for (const auto& e : entries_) {
        std::wstring norm = NormalizePath(e.name);
        if (e.is_dir) {
            std::wstring p = norm;
            if (!p.empty() && p.back() != L'/') p += L'/';
            folders.try_emplace(p);
        }
        add_ancestors(norm, e);
    }

    auto is_visible = [&](const std::wstring& norm) -> bool {
        size_t pos = 0;
        while (true) {
            size_t sl = norm.find(L'/', pos);
            if (sl == std::wstring::npos) return true;
            std::wstring folder = norm.substr(0, sl + 1);
            if (folder == norm) return true;
            if (expanded_folders_.find(folder) == expanded_folders_.end())
                return false;
            pos = sl + 1;
        }
    };

    auto leaf_of = [](const std::wstring& norm_no_slash) {
        auto sl = norm_no_slash.find_last_of(L'/');
        return (sl == std::wstring::npos) ? norm_no_slash
                                          : norm_no_slash.substr(sl + 1);
    };
    auto depth_of = [](const std::wstring& norm_no_slash) {
        return static_cast<int>(std::count(norm_no_slash.begin(),
                                           norm_no_slash.end(), L'/'));
    };

    struct Item {
        std::wstring key;
        bool is_dir;
        const openzip::Extractor::Entry* entry;
        const FolderAgg*                 agg;
    };
    std::vector<Item> items;
    items.reserve(entries_.size() + folders.size());

    std::set<std::wstring> seen_folder_keys;
    for (const auto& e : entries_) {
        std::wstring norm = NormalizePath(e.name);
        if (e.is_dir) {
            while (!norm.empty() && norm.back() == L'/') norm.pop_back();
            if (norm.empty()) continue;
            seen_folder_keys.insert(norm);
            auto it = folders.find(norm + L'/');
            items.push_back({norm, true, &e, it != folders.end() ? &it->second : nullptr});
        } else {
            items.push_back({norm, false, &e, nullptr});
        }
    }
    for (const auto& [folder_with_slash, agg] : folders) {
        std::wstring key = folder_with_slash;
        while (!key.empty() && key.back() == L'/') key.pop_back();
        if (key.empty()) continue;
        if (seen_folder_keys.count(key)) continue;
        items.push_back({key, true, nullptr, &agg});
    }

    // Hierarchical sort that walks both items' paths in parallel,
    // comparing one segment at a time. At each level:
    //   1. Folders before files (always — independent of asc/desc).
    //   2. Same type → compare by the chosen column.
    //   3. Equal at this level → descend into the next segment, OR if one
    //      side has no more segments, that side is the ancestor (DFS-first).
    //
    // The column value for an *ancestor* segment is the folder aggregate
    // (sum of descendant sizes / latest of descendant timestamps), looked
    // up from the `folders` map built earlier. For the *last* segment of
    // a file row, the value is the entry's own size/time. This keeps
    // sibling folders comparable to sibling files at the same level when
    // sorting by Size or Date.
    //
    // sort_descending_ inverts ONLY the sibling order at the level where
    // the first non-equal comparison happens — the parent-before-child
    // relationship (DFS order) and folders-before-files invariant are not
    // affected by the direction toggle.
    auto seg_value = [&](const Item& it, size_t pos, size_t end_pos,
                         bool is_folder_at_level) -> uint64_t {
        if (sort_column_ == 0) return 0;  // name sort uses the segment string
        if (is_folder_at_level) {
            std::wstring path = it.key.substr(0, end_pos) + L'/';
            auto fit = folders.find(path);
            if (fit == folders.end()) return 0;
            const FolderAgg& agg = fit->second;
            switch (sort_column_) {
                case 1: return agg.uncompressed_total;
                case 2: return agg.compressed_total;
                case 3: return static_cast<uint64_t>(agg.latest_mtime);
                case 4: return static_cast<uint64_t>(agg.latest_ctime);
            }
        } else if (it.entry) {
            switch (sort_column_) {
                case 1: return it.entry->uncompressed_size;
                case 2: return it.entry->compressed_size;
                case 3: return static_cast<uint64_t>(it.entry->modified_time);
                case 4: return static_cast<uint64_t>(it.entry->created_time);
            }
        }
        return 0;
        (void)pos;
    };

    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        size_t pos_a = 0, pos_b = 0;
        for (;;) {
            size_t sl_a = a.key.find(L'/', pos_a);
            size_t sl_b = b.key.find(L'/', pos_b);
            bool last_a = (sl_a == std::wstring::npos);
            bool last_b = (sl_b == std::wstring::npos);
            std::wstring seg_a = a.key.substr(pos_a, last_a ? std::wstring::npos : sl_a - pos_a);
            std::wstring seg_b = b.key.substr(pos_b, last_b ? std::wstring::npos : sl_b - pos_b);

            bool a_folder = !last_a || a.is_dir;
            bool b_folder = !last_b || b.is_dir;
            if (a_folder != b_folder) return a_folder;  // folders first, both directions

            int cmp = CSTR_EQUAL;
            if (sort_column_ == 0) {
                cmp = ::CompareStringOrdinal(seg_a.c_str(), -1,
                                             seg_b.c_str(), -1, TRUE);
            } else {
                size_t end_a = last_a ? a.key.size() : sl_a;
                size_t end_b = last_b ? b.key.size() : sl_b;
                uint64_t va = seg_value(a, pos_a, end_a, a_folder);
                uint64_t vb = seg_value(b, pos_b, end_b, b_folder);
                if      (va < vb) cmp = CSTR_LESS_THAN;
                else if (va > vb) cmp = CSTR_GREATER_THAN;
                else cmp = ::CompareStringOrdinal(seg_a.c_str(), -1,
                                                  seg_b.c_str(), -1, TRUE);
            }

            if (cmp != CSTR_EQUAL) {
                return sort_descending_ ? (cmp == CSTR_GREATER_THAN)
                                        : (cmp == CSTR_LESS_THAN);
            }

            // Equal segment at this level — DFS: the side that ends here
            // is the ancestor and sorts first regardless of direction.
            if (last_a && last_b) return false;
            if (last_a) return true;
            if (last_b) return false;
            pos_a = sl_a + 1;
            pos_b = sl_b + 1;
        }
    });

    visible_rows_.reserve(items.size());
    for (const auto& it : items) {
        std::wstring norm_with_marker = it.key + (it.is_dir ? L"/" : L"");
        if (!is_visible(norm_with_marker)) continue;

        Row r;
        r.full_path = it.key;
        r.leaf      = leaf_of(it.key);
        r.depth     = depth_of(it.key);
        r.is_dir    = it.is_dir;
        r.is_expanded = it.is_dir
            && expanded_folders_.find(it.key + L"/") != expanded_folders_.end();
        if (it.is_dir) {
            if (it.agg) {
                r.uncompressed_size = it.agg->uncompressed_total;
                r.compressed_size   = it.agg->compressed_total;
                r.modified_time     = it.agg->latest_mtime;
                r.created_time      = it.agg->latest_ctime;
            }
        } else if (it.entry) {
            r.uncompressed_size = it.entry->uncompressed_size;
            r.compressed_size   = it.entry->compressed_size;
            r.modified_time     = it.entry->modified_time;
            r.created_time      = it.entry->created_time;
        }
        visible_rows_.push_back(std::move(r));
    }
}

// ─── List rendering (rows are inserted blank in column 0; custom-draw
//     paints arrow + icon + label) ───────────────────────────────────

void CArchiveBrowserDialog::RenderRows() {
    list_.SetRedraw(FALSE);
    list_.DeleteAllItems();

    int row = 0;
    for (const auto& r : visible_rows_) {
        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_PARAM;
        item.iItem    = row;
        item.iSubItem = 0;
        item.pszText  = const_cast<LPWSTR>(L"");  // custom-draw fills this
        item.lParam   = static_cast<LPARAM>(row);
        list_.InsertItem(&item);

        list_.SetItemText(row, 1, FormatSize(r.uncompressed_size).c_str());
        list_.SetItemText(row, 2, FormatSize(r.compressed_size).c_str());
        list_.SetItemText(row, 3, FormatDateTime(r.modified_time).c_str());
        list_.SetItemText(row, 4, FormatDateTime(r.created_time).c_str());
        ++row;
    }

    // Common Controls auto-selects + auto-focuses the first inserted item
    // in some configurations. Clear that to start with no selection — the
    // user picks what they want.
    list_.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);

    list_.SetRedraw(TRUE);
    list_.Invalidate();
    UpdateButtonLabel();
}

// ─── Custom-draw: arrow + icon + label in column 0 ───────────────────

void CArchiveBrowserDialog::OnListCustomDraw(NMHDR* hdr, LRESULT* result) {
    auto* nmcd = reinterpret_cast<LPNMLVCUSTOMDRAW>(hdr);
    *result = CDRF_DODEFAULT;
    const bool dark = openzip::dark_theme::IsDarkModeActive();

    // Owner-draw palette per mode — same code path runs in both, only the
    // colors differ. This keeps dark/light visually consistent with
    // flat_button's bimodal paint on the action buttons.
    const COLORREF body_bg     = dark ? kListBodyDark      : kListBodyLight;
    const COLORREF body_fg     = dark ? kListFgDark        : kListFgLight;
    const COLORREF sel_foc_bg  = dark ? kListSelFocBgDark  : kListSelFocBgLight;
    const COLORREF sel_foc_fg  = dark ? kListSelFocFgDark  : kListSelFocFgLight;
    const COLORREF sel_unf_bg  = dark ? kListSelUnfBgDark  : kListSelUnfBgLight;
    const COLORREF sel_unf_fg  = dark ? kListSelUnfFgDark  : kListSelUnfFgLight;

    switch (nmcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: {
            // Paint the listview's body backdrop ourselves in both modes.
            // ListView_SetBkColor *should* do this, but on Win11 with
            // DarkMode_Explorer + LVS_EX_DOUBLEBUFFER the buffered-paint
            // code path inside the common-control DLL uses theme colors
            // and ignores LVM_SETBKCOLOR for the row strip. Filling here
            // happens inside the same buffered DC the items will be drawn
            // into, so it covers both the gaps between rows and the
            // empty space below the last row before any item paints over.
            RECT rc;
            ::GetClientRect(list_.GetSafeHwnd(), &rc);
            HBRUSH br = ::CreateSolidBrush(body_bg);
            ::FillRect(nmcd->nmcd.hdc, &rc, br);
            ::DeleteObject(br);
            *result = CDRF_NOTIFYITEMDRAW;
            return;
        }
        case CDDS_ITEMPREPAINT: {
            // Per-row clrText / clrTextBk override the theme palette in
            // the listview's built-in subitem-draw path; without this the
            // default-drawn columns 1..4 pick up the theme's window color
            // (system white in dark mode, system white in light mode also,
            // but with our explicit palette the selection colors stay
            // consistent across modes).
            int row = static_cast<int>(nmcd->nmcd.dwItemSpec);
            UINT lvis = list_.GetItemState(row, LVIS_SELECTED);
            bool selected = (lvis & LVIS_SELECTED) != 0;
            bool focused  = (::GetFocus() == list_.GetSafeHwnd());
            if (selected) {
                nmcd->clrText   = focused ? sel_foc_fg : sel_unf_fg;
                nmcd->clrTextBk = focused ? sel_foc_bg : sel_unf_bg;
            } else {
                nmcd->clrText   = body_fg;
                nmcd->clrTextBk = body_bg;
            }
            *result = CDRF_NEWFONT | CDRF_NOTIFYSUBITEMDRAW;
            return;
        }
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            if (nmcd->iSubItem != 0) return;  // default for other columns

            int row = static_cast<int>(nmcd->nmcd.dwItemSpec);
            if (row < 0 || row >= static_cast<int>(visible_rows_.size())) return;
            const Row& r = visible_rows_[row];

            HDC dc = nmcd->nmcd.hdc;

            // Column-0 cell rect. nmcd->nmcd.rc is unreliable for SubItem 0
            // (empty when the item has no LVIF_IMAGE/LVIF_TEXT data, or the
            // whole row in some Common Controls versions). Compute it from
            // the row rect + column-0 width to be safe.
            RECT rc;
            if (!list_.GetItemRect(row, &rc, LVIR_BOUNDS)) return;
            rc.right = rc.left + list_.GetColumnWidth(0);

            // Selected state — query the listview directly. NMCUSTOMDRAW's
            // uItemState bits (CDIS_SELECTED etc) are not reliable for
            // ListView controls; LVM_GETITEMSTATE is the safe path.
            UINT lvis = list_.GetItemState(row, LVIS_SELECTED);
            bool selected = (lvis & LVIS_SELECTED) != 0;
            bool focused  = (::GetFocus() == list_.GetSafeHwnd());
            // Use the same palette variables computed at the top of the
            // function so column 0 stays in sync with the default-drawn
            // columns 1..4 (clrText/clrTextBk in CDDS_ITEMPREPAINT) AND
            // with the body fill in CDDS_PREPAINT, in both modes.
            COLORREF bg, fg;
            if (selected) {
                if (focused) { bg = sel_foc_bg; fg = sel_foc_fg; }
                else         { bg = sel_unf_bg; fg = sel_unf_fg; }
            } else {
                bg = body_bg;
                fg = body_fg;
            }

            HBRUSH bgBrush = ::CreateSolidBrush(bg);
            ::FillRect(dc, &rc, bgBrush);
            ::DeleteObject(bgBrush);

            int x = rc.left + 4 + r.depth * kIndentPerDepth;

            // Arrow (folders only). Use a triangle char from Segoe UI Symbol
            // via DrawText for a clean, themed look.
            if (r.is_dir) {
                RECT ar = { x, rc.top, x + kArrowWidth, rc.bottom };
                ::SetBkMode(dc, TRANSPARENT);
                ::SetTextColor(dc, fg);
                const wchar_t* glyph = r.is_expanded ? L"▾" : L"▸";  // ▾ ▸
                ::DrawTextW(dc, glyph, 1, &ar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            x += kArrowWidth;

            // Icon (system imagelist).
            int icon_idx = r.is_dir ? LookupShellIconIndex(L"folder", true)
                                    : LookupShellIconIndex(r.leaf, false);
            if (sys_images_) {
                int iy = rc.top + ((rc.bottom - rc.top) - icon_h_) / 2;
                ::ImageList_Draw(sys_images_, icon_idx, dc, x, iy,
                                 ILD_TRANSPARENT);
            }
            x += kIconWidth;
            x += kLabelGap;

            // Label.
            RECT lr = { x, rc.top, rc.right - 4, rc.bottom };
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, fg);
            ::DrawTextW(dc, r.leaf.c_str(), -1, &lr,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS
                        | DT_NOPREFIX);

            *result = CDRF_SKIPDEFAULT;
            return;
        }
    }
}

void CArchiveBrowserDialog::UpdateButtonLabel() {
    int real_selected = 0;
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    while (sel >= 0) {
        ++real_selected;
        sel = list_.GetNextItem(sel, LVNI_SELECTED);
    }
    CString s;
    s.LoadString(real_selected > 0 ? IDS_BUTTON_EXTRACT_SELECTED
                                   : IDS_BUTTON_EXTRACT_ALL);
    SetDlgItemText(IDC_BROWSE_EXTRACT_ALL, s);
}

void CArchiveBrowserDialog::ToggleFolderAt(int row_index) {
    if (row_index < 0 || row_index >= static_cast<int>(visible_rows_.size())) return;
    const Row& r = visible_rows_[row_index];
    if (!r.is_dir) return;
    std::wstring key = r.full_path + L"/";
    if (expanded_folders_.find(key) == expanded_folders_.end())
        expanded_folders_.insert(key);
    else
        expanded_folders_.erase(key);
    RebuildVisibleRows();
    RenderRows();
}

bool CArchiveBrowserDialog::PickDestinationFolder(std::wstring& out) {
    BROWSEINFOW bi{};
    bi.hwndOwner = GetSafeHwnd();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CString title; title.LoadString(IDS_PICK_DESTINATION);
    bi.lpszTitle = title;
    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {};
    bool ok = (::SHGetPathFromIDListW(pidl, path) != FALSE);
    ::CoTaskMemFree(pidl);
    if (!ok) return false;
    out = path;
    return true;
}

void CArchiveBrowserDialog::ExpandRowToEntryNames(
        const std::wstring& row_full_path, bool row_is_dir,
        std::vector<std::wstring>& out_set) const {
    if (!row_is_dir) {
        for (const auto& e : entries_) {
            if (NormalizePath(e.name) == row_full_path) {
                out_set.push_back(e.name);
                return;
            }
        }
        out_set.push_back(row_full_path);
        return;
    }
    std::wstring prefix = row_full_path;
    if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';
    for (const auto& e : entries_) {
        std::wstring norm = NormalizePath(e.name);
        if (norm == row_full_path || norm == prefix || norm.rfind(prefix, 0) == 0) {
            out_set.push_back(e.name);
        }
    }
}

void CArchiveBrowserDialog::OnExtractAll() {
    std::wstring dest;
    if (!PickDestinationFolder(dest)) return;

    std::vector<std::wstring> filter;
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    while (sel >= 0) {
        if (sel < static_cast<int>(visible_rows_.size())) {
            const Row& r = visible_rows_[sel];
            ExpandRowToEntryNames(r.full_path, r.is_dir, filter);
        }
        sel = list_.GetNextItem(sel, LVNI_SELECTED);
    }
    if (!filter.empty()) {
        std::set<std::wstring> uniq(filter.begin(), filter.end());
        filter.assign(uniq.begin(), uniq.end());
    }
    chosen_extract_dir  = dest;
    chosen_filter_names = std::move(filter);
    EndDialog(IDOK);
}

void CArchiveBrowserDialog::OnListClick(NMHDR* hdr, LRESULT* result) {
    auto* nia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
    *result = 0;
    if (!nia || nia->iItem < 0) return;
    if (nia->iItem >= static_cast<int>(visible_rows_.size())) return;
    const Row& r = visible_rows_[nia->iItem];
    if (!r.is_dir) return;

    // Detect click in the chevron zone. We need the row's left edge in
    // listview client coords; LVIR_BOUNDS on item 0 returns the whole row,
    // which is fine here — only the left edge matters.
    RECT rc;
    if (!list_.GetItemRect(nia->iItem, &rc, LVIR_BOUNDS)) return;
    int arrow_left  = rc.left + 4 + r.depth * kIndentPerDepth;
    int arrow_right = arrow_left + kArrowWidth;
    if (nia->ptAction.x >= arrow_left && nia->ptAction.x < arrow_right) {
        ToggleFolderAt(nia->iItem);
    }
}

void CArchiveBrowserDialog::OnListDoubleClick(NMHDR* hdr, LRESULT* result) {
    auto* nia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
    *result = 0;
    if (!nia || nia->iItem < 0) return;
    if (nia->iItem >= static_cast<int>(visible_rows_.size())) return;
    const Row& r = visible_rows_[nia->iItem];
    if (r.is_dir) {
        ToggleFolderAt(nia->iItem);
        return;
    }

    // File row: extract just this entry into a private temp directory and
    // hand it to the shell's default handler. This matches what users expect
    // from a Windows archive browser — Explorer's built-in zip browser, 7-Zip
    // File Manager, Bandizip, etc. all behave the same way.
    std::vector<std::wstring> filter;
    ExpandRowToEntryNames(r.full_path, /*row_is_dir=*/false, filter);
    auto temp_dir = ExtractEntriesToTempModal(filter);
    if (temp_dir.empty()) return;  // user cancelled / extract failed

    // The extractor preserves the entry's relative path inside target_dir,
    // so the actual file is at temp_dir / r.full_path. Use ShellExecuteW
    // with no verb (= default action — usually "open"). SW_SHOWNORMAL lets
    // image viewers / editors take focus.
    auto file_path = temp_dir / std::filesystem::path(r.full_path);
    ::ShellExecuteW(GetSafeHwnd(), nullptr, file_path.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
}

void CArchiveBrowserDialog::OnListRClick(NMHDR* hdr, LRESULT* result) {
    auto* nia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
    *result = 0;

    // Explorer convention: right-click on an unselected row makes it the
    // selection. Right-click on already-selected items keeps the existing
    // multi-selection. Right-click on empty space leaves selection alone
    // (the menu falls back to "모두 풀기" via UpdateButtonLabel logic).
    if (nia && nia->iItem >= 0 &&
        !(list_.GetItemState(nia->iItem, LVIS_SELECTED) & LVIS_SELECTED)) {
        list_.SetItemState(-1, 0, LVIS_SELECTED);
        list_.SetItemState(nia->iItem, LVIS_SELECTED, LVIS_SELECTED);
    }

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    CString label;
    label.LoadString(list_.GetSelectedCount() > 0 ? IDS_BUTTON_EXTRACT_SELECTED
                                                  : IDS_BUTTON_EXTRACT_ALL);
    ::AppendMenuW(menu, MF_STRING, 1, label);

    POINT pt;
    ::GetCursorPos(&pt);
    UINT cmd = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, GetSafeHwnd(), nullptr);
    ::DestroyMenu(menu);

    if (cmd == 1) OnExtractAll();
}

void CArchiveBrowserDialog::OnListItemChanged(NMHDR* hdr, LRESULT* result) {
    auto* nlv = reinterpret_cast<NMLISTVIEW*>(hdr);
    *result = 0;
    if (!nlv) return;
    if ((nlv->uChanged & LVIF_STATE) == 0) return;
    bool was = (nlv->uOldState & LVIS_SELECTED) != 0;
    bool now = (nlv->uNewState & LVIS_SELECTED) != 0;
    if (was != now) UpdateButtonLabel();
}

void CArchiveBrowserDialog::OnListColumnClick(NMHDR* hdr, LRESULT* result) {
    auto* nlv = reinterpret_cast<NMLISTVIEW*>(hdr);
    *result = 0;
    if (!nlv) return;
    int col = nlv->iSubItem;
    if (col < 0 || col > 4) return;

    // Same column → toggle direction; new column → start ascending.
    if (col == sort_column_) {
        sort_descending_ = !sort_descending_;
    } else {
        sort_column_     = col;
        sort_descending_ = false;
    }

    RebuildVisibleRows();
    RenderRows();
    UpdateSortIndicator();
}

// ─── Temp extraction + drag-out plumbing ─────────────────────────────

void CArchiveBrowserDialog::CleanupOldPreviewDirs() {
    auto root = PreviewRoot();
    if (root.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;

    using clock = std::filesystem::file_time_type::clock;
    auto cutoff = clock::now() - std::chrono::hours(24);

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        std::error_code item_ec;
        auto last_write = std::filesystem::last_write_time(entry, item_ec);
        if (item_ec) continue;
        if (last_write > cutoff) continue;
        // Best-effort: an in-use preview dir (file the user is still viewing)
        // will fail to delete and we'll retry next launch — that's fine.
        std::filesystem::remove_all(entry.path(), item_ec);
    }
}

std::filesystem::path CArchiveBrowserDialog::ExtractEntriesToTempModal(
        const std::vector<std::wstring>& entry_names) {
    if (entry_names.empty()) return {};
    auto dir = MakeFreshPreviewDir();
    if (dir.empty()) return {};

    // Re-use the existing extract dialog so the user gets progress, password,
    // and conflict handling for free. CommandLine's threads / password fields
    // start empty, which is what we want — a fresh prompt if needed.
    openzip::CommandLine cmd;
    cmd.kind       = openzip::JobKind::Extract;
    cmd.zip_path   = zip_path;
    cmd.target_dir = dir;

    CExtractDialog extract(cmd, this);
    extract.include_filter = entry_names;
    INT_PTR rc = extract.DoModal();
    if (rc != IDOK ||
        extract.result() != openzip::Extractor::Result::Success) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return {};
    }
    return dir;
}

void CArchiveBrowserDialog::OnListBeginDrag(NMHDR* hdr, LRESULT* result) {
    auto* nlv = reinterpret_cast<NMLISTVIEW*>(hdr);
    *result = 0;
    if (!nlv) return;

    // Build the entry-name filter from the current selection. If the user
    // started the drag on an unselected row, the listview ensures `iItem`
    // is selected before LVN_BEGINDRAG fires — we follow the live selection
    // state (consistent with right-click behavior elsewhere in this dialog).
    std::vector<std::wstring> filter;
    std::vector<std::wstring> top_relpaths;  // for the CF_HDROP file list
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    while (sel >= 0) {
        if (sel < static_cast<int>(visible_rows_.size())) {
            const Row& r = visible_rows_[sel];
            ExpandRowToEntryNames(r.full_path, r.is_dir, filter);
            // The CF_HDROP roots — one absolute path per *selected* row,
            // pointing to its top-level extracted file or folder. Drop
            // targets see "drag of N items" rather than "drag of every leaf".
            top_relpaths.push_back(r.full_path);
        }
        sel = list_.GetNextItem(sel, LVNI_SELECTED);
    }
    if (filter.empty()) return;
    // De-dup in case the user selected both a folder and one of its children.
    std::set<std::wstring> uniq(filter.begin(), filter.end());
    filter.assign(uniq.begin(), uniq.end());

    // Capture state by value so the callback survives even after this stack
    // frame returns. `this` is captured raw — its lifetime spans DoDragDrop,
    // and the data object is Released before we return below.
    auto extract_cb = [this, filter = std::move(filter),
                       top_relpaths = std::move(top_relpaths)]
                      (std::vector<std::wstring>* out_paths) -> bool {
        // Runs on the FIRST GetData(CF_HDROP) — i.e. when the user has
        // dropped on a willing target. CExtractDialog handles password
        // prompts, conflict prompts (won't fire on a fresh temp dir, but
        // safe to keep), and shows progress for large extractions.
        auto dir = ExtractEntriesToTempModal(filter);
        if (dir.empty()) return false;
        out_paths->reserve(top_relpaths.size());
        for (const auto& rel : top_relpaths) {
            out_paths->push_back(
                (dir / std::filesystem::path(rel)).wstring());
        }
        return true;
    };

    auto* data = new CHDropDataObject(std::move(extract_cb));
    auto* src  = new CDropSource();

    DWORD effect = 0;
    HRESULT hr = ::DoDragDrop(data, src, DROPEFFECT_COPY, &effect);
    (void)hr;
    data->Release();
    src->Release();
    // The temp directory created by the lazy callback is NOT removed here —
    // Explorer and other targets read the source files asynchronously after
    // GetData returns. Cleanup happens at the next dialog launch via
    // CleanupOldPreviewDirs (24h window), which is plenty of time.
}

void CArchiveBrowserDialog::UpdateSortIndicator() {
    HWND hdr = ListView_GetHeader(list_.GetSafeHwnd());
    if (!hdr) return;
    int count = static_cast<int>(::SendMessageW(hdr, HDM_GETITEMCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        HDITEMW hdi{};
        hdi.mask = HDI_FORMAT;
        if (!::SendMessageW(hdr, HDM_GETITEMW, i,
                            reinterpret_cast<LPARAM>(&hdi))) continue;
        hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sort_column_) {
            hdi.fmt |= sort_descending_ ? HDF_SORTDOWN : HDF_SORTUP;
        }
        ::SendMessageW(hdr, HDM_SETITEMW, i,
                       reinterpret_cast<LPARAM>(&hdi));
    }
    // Force the dark-mode header subclass to repaint with the new flags;
    // HDM_SETITEMW alone doesn't always invalidate when only fmt changes.
    ::InvalidateRect(hdr, nullptr, TRUE);
}

// ─── Resize handling ─────────────────────────────────────────────────

void CArchiveBrowserDialog::OnSize(UINT nType, int cx, int cy) {
    CDialogEx::OnSize(nType, cx, cy);
    // OnSize fires before child controls are wired during dialog init; bail
    // out if the listview isn't ready yet.
    if (!list_.GetSafeHwnd()) return;
    RelayoutChildren(cx, cy);
}

void CArchiveBrowserDialog::OnGetMinMaxInfo(MINMAXINFO* mmi) {
    if (mmi) {
        mmi->ptMinTrackSize.x = 540;
        mmi->ptMinTrackSize.y = 320;
    }
    CDialogEx::OnGetMinMaxInfo(mmi);
}

void CArchiveBrowserDialog::RelayoutChildren(int cx, int cy) {
    constexpr int margin       = 12;
    constexpr int top_label_h  = 18;
    constexpr int top_gap      = 6;
    constexpr int bottom_gap   = 8;
    constexpr int btn_h        = 24;
    constexpr int btn_w_extract = 110;
    constexpr int btn_w_close  = 80;
    constexpr int btn_gap      = 6;

    int btn_y    = cy - margin - btn_h;
    int close_x  = cx - margin - btn_w_close;
    int extract_x = close_x - btn_gap - btn_w_extract;
    int list_top    = margin + top_label_h + top_gap;
    int list_bottom = btn_y - bottom_gap;
    int list_h      = list_bottom - list_top;
    if (list_h < 60) list_h = 60;

    // Move all children atomically with SWP_NOCOPYBITS.
    //
    // The earlier MoveWindow-per-child approach left a faint stale-pixel
    // trail along the Extract button's right edge during a continuous
    // shrink-drag: when the dialog narrowed faster than btn_gap (6 px) per
    // WM_SIZE, the close button's NEW bounds momentarily overlapped the
    // Extract button's OLD bounds, and Windows' bit-blit optimization
    // pulled those old Extract pixels into the close button surface before
    // the Extract button itself moved. RDW_FRAME | RDW_UPDATENOW couldn't
    // fix it — the artifact was already laid down by the bit-blit before
    // any WM_PAINT ran.
    //
    // BeginDeferWindowPos batches the moves so they apply as a single
    // operation; SWP_NOCOPYBITS forces full invalidation of each child's
    // new bounds instead of bit-blitting from the old position, which is
    // what eliminates the ghost. The synchronous RedrawWindow on the two
    // buttons afterwards still pushes the flat-button paint through before
    // the next WM_SIZE arrives during a continuous drag, so the user sees
    // a clean frame each step.
    HDWP hdwp = ::BeginDeferWindowPos(4);
    constexpr UINT swp = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS;
    if (auto* w = GetDlgItem(IDC_BROWSE_ZIPPATH)) {
        hdwp = ::DeferWindowPos(hdwp, w->GetSafeHwnd(), nullptr,
                                margin, margin, cx - 2 * margin, top_label_h,
                                swp);
    }
    if (auto* w = GetDlgItem(IDC_BROWSE_LIST)) {
        hdwp = ::DeferWindowPos(hdwp, w->GetSafeHwnd(), nullptr,
                                margin, list_top, cx - 2 * margin, list_h,
                                swp);
    }
    if (auto* w = GetDlgItem(IDC_BROWSE_EXTRACT_ALL)) {
        hdwp = ::DeferWindowPos(hdwp, w->GetSafeHwnd(), nullptr,
                                extract_x, btn_y, btn_w_extract, btn_h,
                                swp);
    }
    if (auto* w = GetDlgItem(IDCANCEL)) {
        hdwp = ::DeferWindowPos(hdwp, w->GetSafeHwnd(), nullptr,
                                close_x, btn_y, btn_w_close, btn_h,
                                swp);
    }
    ::EndDeferWindowPos(hdwp);

    auto repaint_btn = [](CWnd* w) {
        if (!w) return;
        ::RedrawWindow(w->GetSafeHwnd(), nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW);
    };
    repaint_btn(GetDlgItem(IDC_BROWSE_EXTRACT_ALL));
    repaint_btn(GetDlgItem(IDCANCEL));

    // Repaint the dialog backdrop so the 1-px listview-pane frame drawn
    // in OnPaint follows the listview to its new bounds. WS_CLIPCHILDREN
    // clips the listview itself out of the dialog's paint region, so
    // this only redraws the strip around the listview.
    ::InvalidateRect(GetSafeHwnd(), nullptr, FALSE);
    ::UpdateWindow(GetSafeHwnd());
}

void CArchiveBrowserDialog::OnPaint() {
    CPaintDC dc(this);

    // After CDialogEx fills the backdrop (system COLOR_3DFACE in light,
    // dark brush via OnCtlColor in dark), draw a 1px outline around the
    // listview's bounds in light mode so the white listview pane has a
    // defined edge against the gray dialog backdrop. Without this the
    // listview's white body bleeds into nothing — the user can't tell
    // where the pane ends and the dialog begins. Dark mode doesn't need
    // this: the body→backdrop color tier (38 vs 32) already provides
    // the boundary cue.
    CDialogEx::OnPaint();
    if (openzip::dark_theme::IsDarkModeActive()) return;
    if (!list_.GetSafeHwnd()) return;

    RECT lr;
    list_.GetWindowRect(&lr);
    ::ScreenToClient(GetSafeHwnd(),
                     reinterpret_cast<LPPOINT>(&lr.left));
    ::ScreenToClient(GetSafeHwnd(),
                     reinterpret_cast<LPPOINT>(&lr.right));

    HPEN pen     = ::CreatePen(PS_SOLID, 1, RGB(213, 213, 213));
    HPEN old_pen = static_cast<HPEN>(::SelectObject(dc.GetSafeHdc(), pen));
    HBRUSH old_br = static_cast<HBRUSH>(
        ::SelectObject(dc.GetSafeHdc(), ::GetStockObject(NULL_BRUSH)));
    ::Rectangle(dc.GetSafeHdc(), lr.left - 1, lr.top - 1,
                                 lr.right + 1, lr.bottom + 1);
    ::SelectObject(dc.GetSafeHdc(), old_pen);
    ::SelectObject(dc.GetSafeHdc(), old_br);
    ::DeleteObject(pen);
}

HBRUSH CArchiveBrowserDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
    UINT msg;
    switch (nCtlColor) {
        case CTLCOLOR_EDIT:   msg = WM_CTLCOLOREDIT;   break;
        case CTLCOLOR_STATIC: msg = WM_CTLCOLORSTATIC; break;
        case CTLCOLOR_BTN:    msg = WM_CTLCOLORBTN;    break;
        default:              msg = WM_CTLCOLORDLG;    break;
    }
    if (HBRUSH b = openzip::dark_theme::OnCtlColor(
            pWnd->GetSafeHwnd(), pDC->GetSafeHdc(), msg))
        return b;
    return CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
}
