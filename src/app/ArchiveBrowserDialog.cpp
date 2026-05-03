#include "stdafx.h"
#include "ArchiveBrowserDialog.h"
#include "flat_button.h"

#include <algorithm>
#include <cmath>
#include <CommCtrl.h>
#include <ctime>
#include <map>
#include <set>
#include <shellapi.h>
#include <shlobj_core.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

// ─── Geometry ─────────────────────────────────────────────────────────
// Layout per row in the Name column:
//   [ depth*kIndentPerDepth ] [ kArrowWidth ] [ kIconWidth ] [ kLabelGap ] [ leaf text ]
// The arrow is drawn for folders only; files leave the arrow zone blank.
constexpr int kIndentPerDepth = 16;
constexpr int kArrowWidth     = 16;
constexpr int kIconWidth      = 18;
constexpr int kLabelGap       = 4;

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

}  // anonymous namespace

IMPLEMENT_DYNAMIC(CArchiveBrowserDialog, CDialogEx)

CArchiveBrowserDialog::CArchiveBrowserDialog(CWnd* p)
    : CDialogEx(IDD, p) {}

BEGIN_MESSAGE_MAP(CArchiveBrowserDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BROWSE_EXTRACT_ALL, &CArchiveBrowserDialog::OnExtractAll)
    ON_WM_CTLCOLOR()
    ON_WM_SIZE()
    ON_WM_GETMINMAXINFO()
    ON_NOTIFY(NM_CLICK,        IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListClick)
    ON_NOTIFY(NM_DBLCLK,       IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListItemChanged)
    ON_NOTIFY(NM_CUSTOMDRAW,   IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListCustomDraw)
    ON_NOTIFY(NM_RCLICK,       IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListRClick)
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

    entries_ = openzip::Extractor::ListEntries(zip_path);
    expanded_folders_.clear();

    RebuildVisibleRows();
    RenderRows();
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

    // Sort key: per-segment type marker + segment name, concatenated.
    //   "src/app/main.cpp" (file) →  \1 src  \1 app  \2 main.cpp
    //   "src"               (dir)  →  \1 src
    //   "src/app"           (dir)  →  \1 src  \1 app
    //   "README.md"         (file) →  \2 README.md
    //
    // The marker (\1 dir, \2 file) goes BEFORE each segment so:
    //   * same-parent siblings interleave correctly (parent prefix matches)
    //   * folders sort before files at every level (\1 < \2)
    //   * a folder's own row sorts before its descendants (parent key is a
    //     strict prefix of any descendant key, so it sorts first)
    // → produces a strict DFS pre-order: parent, then all its children,
    //   then the next sibling.
    auto sort_key = [](const Item& it) {
        std::wstring k;
        k.reserve(it.key.size() + 8);
        size_t pos = 0;
        for (;;) {
            size_t sl = it.key.find(L'/', pos);
            bool last = (sl == std::wstring::npos);
            // For non-last segments we're walking through ancestor directories;
            // for the last segment, the type comes from the item itself.
            wchar_t marker = (!last || it.is_dir) ? L'\x01' : L'\x02';
            k += marker;
            k += it.key.substr(pos, last ? std::wstring::npos : sl - pos);
            if (last) break;
            pos = sl + 1;
        }
        return k;
    };
    std::sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        std::wstring ka = sort_key(a);
        std::wstring kb = sort_key(b);
        return ::CompareStringOrdinal(ka.c_str(), -1, kb.c_str(), -1, TRUE)
               == CSTR_LESS_THAN;
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

    switch (nmcd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
            *result = CDRF_NOTIFYITEMDRAW;
            return;
        case CDDS_ITEMPREPAINT:
            *result = CDRF_NOTIFYSUBITEMDRAW;
            return;
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
            COLORREF bg, fg;
            if (selected) {
                bg = ::GetSysColor(focused ? COLOR_HIGHLIGHT : COLOR_BTNFACE);
                fg = ::GetSysColor(focused ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
            } else {
                bg = ::GetSysColor(COLOR_WINDOW);
                fg = ::GetSysColor(COLOR_WINDOWTEXT);
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
    std::wstring dest;
    if (!PickDestinationFolder(dest)) return;
    std::vector<std::wstring> filter;
    ExpandRowToEntryNames(r.full_path, /*row_is_dir=*/false, filter);
    chosen_extract_dir  = dest;
    chosen_filter_names = std::move(filter);
    EndDialog(IDOK);
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

    // Path label — top, full width minus margins.
    if (auto* w = GetDlgItem(IDC_BROWSE_ZIPPATH)) {
        w->MoveWindow(margin, margin, cx - 2 * margin, top_label_h);
    }

    // Buttons — bottom-right, fixed size.
    int btn_y    = cy - margin - btn_h;
    int close_x  = cx - margin - btn_w_close;
    int extract_x = close_x - btn_gap - btn_w_extract;
    if (auto* w = GetDlgItem(IDCANCEL))
        w->MoveWindow(close_x, btn_y, btn_w_close, btn_h);
    if (auto* w = GetDlgItem(IDC_BROWSE_EXTRACT_ALL))
        w->MoveWindow(extract_x, btn_y, btn_w_extract, btn_h);

    // List view — middle band, fills the rest.
    int list_top    = margin + top_label_h + top_gap;
    int list_bottom = btn_y - bottom_gap;
    int list_h      = list_bottom - list_top;
    if (list_h < 60) list_h = 60;
    list_.MoveWindow(margin, list_top, cx - 2 * margin, list_h);
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
