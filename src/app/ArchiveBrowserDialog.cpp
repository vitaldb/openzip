#include "stdafx.h"
#include "ArchiveBrowserDialog.h"
#include "flat_button.h"

#include <algorithm>
#include <map>
#include <set>
#include <shellapi.h>
#include <shlobj_core.h>

#pragma comment(lib, "shell32.lib")

namespace {

std::wstring FormatSize(uint64_t bytes) {
    wchar_t buf[32];
    ::_ui64tow_s(bytes, buf, 32, 10);
    std::wstring raw = buf;
    std::wstring result;
    result.reserve(raw.size() + raw.size() / 3 + 1);
    size_t digits_before_first_comma = raw.size() % 3;
    if (digits_before_first_comma == 0) digits_before_first_comma = 3;
    size_t i = 0;
    for (wchar_t ch : raw) {
        if (i == digits_before_first_comma && i != 0) {
            result += L',';
            digits_before_first_comma += 3;
        }
        result += ch;
        ++i;
    }
    return result;
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

// Per-row metadata stored in the listview's item LPARAM.
struct RowData {
    std::wstring name;   // segment without trailing slash; ".." for the parent row
    bool is_dir;
    bool is_parent;      // true only for the synthetic ".." row
    uint64_t size;       // 0 for dirs and parent rows
};

}  // namespace

IMPLEMENT_DYNAMIC(CArchiveBrowserDialog, CDialogEx)

CArchiveBrowserDialog::CArchiveBrowserDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent) {}

BEGIN_MESSAGE_MAP(CArchiveBrowserDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BROWSE_EXTRACT_ALL, &CArchiveBrowserDialog::OnExtractAll)
    ON_WM_CTLCOLOR()
    ON_NOTIFY(NM_DBLCLK,        IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED,  IDC_BROWSE_LIST, &CArchiveBrowserDialog::OnListItemChanged)
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

    CString s;
    s.LoadString(IDS_DIALOG_BROWSE_TITLE);  SetWindowText(s);
    s.LoadString(IDS_BUTTON_CLOSE);         SetDlgItemText(IDCANCEL, s);
    UpdateButtonLabel();  // sets the Extract All / Extract Selected button

    SetDlgItemText(IDC_BROWSE_ZIPPATH, zip_path.wstring().c_str());

    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    SHFILEINFOW sfi{};
    HIMAGELIST sysImages = reinterpret_cast<HIMAGELIST>(::SHGetFileInfoW(
        L"", 0, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES));
    if (sysImages) {
        ::SendMessageW(list_.GetSafeHwnd(), LVM_SETIMAGELIST, LVSIL_SMALL,
                       reinterpret_cast<LPARAM>(sysImages));
    }

    CString colName, colSize;
    colName.LoadString(IDS_LABEL_BROWSE_NAME);
    colSize.LoadString(IDS_LABEL_BROWSE_SIZE);
    list_.InsertColumn(0, colName, LVCFMT_LEFT,  300);
    list_.InsertColumn(1, colSize, LVCFMT_RIGHT,  80);

    entries_ = openzip::Extractor::ListEntries(zip_path);
    current_dir_.clear();

    PopulateList();
    return TRUE;
}

void CArchiveBrowserDialog::PopulateList() {
    list_.SetRedraw(FALSE);

    int existing = list_.GetItemCount();
    for (int i = 0; i < existing; ++i) {
        if (auto* d = reinterpret_cast<RowData*>(list_.GetItemData(i))) delete d;
    }
    list_.DeleteAllItems();

    std::map<std::wstring, RowData> shown;
    const std::wstring prefix = NormalizePath(current_dir_);
    for (const auto& e : entries_) {
        std::wstring path = NormalizePath(e.name);
        if (!prefix.empty()) {
            if (path.rfind(prefix, 0) != 0) continue;
            path = path.substr(prefix.size());
        }
        if (path.empty()) continue;

        auto sep = path.find(L'/');
        if (sep == std::wstring::npos) {
            shown[path] = RowData{path, e.is_dir, false, e.uncompressed_size};
        } else {
            std::wstring seg = path.substr(0, sep);
            auto it = shown.find(seg);
            if (it == shown.end()) {
                shown[seg] = RowData{seg, true, false, 0};
            } else {
                it->second.is_dir = true;
            }
        }
    }

    std::vector<RowData> rows;
    rows.reserve(shown.size() + 1);
    if (!current_dir_.empty()) {
        rows.push_back(RowData{L"..", true, true, 0});
    }
    std::vector<RowData> dirs, files;
    for (auto& [k, v] : shown) {
        if (v.is_dir) dirs.push_back(v);
        else          files.push_back(v);
    }
    auto byName = [](const RowData& a, const RowData& b) {
        return ::CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    };
    std::sort(dirs.begin(),  dirs.end(),  byName);
    std::sort(files.begin(), files.end(), byName);
    rows.insert(rows.end(), dirs.begin(),  dirs.end());
    rows.insert(rows.end(), files.begin(), files.end());

    int row = 0;
    for (const auto& r : rows) {
        std::wstring displayName = r.name;
        int iconIdx = r.is_dir ? LookupShellIconIndex(L"folder", true)
                               : LookupShellIconIndex(r.name, false);

        auto* rowData = new RowData(r);

        LVITEMW item{};
        item.mask     = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem    = row;
        item.iSubItem = 0;
        item.pszText  = const_cast<LPWSTR>(displayName.c_str());
        item.iImage   = iconIdx;
        item.lParam   = reinterpret_cast<LPARAM>(rowData);
        list_.InsertItem(&item);

        if (r.is_dir) list_.SetItemText(row, 1, L"");
        else          list_.SetItemText(row, 1, FormatSize(r.size).c_str());
        ++row;
    }

    list_.SetRedraw(TRUE);
    list_.Invalidate();
    UpdateButtonLabel();
}

