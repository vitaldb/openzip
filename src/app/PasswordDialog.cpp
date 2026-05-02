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

    CString prompt;
    if (entry_name_.IsEmpty()) {
        prompt.Format(L"Archive '%s' is password-protected.", archive_name_.GetString());
    } else {
        prompt.Format(L"'%s' inside '%s' is encrypted.",
                      entry_name_.GetString(), archive_name_.GetString());
    }
    SetDlgItemText(IDC_LABEL_PASSWORD_PROMPT, prompt);

    if (was_wrong_) {
        SetDlgItemText(IDC_LABEL_PASSWORD_HINT, L"Wrong password. Try again or cancel.");
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
