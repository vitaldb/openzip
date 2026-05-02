#pragma once

#include "stdafx.h"
#include "resource.h"
#include "dark_theme.h"

#include <filesystem>

// CArchiveBrowserDialog — shown when the user double-clicks a .zip in Explorer
// (file-association invocation: OpenZipApp.exe <zip>).
//
// Caller sets zip_path before calling DoModal().
// DoModal() returns IDOK  → caller should run CExtractDialog (Extract All).
// DoModal() returns IDCANCEL → user closed the browser; nothing to do.

class CArchiveBrowserDialog : public CDialogEx {
    DECLARE_DYNAMIC(CArchiveBrowserDialog)
public:
    explicit CArchiveBrowserDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_ARCHIVE_BROWSER };

    // Caller fills before DoModal:
    std::filesystem::path zip_path;

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;

    afx_msg void OnExtractAll();
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    DECLARE_MESSAGE_MAP()

private:
    void PopulateList();

    CListCtrl list_;  // wired to IDC_BROWSE_LIST
};
