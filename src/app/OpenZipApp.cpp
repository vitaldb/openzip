#include "stdafx.h"
#include "OpenZipApp.h"

#include "CommandLine.h"
#include "ExtractDialog.h"
#include "SingleInstance.h"
#include "resource.h"

BEGIN_MESSAGE_MAP(COpenZipApp, CWinApp)
END_MESSAGE_MAP()

COpenZipApp theApp;

namespace {

constexpr DWORD kLeaderGraceMs = 250;

void ShowHelp() {
    CString text;
    CString title;
    text.LoadString(IDS_HELP_TEXT);
    title.LoadString(IDS_HELP_TITLE);
    AfxMessageBox(text, MB_OK | MB_ICONINFORMATION);
}

void ProcessOne(const std::wstring& raw_cmdline) {
    auto cmd = openzip::ParseCommandLine(raw_cmdline.c_str());
    if (cmd.show_help) {
        ShowHelp();
        return;
    }
    if (!cmd.valid) {
        AfxMessageBox(cmd.error.c_str(), MB_OK | MB_ICONERROR);
        return;
    }
    CExtractDialog dlg(cmd);
    dlg.DoModal();
}

}  // namespace

BOOL COpenZipApp::InitInstance() {
    CWinApp::InitInstance();
    AfxEnableControlContainer();

    // Honour the user's preferred UI language. STRINGTABLEs in OpenZipApp.rc
    // are split between LANG_ENGLISH and LANG_KOREAN; LoadString picks
    // whichever matches the thread's UI language at runtime.
    //
    // Prefer Korean if EITHER the MUI language OR the regional locale is
    // Korean — many users run an English MUI Windows with ko-KR regional
    // settings and still expect Korean app labels.
    LANGID mui = ::GetUserDefaultUILanguage();
    LANGID loc = LANGIDFROMLCID(::GetUserDefaultLCID());
    if (PRIMARYLANGID(mui) == LANG_KOREAN || PRIMARYLANGID(loc) == LANG_KOREAN) {
        ::SetThreadUILanguage(MAKELANGID(LANG_KOREAN, SUBLANG_KOREAN));
    } else {
        ::SetThreadUILanguage(mui);
    }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);

    openzip::SingleInstance si;
    if (!si.TryAcquireOrSend(::GetCommandLineW())) {
        return FALSE;  // forwarded to existing leader
    }

    std::wstring next;
    while (si.PopNext(next, kLeaderGraceMs)) {
        ProcessOne(next);
    }

    si.Shutdown();
    return FALSE;
}
