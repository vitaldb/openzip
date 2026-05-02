#include "stdafx.h"
#include "ConflictDialog.h"
#include "flat_button.h"

IMPLEMENT_DYNAMIC(CConflictDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CConflictDialog, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_OVERWRITE, &CConflictDialog::OnOverwrite)
    ON_BN_CLICKED(IDC_BTN_SKIP,      &CConflictDialog::OnSkip)
    ON_BN_CLICKED(IDC_BTN_RENAME,    &CConflictDialog::OnRename)
    ON_WM_CTLCOLOR()
END_MESSAGE_MAP()

CConflictDialog::CConflictDialog(const CString& dest_path, CWnd* parent)
    : CDialogEx(IDD_CONFLICT, parent), dest_path_(dest_path) {}

void CConflictDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Check(pDX, IDC_CHECK_REMEMBER, remember_);
}

BOOL CConflictDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
    }
    openzip::flat::ApplyToDialog(GetSafeHwnd());

    CString s;
    s.LoadString(IDS_DIALOG_CONFLICT_TITLE);  SetWindowText(s);
    s.LoadString(IDS_LABEL_CONFLICT);         SetDlgItemText(IDC_LABEL_CONFLICT_HEADING, s);
    s.LoadString(IDS_CHECK_REMEMBER);         SetDlgItemText(IDC_CHECK_REMEMBER, s);
    s.LoadString(IDS_BUTTON_OVERWRITE);       SetDlgItemText(IDC_BTN_OVERWRITE, s);
    s.LoadString(IDS_BUTTON_SKIP);            SetDlgItemText(IDC_BTN_SKIP, s);
    s.LoadString(IDS_BUTTON_RENAME);          SetDlgItemText(IDC_BTN_RENAME, s);
    s.LoadString(IDS_BUTTON_CANCEL);          SetDlgItemText(IDCANCEL, s);

    SetDlgItemText(IDC_LABEL_CONFLICT_PATH, dest_path_);
    GetDlgItem(IDC_BTN_SKIP)->SetFocus();  // default = Skip per plan §12
    return FALSE;
}

void CConflictDialog::OnOverwrite() {
    UpdateData(TRUE);
    action_ = openzip::Extractor::ConflictAction::Overwrite;
    EndDialog(IDOK);
}

void CConflictDialog::OnSkip() {
    UpdateData(TRUE);
    action_ = openzip::Extractor::ConflictAction::Skip;
    EndDialog(IDOK);
}

void CConflictDialog::OnRename() {
    UpdateData(TRUE);
    action_ = openzip::Extractor::ConflictAction::Rename;
    EndDialog(IDOK);
}

void CConflictDialog::OnCancel() {
    UpdateData(TRUE);
    action_ = openzip::Extractor::ConflictAction::Cancel;
    EndDialog(IDCANCEL);
}

HBRUSH CConflictDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
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
