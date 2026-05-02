#include "stdafx.h"
#include "OpenZipApp.h"

#include "ArchiveBrowserDialog.h"
#include "CliRunner.h"
#include "CommandLine.h"
#include "CompressDialog.h"
#include "CompressOptionsDialog.h"
#include "ExtractDialog.h"
#include "SingleInstance.h"
#include "resource.h"

#include <cstdio>
#include <io.h>
#include <fcntl.h>

BEGIN_MESSAGE_MAP(COpenZipApp, CWinApp)
END_MESSAGE_MAP()

COpenZipApp theApp;

namespace {

constexpr DWORD kLeaderGraceMs = 250;

// Did we successfully attach to the parent shell's console at startup?
// When true, --help and parse-error messages go to stdout/stderr instead
// of MessageBox so `openzip --help` from cmd/PowerShell prints inline.
bool g_console_attached = false;

void TryAttachParentConsole() {
    // Step 1: attach to parent console if it has one (cmd, PowerShell, etc).
    bool attached = ::AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;

    // Step 2: bind CRT stdout/stderr to OS standard handles. This is the same
    // technique VitalRecorder uses — works whether AttachConsole succeeded
    // (real console) or our std handles were inherited as pipes/files
    // (e.g. PowerShell `Start-Process -RedirectStandardOutput`). _open_osfhandle
    // adopts the OS handle as a CRT file descriptor, then _dup2 binds it to
    // the well-known stdout/stderr fd that fputs/printf write through.
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = ::GetStdHandle(STD_ERROR_HANDLE);
    if (hOut && hOut != INVALID_HANDLE_VALUE) {
        int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(hOut), 0);
        if (fd >= 0) { ::_dup2(fd, ::_fileno(stdout)); std::setvbuf(stdout, nullptr, _IONBF, 0); }
    }
    if (hErr && hErr != INVALID_HANDLE_VALUE) {
        int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(hErr), 0);
        if (fd >= 0) { ::_dup2(fd, ::_fileno(stderr)); std::setvbuf(stderr, nullptr, _IONBF, 0); }
    }

    // Step 3: declare CLI mode if we actually have somewhere to write.
    DWORD ftOut = hOut ? ::GetFileType(hOut) : FILE_TYPE_UNKNOWN;
    bool stdio_connected = (ftOut == FILE_TYPE_CHAR
                         || ftOut == FILE_TYPE_PIPE
                         || ftOut == FILE_TYPE_DISK);
    if (attached || stdio_connected) {
        g_console_attached = true;
        if (attached) ::SetConsoleOutputCP(CP_UTF8);
    }
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

void ShowHelp() {
    CString text;
    text.LoadString(IDS_HELP_TEXT);
    if (g_console_attached) {
        std::fputs(WideToUtf8(text.GetString()).c_str(), stdout);
        std::fputc('\n', stdout);
    } else {
        AfxMessageBox(text, MB_OK | MB_ICONINFORMATION);
    }
}

void ShowError(const std::wstring& msg) {
    if (g_console_attached) {
        std::fputs("openzip: ", stderr);
        std::fputs(WideToUtf8(msg).c_str(), stderr);
        std::fputc('\n', stderr);
    } else {
        AfxMessageBox(msg.c_str(), MB_OK | MB_ICONERROR);
    }
}

void ProcessOne(const std::wstring& raw_cmdline) {
    auto cl = openzip::ParseCommandLine(raw_cmdline.c_str());

    if (cl.show_help) {
        ShowHelp();
        return;
    }
    if (!cl.valid) {
        ShowError(cl.error);
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

    // If we were spawned from a console (cmd, PowerShell, Windows Terminal),
    // attach to the parent's console so --help and parse errors print inline
    // instead of popping a MessageBox. No-op when launched from Explorer or
    // by the shell extension's CreateProcess (which has no console).
    TryAttachParentConsole();

    // Console-attached → CLI mode: extract / compress / list directly to
    // stdout, no dialogs, no single-instance queue. Each invocation is
    // independent and exits when the work is done.
    //
    // ExitProcess is used (instead of returning FALSE from InitInstance,
    // which would force exit code 0) so the shell sees a meaningful status:
    // 0 = success, 1 = operation failed, 2 = bad command line.
    if (g_console_attached) {
        auto cl = openzip::ParseCommandLine(::GetCommandLineW());
        if (cl.show_help)              { ShowHelp(); ::ExitProcess(0); }
        if (!cl.valid)                 { ShowError(cl.error); ::ExitProcess(2); }
        int rc = openzip::cli::Run(cl);
        ::ExitProcess(static_cast<UINT>(rc));
    }

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
