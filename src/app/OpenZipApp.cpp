#include "stdafx.h"
#include "OpenZipApp.h"

#include "CommandLine.h"
#include "ExtractDialog.h"

BEGIN_MESSAGE_MAP(COpenZipApp, CWinApp)
END_MESSAGE_MAP()

COpenZipApp theApp;

namespace {

void ShowHelp() {
    AfxMessageBox(
        L"OpenZip 0.1.0 — usage:\n\n"
        L"  OpenZipApp.exe [--extract] <zip-path> [options]\n\n"
        L"Options:\n"
        L"  --target <dir>   explicit output directory\n"
        L"  --here           extract into the zip's parent directory\n"
        L"  --folder         extract into a new subfolder named after the zip (default)\n"
        L"  --password <pw>  pre-supply the password for encrypted archives",
        MB_OK | MB_ICONINFORMATION);
}

}  // namespace

BOOL COpenZipApp::InitInstance() {
    CWinApp::InitInstance();
    AfxEnableControlContainer();

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    ::InitCommonControlsEx(&icc);

    auto cmd = openzip::ParseCommandLine(::GetCommandLineW());
    if (cmd.show_help) {
        ShowHelp();
        return FALSE;
    }
    if (!cmd.valid) {
        AfxMessageBox(cmd.error.c_str(), MB_OK | MB_ICONERROR);
        return FALSE;
    }

    CExtractDialog dlg(cmd);
    dlg.DoModal();

    return FALSE;
}