void CArchiveBrowserDialog::NavigateInto(const std::wstring& folder_name) {
    current_dir_ += folder_name;
    current_dir_ += L'/';
    PopulateList();
}

void CArchiveBrowserDialog::NavigateUp() {
    if (current_dir_.empty()) return;
    auto path = current_dir_;
    if (!path.empty() && (path.back() == L'/' || path.back() == L'\\')) path.pop_back();
    auto sep = path.find_last_of(L"/\\");
    current_dir_ = (sep == std::wstring::npos) ? std::wstring() : path.substr(0, sep + 1);
    PopulateList();
}

void CArchiveBrowserDialog::UpdateButtonLabel() {
    // Count selected rows that are real (not the ".." parent navigation row).
    int real_selected = 0;
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    while (sel >= 0) {
        if (auto* r = reinterpret_cast<RowData*>(list_.GetItemData(sel))) {
            if (!r->is_parent) ++real_selected;
        }
        sel = list_.GetNextItem(sel, LVNI_SELECTED);
    }
    CString s;
    s.LoadString(real_selected > 0 ? IDS_BUTTON_EXTRACT_SELECTED : IDS_BUTTON_EXTRACT_ALL);
    SetDlgItemText(IDC_BROWSE_EXTRACT_ALL, s);
}

bool CArchiveBrowserDialog::PickDestinationFolder(std::wstring& out) {
    BROWSEINFOW bi{};
    bi.hwndOwner = GetSafeHwnd();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CString title;
    title.LoadString(IDS_PICK_DESTINATION);
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
        // File: include the entry whose decoded name matches (after slash
        // normalization). The decoded name uses '/' on its own; ZIP entries
        // historically can use either.
        for (const auto& e : entries_) {
            if (NormalizePath(e.name) == row_full_path) {
                out_set.push_back(e.name);
                return;
            }
        }
        // Fallback: still include the unnormalized form in case there's no
        // exact match (shouldn't happen since row was derived from entries_).
        out_set.push_back(row_full_path);
        return;
    }
    // Folder: include the folder entry (if any) plus every descendant entry.
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
    // Determine target folder first; if user cancels the picker, do nothing.
    std::wstring dest;
    if (!PickDestinationFolder(dest)) return;

    // Collect the include filter from the current selection (excluding "..").
    std::vector<std::wstring> filter;
    int sel = list_.GetNextItem(-1, LVNI_SELECTED);
    while (sel >= 0) {
        if (auto* r = reinterpret_cast<RowData*>(list_.GetItemData(sel))) {
            if (!r->is_parent) {
                std::wstring full = NormalizePath(current_dir_ + r->name);
                ExpandRowToEntryNames(full, r->is_dir, filter);
            }
        }
        sel = list_.GetNextItem(sel, LVNI_SELECTED);
    }

    // Dedupe.
    if (!filter.empty()) {
        std::set<std::wstring> uniq(filter.begin(), filter.end());
        filter.assign(uniq.begin(), uniq.end());
    }

    chosen_extract_dir  = dest;
    chosen_filter_names = std::move(filter);
    EndDialog(IDOK);
}

void CArchiveBrowserDialog::OnListDoubleClick(NMHDR* hdr, LRESULT* result) {
    auto* nia = reinterpret_cast<NMITEMACTIVATE*>(hdr);
    *result = 0;
    if (!nia || nia->iItem < 0) return;

    auto* r = reinterpret_cast<RowData*>(list_.GetItemData(nia->iItem));
    if (!r) return;

    if (r->is_parent) {
        NavigateUp();
        return;
    }
    if (r->is_dir) {
        NavigateInto(r->name);
        return;
    }
    // File: pick a destination and extract just this one entry.
    std::wstring dest;
    if (!PickDestinationFolder(dest)) return;

    std::wstring full = NormalizePath(current_dir_ + r->name);
    std::vector<std::wstring> filter;
    ExpandRowToEntryNames(full, /*row_is_dir=*/false, filter);

    chosen_extract_dir  = dest;
    chosen_filter_names = std::move(filter);
    EndDialog(IDOK);
}

void CArchiveBrowserDialog::OnListItemChanged(NMHDR* hdr, LRESULT* result) {
    auto* nlv = reinterpret_cast<NMLISTVIEW*>(hdr);
    *result = 0;
    if (!nlv) return;
    if ((nlv->uChanged & LVIF_STATE) == 0) return;
    // Only react when selection state actually flipped.
    bool was_selected = (nlv->uOldState & LVIS_SELECTED) != 0;
    bool is_selected  = (nlv->uNewState & LVIS_SELECTED) != 0;
    if (was_selected == is_selected) return;
    UpdateButtonLabel();
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
