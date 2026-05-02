#include "stdafx.h"
#include "PasswordDialog.h"

IMPLEMENT_DYNAMIC(CPasswordDialog, CDialogEx)

BEGIN_MESSAGE_MAP(CPasswordDialog, CDialogEx)
END_MESSAGE_MAP()

CPasswordDialog::CPasswordDialog(const CString& archive_name, const CString& entry_name,
                                 bool was_wrong, CWnd* parent)
    : CDialogEx(IDD_PASSWORD, parent),
      archive_name_(archive_name),
      entry_name_(entry_name),
      was_wrong_(was_wrong) {}

void CPasswordDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_EDIT_PASSWORD, password_);
}

BOOL CPasswordDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    CString s;
    s.LoadString(IDS_DIALOG_PASSWORD_TITLE);  SetWindowText(s);
    s.LoadString(IDS_LABEL_PASSWORD_ENTER);   SetDlgItemText(IDC_LABEL_PASSWORD_ENTER, s);
    s.LoadString(IDS_BUTTON_OK);              SetDlgItemText(IDOK, s);
    s.LoadString(IDS_BUTTON_CANCEL);          SetDlgItemText(IDCANCEL, s);

    CString prompt;
    if (entry_name_.IsEmpty()) {
        CString fmt;
        fmt.LoadString(IDS_LABEL_PASSWORD_ENC_FOR);
        prompt.Format(fmt, archive_name_.GetString());
    } else {
        CString fmt;
        fmt.LoadString(IDS_LABEL_PASSWORD_ENC_ENTRY);
        prompt.Format(fmt, entry_name_.GetString(), archive_name_.GetString());
    }
    SetDlgItemText(IDC_LABEL_PASSWORD_PROMPT, prompt);

    if (was_wrong_) {
        CString hint;
        hint.LoadString(IDS_LABEL_PASSWORD_HINT_WRONG);
        SetDlgItemText(IDC_LABEL_PASSWORD_HINT, hint);
    } else {
        SetDlgItemText(IDC_LABEL_PASSWORD_HINT, L"");
    }

    GetDlgItem(IDC_EDIT_PASSWORD)->SetFocus();
    return FALSE;  // we set focus explicitly
}

void CPasswordDialog::OnOK() {
    UpdateData(TRUE);
    if (password_.IsEmpty()) {
        // Treat empty password as cancel — most archives don't have empty passwords.
        EndDialog(IDCANCEL);
        return;
    }
    CDialogEx::OnOK();
}
