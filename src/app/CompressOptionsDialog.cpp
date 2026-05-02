#include "stdafx.h"
#include "CompressOptionsDialog.h"

#include <filesystem>
#include <shlobj_core.h>

IMPLEMENT_DYNAMIC(CCompressOptionsDialog, CDialogEx)

CCompressOptionsDialog::CCompressOptionsDialog(CWnd* pParent)
    : CDialogEx(IDD, pParent) {}

BEGIN_MESSAGE_MAP(CCompressOptionsDialog, CDialogEx)
    ON_BN_CLICKED(IDC_COMPRESS_BROWSE,  &CCompressOptionsDialog::OnBrowse)
    ON_BN_CLICKED(IDC_COMPRESS_SHOW_PW, &CCompressOptionsDialog::OnTogglePasswordVisibility)
END_MESSAGE_MAP()

void CCompressOptionsDialog::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_COMPRESS_OUTNAME,       outname_);
    DDX_Text(pDX, IDC_COMPRESS_OUTDIR,        outdir_);
    DDX_Text(pDX, IDC_COMPRESS_PASSWORD,      password_);
    DDX_Text(pDX, IDC_COMPRESS_PASSWORD_CFM,  password_cfm_);
    DDX_Radio(pDX, IDC_COMPRESS_LEVEL_STORE,  level_);
    DDX_Check(pDX, IDC_COMPRESS_SHOW_PW,      show_pw_);
}

BOOL CCompressOptionsDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    // Apply localized caption/labels at runtime (same pattern as ExtractDialog).
    SetWindowText(L"OpenZip — 압축 옵션");  // "OpenZip — 압축 옵션"

    outname_ = default_output_name.c_str();
    outdir_  = default_output_dir.c_str();
    UpdateData(FALSE);
    return TRUE;
}

void CCompressOptionsDialog::OnOK() {
    UpdateData(TRUE);

    if (password_ != password_cfm_) {
        AfxMessageBox(L"비밀번호가 일치하지 않습니다.",
                      MB_ICONWARNING);  // "비밀번호가 일치하지 않습니다."
        return;
    }
    if (outname_.IsEmpty() || outdir_.IsEmpty()) {
        AfxMessageBox(L"파일명과 경로를 모두 입력하세요.",
                      MB_ICONWARNING);  // "파일명과 경로를 모두 입력하세요."
        return;
    }

    namespace fs = std::filesystem;
    chosen_output_path = (fs::path(outdir_.GetString()) / outname_.GetString()).wstring();
    chosen_options.password = password_.GetString();
    switch (level_) {
        case 0:  chosen_options.level = openzip::Compressor::Level::Store;  break;
        case 1:  chosen_options.level = openzip::Compressor::Level::Fast;   break;
        case 2:  chosen_options.level = openzip::Compressor::Level::Normal; break;
        case 3:  chosen_options.level = openzip::Compressor::Level::Max;    break;
        default: chosen_options.level = openzip::Compressor::Level::Normal; break;
    }
    chosen_options.filename_encoding = openzip::Compressor::Encoding::Utf8;

    CDialogEx::OnOK();
}

void CCompressOptionsDialog::OnBrowse() {
    UpdateData(TRUE);
    BROWSEINFOW bi{};
    bi.hwndOwner = GetSafeHwnd();
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpszTitle = L"입력 폴더 선택";  // "출력 폴더 선택"
    LPITEMIDLIST pidl = ::SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH];
    if (::SHGetPathFromIDListW(pidl, path)) {
        outdir_ = path;
        UpdateData(FALSE);
    }
    ::CoTaskMemFree(pidl);
}

void CCompressOptionsDialog::OnTogglePasswordVisibility() {
    UpdateData(TRUE);
    HWND p1 = GetDlgItem(IDC_COMPRESS_PASSWORD)->GetSafeHwnd();
    HWND p2 = GetDlgItem(IDC_COMPRESS_PASSWORD_CFM)->GetSafeHwnd();
    wchar_t bullet = show_pw_ ? L'\0' : L'•';  // U+2022 bullet
    ::SendMessageW(p1, EM_SETPASSWORDCHAR, static_cast<WPARAM>(bullet), 0);
    ::SendMessageW(p2, EM_SETPASSWORDCHAR, static_cast<WPARAM>(bullet), 0);
    ::InvalidateRect(p1, nullptr, TRUE);
    ::InvalidateRect(p2, nullptr, TRUE);
}
