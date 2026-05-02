#pragma once

#include "stdafx.h"
#include "resource.h"
#include "dark_theme.h"

class CPasswordDialog : public CDialogEx {
    DECLARE_DYNAMIC(CPasswordDialog)
public:
    CPasswordDialog(const CString& archive_name, const CString& entry_name,
                    bool was_wrong, CWnd* parent = nullptr);

    enum { IDD = IDD_PASSWORD };

    CString password() const { return password_; }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);

    DECLARE_MESSAGE_MAP()

private:
    CString archive_name_;
    CString entry_name_;
    bool was_wrong_;
    CString password_;
};
