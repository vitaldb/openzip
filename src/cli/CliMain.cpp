// openzip.exe — Console-subsystem entry point.
// Pure CLI: no MFC, no dialogs, no GUI fallback. Parses the command line
// and hands off to openzip::cli::Run, which prints to stdout/stderr.
//
// The GUI counterpart (OpenZipApp.exe) lives in ../app and is the binary
// the shell extensions and file association invoke. Both binaries share
// CommandLine.{h,cpp} and the CliRunner module, compiled into each
// project independently.

#include "CliRunner.h"
#include "CommandLine.h"

#include <Windows.h>
#include <cstdio>

namespace {

void PrintHelp() {
    static const char* HELP =
        "OpenZip 0.3.0 - usage:\n"
        "\n"
        "  openzip <zip-path>                              list archive contents\n"
        "  openzip --extract <zip> [--target <dir>|--here|--folder]\n"
        "                                                  extract\n"
        "  openzip --compress --output <zip> --item <path>... [options]\n"
        "                                                  create archive\n"
        "  openzip --help                                  this help\n"
        "\n"
        "Extract options:\n"
        "  --target <dir>   explicit output directory\n"
        "  --here           extract into the zip's parent directory\n"
        "  --folder         extract into a subfolder named after the zip\n"
        "  --password <pw>  pre-supply password for encrypted archives\n"
        "  --threads <n>    extraction parallelism (0 = auto)\n"
        "\n"
        "Compress options:\n"
        "  --output <zip>   output zip path (required for bundle mode)\n"
        "  --mode bundle|each|prompt    archive layout (default: bundle)\n"
        "  --level store|fast|normal|max  compression level (default: normal)\n"
        "  --encoding utf8|cp949        filename codepage (default: utf8)\n"
        "  --item <path>    source file or folder (repeatable)\n"
        "\n"
        "Exit codes:  0 success | 1 operation failed | 2 bad command line\n";
    std::fputs(HELP, stdout);
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** /*argv*/) {
    (void)argc;

    // Console codepage to UTF-8 so Korean filenames render correctly.
    ::SetConsoleOutputCP(CP_UTF8);

    auto cl = openzip::ParseCommandLine(::GetCommandLineW());

    if (cl.show_help) {
        PrintHelp();
        return 0;
    }
    if (!cl.valid) {
        // ParseCommandLine error is wide; convert to UTF-8 for stderr.
        int n = ::WideCharToMultiByte(CP_UTF8, 0, cl.error.c_str(), -1,
                                      nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string buf(static_cast<size_t>(n - 1), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, cl.error.c_str(), -1,
                                  buf.data(), n, nullptr, nullptr);
            std::fputs("openzip: ", stderr);
            std::fputs(buf.c_str(), stderr);
            std::fputc('\n', stderr);
        }
        return 2;
    }

    return openzip::cli::Run(cl);
}
