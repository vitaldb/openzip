#pragma once

#include "stdafx.h"
#include "resource.h"
#include "core/compressor.h"
#include "dark_theme.h"

class CCompressOptionsDialog : public CDialogEx {
    DECLARE_DYNAMIC(CCompressOptionsDialog)
public:
    explicit CCompressOptionsDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_COMPRESS_OPTIONS };

    // Inputs (caller fills before DoModal):
    std::wstring default_output_name;   // e.g. "Documents.zip"
    std::wstring default_output_dir;    // parent dir of selection

    // Outputs (after IDOK):
    std::wstring chosen_output_path;    // joined, absolute
    openzip::Compressor::Options chosen_options;

protected:
    BOOL OnInitDialog() override;
    void DoDataExchange(CDataExchange* pDX) override;
    void OnOK() override;
    afx_msg void OnBrowse();
    afx_msg void OnTogglePasswordVisibility();
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    DECLARE_MESSAGE_MAP()

private:
    CString outname_;
    CString outdir_;
    CString password_;
    CString password_cfm_;
    int level_ = 2;   // 0=Store, 1=Fast, 2=Normal, 3=Max
    BOOL show_pw_ = FALSE;
};
