#pragma once

#include "stdafx.h"
#include "resource.h"
#include "dark_theme.h"

#include <filesystem>
#include <vector>

// CDropTargetDialog — shown when OpenZipApp.exe is launched with no
// arguments (e.g. via Start menu, taskbar pin, or a bare double-click of
// the EXE itself). Provides a small empty window that accepts drag-drop
// of archive files. Each dropped file is appended to `dropped_paths` and
// the dialog closes; the caller (OpenZipApp::ProcessOne) opens the regular
// archive-browser flow for each path.
class CDropTargetDialog : public CDialogEx {
    DECLARE_DYNAMIC(CDropTargetDialog)
public:
    explicit CDropTargetDialog(CWnd* pParent = nullptr);
    enum { IDD = IDD_DROP_TARGET };

    // Filled in before EndDialog(IDOK). Empty when the user closed the
    // window without dropping anything.
    std::vector<std::filesystem::path> dropped_paths;

protected:
    BOOL OnInitDialog() override;
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
    afx_msg void   OnDropFiles(HDROP hDrop);
    DECLARE_MESSAGE_MAP()
};
