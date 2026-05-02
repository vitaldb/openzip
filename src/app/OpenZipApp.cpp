#include "stdafx.h"
#include "OpenZipApp.h"

#include "ArchiveBrowserDialog.h"
#include "CommandLine.h"
#include "CompressDialog.h"
#include "CompressOptionsDialog.h"
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
    auto cl = openzip::ParseCommandLine(raw_cmdline.c_str());

    if (cl.show_help) {
        ShowHelp();
        return;
    }
    if (!cl.valid) {
        AfxMessageBox(cl.error.c_str(), MB_OK | MB_ICONERROR);
        return;
    }

    if (cl.kind == openzip::JobKind::Extract) {
        // ── Extract job ────────────────────────────────────────────
        if (cl.show_browser) {
            // File-association double-click: show archive contents browser first.
            CArchiveBrowserDialog browser;
            browser.zip_path = cl.zip_path;
            if (browser.DoModal() == IDOK) {
                // The browser already prompted for destination + filter.
                openzip::CommandLine cmd_with_target = cl;
                cmd_with_target.target_dir = browser.chosen_extract_dir;
                CExtractDialog extract(cmd_with_target);
                extract.include_filter = browser.chosen_filter_names;
                extract.DoModal();
            }
            // IDCANCEL: user closed the browser; nothing more to do.
        } else {
            CExtractDialog dlg(cl);
            dlg.DoModal();
        }

    } else {
        // ── Compress job ───────────────────────────────────────────
        namespace fs = std::filesystem;

        if (cl.compress_mode == openzip::CompressMode::Prompt) {
            // Show the options dialog first, then run compress progress dialog.
            CCompressOptionsDialog opt;

            // Seed defaults: name from first item's parent folder or file stem.
            const auto& first = cl.compress_items.front();
            if (fs::is_directory(first)) {
                opt.default_output_name = first.filename().wstring() + L".zip";
            } else {
                opt.default_output_name = first.stem().wstring() + L".zip";
            }
            opt.default_output_dir = first.parent_path().wstring();

            if (opt.DoModal() != IDOK) return;

            CCompressDialog cd;
            cd.sources     = cl.compress_items;
            cd.output_path = opt.chosen_output_path;
            cd.options     = opt.chosen_options;
            cd.DoModal();

        } else if (cl.compress_mode == openzip::CompressMode::Each) {
            // Compress each item separately into a same-name .zip beside it.
            for (size_t i = 0; i < cl.compress_items.size(); ++i) {
                const auto& src = cl.compress_items[i];
                fs::path out = src.parent_path() / (src.stem().wstring() + L".zip");

                CCompressDialog cd;
                cd.sources      = {src};
                cd.output_path  = out;
                cd.batch_index  = static_cast<int>(i);
                cd.batch_total  = static_cast<int>(cl.compress_items.size());
                cd.DoModal();
            }

        } else {
            // Bundle: all items → one zip at compress_output.
            CCompressDialog cd;
            cd.sources     = cl.compress_items;
            cd.output_path = cl.compress_output;
            cd.DoModal();
        }
    }
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
