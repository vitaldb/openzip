#include "CommandLine.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

namespace fs = std::filesystem;

namespace openzip {

namespace {

enum class Mode { Default, Here, Folder };

// Case-insensitive single-stream archive check. Pulled inline rather than
// taking a dependency on the core lib so command-line parsing stays cheap.
bool IsSingleStreamArchive(const fs::path& p) {
    std::wstring fn = p.filename().wstring();
    std::transform(fn.begin(), fn.end(), fn.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    auto ends = [&](const wchar_t* sfx) {
        size_t n = ::wcslen(sfx);
        return fn.size() >= n &&
               std::equal(sfx, sfx + n, fn.end() - n);
    };
    return ends(L".gz") || ends(L".tgz") || ends(L".taz") ||
           ends(L".xz") || ends(L".txz");
}

// Resolve target dir from explicit --target or zip + mode.
//
// For single-stream archives (.gz/.xz/etc.), "Default" semantics flip to
// Here: these formats produce exactly one output file, so creating a
// same-named folder and dropping that one file inside it would just nest
// "foo.txt" inside a "foo.txt/" directory. Users who explicitly pass
// --folder still get folder mode, awkward as it is.
fs::path ResolveTarget(const fs::path& zip, const fs::path& explicit_target, Mode mode) {
    if (!explicit_target.empty()) return fs::absolute(explicit_target);
    fs::path parent = zip.parent_path();
    if (parent.empty()) parent = fs::current_path();
    if (mode == Mode::Here) return parent;
    if (mode == Mode::Default && IsSingleStreamArchive(zip)) return parent;
    return parent / zip.stem();  // Folder, or Default for non-gz
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
    bool any_extract_flag_seen = false;  // true if --extract/--here/--folder/--target was seen

    // argv[0] is the exe path; with nothing after it the user launched the
    // GUI directly (Start menu, taskbar, double-click on the .exe). We show
    // the drag-and-drop target window in that case instead of erroring.
    if (argc <= 1) {
        c.show_drop_target = true;
        ::LocalFree(argv);
        return c;
    }

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

        // ── Shared / top-level flags ───────────────────────────────
        if (a == L"--help" || a == L"-h" || a == L"/?") {
            c.show_help = true;

        // ── Compress flags ─────────────────────────────────────────
        } else if (a == L"--compress") {
            c.kind = JobKind::Compress;
        } else if (a == L"--output") {
            const wchar_t* v = need_value(L"--output"); if (!v) break;
            c.compress_output = v;
        } else if (a == L"--mode") {
            const wchar_t* v = need_value(L"--mode"); if (!v) break;
            std::wstring mv = v;
            if      (mv == L"bundle") c.compress_mode = CompressMode::Bundle;
            else if (mv == L"each")   c.compress_mode = CompressMode::Each;
            else if (mv == L"prompt") c.compress_mode = CompressMode::Prompt;
            else { c.valid = false; c.error = L"unknown --mode value: " + mv; break; }
        } else if (a == L"--level") {
            const wchar_t* v = need_value(L"--level"); if (!v) break;
            std::wstring lv = v;
            if      (lv == L"store")  c.compress_level = 0;
            else if (lv == L"fast")   c.compress_level = 1;
            else if (lv == L"normal") c.compress_level = 6;
            else if (lv == L"max")    c.compress_level = 9;
            else { c.valid = false; c.error = L"unknown --level value: " + lv; break; }
        } else if (a == L"--encoding") {
            const wchar_t* v = need_value(L"--encoding"); if (!v) break;
            c.compress_encoding = v;
        } else if (a == L"--item") {
            const wchar_t* v = need_value(L"--item"); if (!v) break;
            c.compress_items.emplace_back(v);

        // ── Extract flags ──────────────────────────────────────────
        } else if (a == L"--extract") {
            any_extract_flag_seen = true;
            const wchar_t* v = need_value(L"--extract");
            if (!v) break;
            c.zip_path = v;
        } else if (a == L"--target") {
            any_extract_flag_seen = true;
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
            any_extract_flag_seen = true;
            mode = Mode::Here;
        } else if (a == L"--folder") {
            any_extract_flag_seen = true;
            mode = Mode::Folder;

        // ── Unknown flag or positional arg ─────────────────────────
        } else if (!a.empty() && a.front() == L'-') {
            c.valid = false;
            c.error = L"unknown option: " + a;
            break;
        } else if (c.kind == JobKind::Extract && c.zip_path.empty()) {
            // Positional argument: the zip file (e.g. file-association double-click).
            c.zip_path = a;
        } else {
            // Extra positional → error.
            c.valid = false;
            c.error = L"unexpected argument: " + a;
            break;
        }
    }
    ::LocalFree(argv);

    if (c.show_help || !c.valid) return c;

    // ── Compress post-parse validation ─────────────────────────────
    if (c.kind == JobKind::Compress) {
        if (c.compress_items.empty()) {
            c.valid = false;
            c.error = L"--compress requires at least one --item";
            return c;
        }
        // Bundle is the only mode that requires a pre-resolved --output:
        //   each   → derives <stem>.zip per item
        //   prompt → user picks the path in the options dialog
        if (c.compress_output.empty() && c.compress_mode == CompressMode::Bundle) {
            c.valid = false;
            c.error = L"--compress --mode bundle requires --output";
            return c;
        }
        if (!c.compress_output.empty())
            c.compress_output = fs::absolute(c.compress_output);
        return c;
    }

    // ── Extract post-parse validation ──────────────────────────────
    if (c.zip_path.empty()) {
        c.valid = false;
        c.error = L"no zip file specified (use --extract <path> or pass a path positionally)";
        return c;
    }

    c.zip_path   = fs::absolute(c.zip_path);
    c.target_dir = ResolveTarget(c.zip_path, explicit_target, mode);

    // Show the archive browser when the user double-clicked the zip (positional-only),
    // i.e. no explicit extract/placement flags were supplied.
    if (!any_extract_flag_seen) {
        c.show_browser = true;
    }

    return c;
}

}  // namespace openzip
