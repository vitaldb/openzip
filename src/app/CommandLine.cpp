#include "stdafx.h"
#include "CommandLine.h"

#include <Windows.h>
#include <shellapi.h>

namespace fs = std::filesystem;

namespace openzip {

namespace {

enum class Mode { Default, Here, Folder };

// Resolve target dir from explicit --target or zip + mode.
fs::path ResolveTarget(const fs::path& zip, const fs::path& explicit_target, Mode mode) {
    if (!explicit_target.empty()) return fs::absolute(explicit_target);
    fs::path parent = zip.parent_path();
    if (parent.empty()) parent = fs::current_path();
    if (mode == Mode::Here) return parent;
    return parent / zip.stem();  // Folder or Default
}

}  // namespace

CommandLine ParseCommandLine(const wchar_t* cmdline) {
    CommandLine c;
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(cmdline, &argc);
    if (!argv) {
        c.valid = false;
        c.error = L"failed to parse command line";
        return c;
    }

    fs::path explicit_target;
    Mode mode = Mode::Default;

    // argv[0] is the exe path; skip it.
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        auto need_value = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                c.valid = false;
                c.error = std::wstring(name) + L" requires a value";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == L"--extract") {
            const wchar_t* v = need_value(L"--extract");
            if (!v) break;
            c.zip_path = v;
        } else if (a == L"--target") {
            const wchar_t* v = need_value(L"--target");
            if (!v) break;
            explicit_target = v;
        } else if (a == L"--password") {
            const wchar_t* v = need_value(L"--password");
            if (!v) break;
            c.password = v;
        } else if (a == L"--threads") {
            const wchar_t* v = need_value(L"--threads");
            if (!v) break;
            c.threads = ::_wtoi(v);
            if (c.threads < 0) c.threads = 0;
        } else if (a == L"--here") {
            mode = Mode::Here;
        } else if (a == L"--folder") {
            mode = Mode::Folder;
        } else if (a == L"--help" || a == L"-h" || a == L"/?") {
            c.show_help = true;
        } else if (!a.empty() && a.front() == L'-') {
            c.valid = false;
            c.error = L"unknown option: " + a;
            break;
        } else if (c.zip_path.empty()) {
            // Positional argument: the zip file (e.g. file-association double-click).
            c.zip_path = a;
        } else {
            // Extra positional → error (use --target for output dir).
            c.valid = false;
            c.error = L"unexpected argument: " + a;
            break;
        }
    }
    ::LocalFree(argv);

    if (c.show_help || !c.valid) return c;

    if (c.zip_path.empty()) {
        c.valid = false;
        c.error = L"no zip file specified (use --extract <path> or pass a path positionally)";
        return c;
    }

    c.zip_path = fs::absolute(c.zip_path);
    c.target_dir = ResolveTarget(c.zip_path, explicit_target, mode);
    return c;
}

}  // namespace openzip
