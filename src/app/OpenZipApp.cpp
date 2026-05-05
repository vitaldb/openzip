#include "stdafx.h"
#include "OpenZipApp.h"

#include "ArchiveBrowserDialog.h"
#include "CommandLine.h"
#include "CompressDialog.h"
#include "CompressOptionsDialog.h"
#include "DropTargetDialog.h"
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
    text.LoadString(IDS_HELP_TEXT);
    AfxMessageBox(text, MB_OK | MB_ICONINFORMATION);
}

// Forward declaration so the drop-target handler can re-enter the regular
// extract flow with a synthetic command line per dropped file.
void ProcessOne(const std::wstring& raw_cmdline);

// Build a `--extract <path>` command line for a single archive. Used when the
// drop-target dialog hands us paths — re-using ParseCommandLine keeps target
// resolution (Mode::Default → Here for .gz/.xz, Folder otherwise) consistent
// with file-association double-click.
std::wstring BuildExtractCmdline(const std::filesystem::path& archive) {
    std::wstring path = archive.wstring();
    // Quote the path so embedded spaces survive CommandLineToArgvW.
    std::wstring quoted;
    quoted.reserve(path.size() + 2);
    quoted.push_back(L'"');
    for (wchar_t c : path) {
        if (c == L'"') quoted.push_back(L'\\');
        quoted.push_back(c);
    }
    quoted.push_back(L'"');
    // argv[0] is required; ParseCommandLine skips it. Use the real exe path
    // to keep semantics aligned with normal launches.
    wchar_t exe[MAX_PATH] = L"OpenZipApp.exe";
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring out;
    out.reserve(static_cast<size_t>(::wcslen(exe)) + quoted.size() + 4);
    out.push_back(L'"'); out += exe; out.push_back(L'"');
    out.push_back(L' '); out += quoted;
    return out;
}

void ProcessOne(const std::wstring& raw_cmdline) {
    auto cl = openzip::ParseCommandLine(raw_cmdline.c_str());

    if (cl.show_help) {
        ShowHelp();
        return;
    }
    if (cl.show_drop_target) {
        // Idle GUI launch — no archive specified. Show an empty drop window;
        // when the user drops files, re-enter ProcessOne for each so the
        // regular browser/extract flow handles them. The drop loop runs
        // until the user closes the window without dropping (IDCANCEL).
        for (;;) {
            CDropTargetDialog drop;
            INT_PTR rc = drop.DoModal();
            if (rc != IDOK || drop.dropped_paths.empty()) break;
            for (const auto& p : drop.dropped_paths) {
                ProcessOne(BuildExtractCmdline(p));
            }
        }
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
            // Folders keep their full name; only files have an extension to drop.
            for (size_t i = 0; i < cl.compress_items.size(); ++i) {
                const auto& src = cl.compress_items[i];
                std::wstring base = fs::is_directory(src)
                    ? src.filename().wstring()
                    : src.stem().wstring();
                fs::path out = src.parent_path() / (base + L".zip");

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

    // OpenZipApp.exe is the GUI binary — file-association double-click and
    // shell-extension launches land here. The CLI entry point lives in the
    // separate openzip.exe binary (src/cli/), built as a console-subsystem
    // app so it blocks the parent shell properly. No console-attach hacks here.

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
