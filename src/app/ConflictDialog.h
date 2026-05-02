#pragma once

#include "stdafx.h"
#include "resource.h"
#include "core/extractor.h"

class CConflictDialog : public CDialogEx {
    DECLARE_DYNAMIC(CConflictDialog)
public:
    CConflictDialog(const CString& dest_path, CWnd* parent = nullptr);

    enum { IDD = IDD_CONFLICT };

    openzip::Extractor::ConflictAction action() const { return action_; }
    bool remember() const { return remember_; }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;

    afx_msg void OnOverwrite();
    afx_msg void OnSkip();
    afx_msg void OnRename();
    afx_msg void OnCancel();

    DECLARE_MESSAGE_MAP()

private:
    CString dest_path_;
    openzip::Extractor::ConflictAction action_ = openzip::Extractor::ConflictAction::Cancel;
    BOOL remember_ = FALSE;
};
