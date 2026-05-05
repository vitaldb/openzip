#include "stdafx.h"
#include "DropTargetDialog.h"
#include "flat_button.h"

#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

IMPLEMENT_DYNAMIC(CDropTargetDialog, CDialogEx)

CDropTargetDialog::CDropTargetDialog(CWnd* p) : CDialogEx(IDD, p) {}

BEGIN_MESSAGE_MAP(CDropTargetDialog, CDialogEx)
    ON_WM_CTLCOLOR()
    ON_WM_DROPFILES()
END_MESSAGE_MAP()

BOOL CDropTargetDialog::OnInitDialog() {
    CDialogEx::OnInitDialog();

    if (openzip::dark_theme::IsDarkModeActive()) {
        openzip::dark_theme::EnableForWindow(GetSafeHwnd());
    }
    openzip::flat::ApplyToDialog(GetSafeHwnd());
    openzip::icons::ApplyDialogIcon(GetSafeHwnd(), AfxGetResourceHandle(), IDR_MAINFRAME);

    CString title;
    title.LoadString(IDS_DROP_TARGET_TITLE);
    SetWindowText(title);

    CString hint;
    hint.LoadString(IDS_DROP_TARGET_HINT);
    SetDlgItemText(IDC_DROP_HINT, hint);

    // Belt-and-braces drag-drop registration. The dialog template already
    // declares WS_EX_ACCEPTFILES, but DragAcceptFiles also wires the
    // window into the shell's drop-target list explicitly — some shells
    // (and elevated drag sources) need the runtime call.
    ::DragAcceptFiles(GetSafeHwnd(), TRUE);
    return TRUE;
}

HBRUSH CDropTargetDialog::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor) {
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

void CDropTargetDialog::OnDropFiles(HDROP hDrop) {
    UINT count = ::DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
    dropped_paths.clear();
    dropped_paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        wchar_t buf[MAX_PATH * 2];
        UINT n = ::DragQueryFileW(hDrop, i, buf, static_cast<UINT>(std::size(buf)));
        if (n > 0 && n < std::size(buf)) {
            dropped_paths.emplace_back(buf);
        }
    }
    ::DragFinish(hDrop);

    // Close the drop dialog so the caller can iterate `dropped_paths` and
    // open the regular browser/extract flow per dropped file. Only signal
    // OK if we actually got at least one path — an empty drop (e.g. of a
    // non-file shell object like a printer) leaves the dialog open.
    if (!dropped_paths.empty()) {
        EndDialog(IDOK);
    }
}
