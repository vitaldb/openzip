#include "stdafx.h"
#include "ArchiveBrowserDialog.h"

#include "core/extractor.h"

#include <locale>

namespace {

// Format a byte count with thousands separators (e.g. 1,234,567).
// Returns L"—" (em dash) for directories or zero-sized entries marked dir.
std::wstring FormatSize(uint64_t bytes) {
    // Manual thousands-separator insertion to avoid locale clobbering.
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

}  // namespace

IMPLEMENT_DYNAMIC(CArchiveBrowserDialog, CDialogEx)

CArchiveBrowserDialog::CArchiveBrowserDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent) {}

BEGIN_MESSAGE_MAP(CArchiveBrowserDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BROWSE_EXTRACT_ALL, &CArchiveBrowserDialog::OnExtractAll)
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

void CArchiveBrowserDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_BROWSE_LIST, list_);
}

BOOL CArchiveBrowserDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    // Apply dark theme (titlebar + child windows).
    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
    }

    // Localize caption and button labels.
    CString s;
    s.LoadString(IDS_DIALOG_BROWSE_TITLE);  SetWindowText(s);
    s.LoadString(IDS_BUTTON_EXTRACT_ALL);   SetDlgItemText(IDC_BROWSE_EXTRACT_ALL, s);
    s.LoadString(IDS_BUTTON_CLOSE);         SetDlgItemText(IDCANCEL, s);

    // Show the zip path in the static label.
    SetDlgItemText(IDC_BROWSE_ZIPPATH, zip_path.wstring().c_str());

    // Configure list view extended styles.
    list_.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

    // Add columns.
    CString colName, colSize;
    colName.LoadString(IDS_LABEL_BROWSE_NAME);
    colSize.LoadString(IDS_LABEL_BROWSE_SIZE);
    list_.InsertColumn(0, colName, LVCFMT_LEFT,  300);
    list_.InsertColumn(1, colSize, LVCFMT_RIGHT,  80);

    PopulateList();

    return TRUE;
}

void CArchiveBrowserDialog::PopulateList() {
    auto entries = openzip::Extractor::ListEntries(zip_path);

    list_.SetRedraw(FALSE);
    list_.DeleteAllItems();

    int row = 0;
    for (const auto& e : entries) {
        std::wstring displayName = e.name;
        if (e.is_dir && !displayName.empty() && displayName.back() != L'\\') {
            displayName += L'\\';
        }

        list_.InsertItem(row, displayName.c_str());

        if (e.is_dir) {
            list_.SetItemText(row, 1, L"—");  // em dash for directories
        } else {
            list_.SetItemText(row, 1, FormatSize(e.uncompressed_size).c_str());
        }

        ++row;
    }

    list_.SetRedraw(TRUE);
    list_.Invalidate();
}

void CArchiveBrowserDialog::OnExtractAll() {
    EndDialog(IDOK);
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
