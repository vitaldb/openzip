// OpenZipShellExt — IExplorerCommand implementation for the Win11 modern
// context menu. Registered via AppxManifest (windows.fileExplorerContextMenus +
// windows.comServer); never via regsvr32.
//
// CLSID **must** match Package.appxmanifest:
//     A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <combaseapi.h>
#include <cstdarg>
#include <new>
#include <string>

#include "resource.h"

#pragma comment(lib, "shlwapi.lib")

// ---------------------------------------------------------------------------
// Diagnostic logging — writes to %LOCALAPPDATA%\OpenZip\shellext.log

static void Log(const wchar_t* fmt, ...) {
    wchar_t logdir[MAX_PATH];
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, logdir))) return;
    ::wcscat_s(logdir, L"\\OpenZip");
    ::CreateDirectoryW(logdir, nullptr);

    wchar_t logpath[MAX_PATH];
    ::wcscpy_s(logpath, logdir);
    ::wcscat_s(logpath, L"\\shellext.log");

    HANDLE h = ::CreateFileW(logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    ::SetFilePointer(h, 0, nullptr, FILE_END);

    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t prefix[64];
    ::swprintf_s(prefix, L"[%02d:%02d:%02d.%03d pid=%lu tid=%lu] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 ::GetCurrentProcessId(), ::GetCurrentThreadId());

    wchar_t body[1024];
    va_list args;
    va_start(args, fmt);
    int n = ::vswprintf_s(body, fmt, args);
    va_end(args);
    if (n < 0) n = 0;

    wchar_t line[2048];
    ::wcscpy_s(line, prefix);
    ::wcscat_s(line, body);
    ::wcscat_s(line, L"\r\n");

    char utf8[4096];
    int alen = ::WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    if (alen > 1) {
        DWORD written;
        ::WriteFile(h, utf8, alen - 1, &written, nullptr);  // skip terminator
    }
    ::CloseHandle(h);
}

namespace {

// {A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F1A0C} — parent verb. Mirrored in AppxManifest.
constexpr GUID kCLSID_OpenZipCommand = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1A, 0x0C}};

constexpr GUID kCLSID_ExtractHere = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1A, 0x0D}};

constexpr GUID kCLSID_ExtractToFolder = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x1A, 0x0E}};

LONG g_dll_ref_count = 0;
HMODULE g_module = nullptr;

// ---------------------------------------------------------------------------

enum class CmdKind { Parent, Here, Folder };

// Get the full path of the first selected item, or empty on failure.
std::wstring FirstSelectedPath(IShellItemArray* items) {
    std::wstring out;
    if (!items) return out;

    DWORD count = 0;
    if (FAILED(items->GetCount(&count)) || count == 0) return out;

    IShellItem* item = nullptr;
    if (FAILED(items->GetItemAt(0, &item)) || !item) return out;

    LPWSTR display = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &display)) && display) {
        out = display;
        ::CoTaskMemFree(display);
    }
    item->Release();
    return out;
}

bool EndsWithIgnoreCase(const std::wstring& s, const wchar_t* suffix) {
    size_t n = std::char_traits<wchar_t>::length(suffix);
    if (s.size() < n) return false;
    return ::CompareStringOrdinal(s.c_str() + s.size() - n, static_cast<int>(n),
                                  suffix, static_cast<int>(n), TRUE) == CSTR_EQUAL;
}

// Strip directory and extension from a zip path → "foo".
std::wstring StemOf(const std::wstring& zip_path) {
    size_t slash = zip_path.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? zip_path : zip_path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

bool IsKoreanLocale() {
    // Prefer Korean labels if either MUI or regional locale is Korean
    // (common case: English MUI Windows + ko-KR regional settings).
    LANGID mui = ::GetUserDefaultUILanguage();
    LANGID loc = LANGIDFROMLCID(::GetUserDefaultLCID());
    return PRIMARYLANGID(mui) == LANG_KOREAN || PRIMARYLANGID(loc) == LANG_KOREAN;
}

// Localized menu title for the "Extract to <name>\" verb.
std::wstring TitleExtractTo(const std::wstring& zip_path) {
    std::wstring name = StemOf(zip_path);
    if (IsKoreanLocale()) {
        return name.empty() ? std::wstring(L"폴더에 풀기")
                            : L"\"" + name + L"\\\"에 풀기";
    }
    return name.empty() ? std::wstring(L"Extract to folder\\")
                        : L"Extract to \"" + name + L"\\\"";
}

const wchar_t* TitleExtractHere() {
    return IsKoreanLocale() ? L"여기에 풀기" : L"Extract Here";
}

HRESULT CopyToTaskMem(const wchar_t* src, LPWSTR* out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!src) return S_OK;
    size_t bytes = (std::char_traits<wchar_t>::length(src) + 1) * sizeof(wchar_t);
    *out = static_cast<LPWSTR>(::CoTaskMemAlloc(bytes));
    if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, src, bytes);
    return S_OK;
}

// Resolve the absolute path of OpenZipApp.exe (assumed to live next to this DLL).
std::wstring AppExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(g_module, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash + 1);
    path += L"OpenZipApp.exe";
    return path;
}

void LaunchApp(const std::wstring& zip_path, CmdKind kind) {
    std::wstring exe = AppExePath();
    Log(L"LaunchApp exe=%s zip=%s kind=%d", exe.c_str(), zip_path.c_str(), static_cast<int>(kind));
    if (exe.empty() || zip_path.empty()) return;

    std::wstring cmdline;
    cmdline.reserve(exe.size() + zip_path.size() + 64);
    cmdline += L"\"" + exe + L"\"";
    cmdline += L" --extract \"" + zip_path + L"\"";
    if (kind == CmdKind::Here) cmdline += L" --here";
    else if (kind == CmdKind::Folder) cmdline += L" --folder";

    // Set the working directory to the user's profile so the child's CWD is sensible
    // (the surrogate's CWD may be system32, which is read-only for non-admins).
    wchar_t cwd[MAX_PATH] = L"";
    ::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, cwd);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                               0, nullptr, *cwd ? cwd : nullptr, &si, &pi);
    if (ok) {
        Log(L"CreateProcessW ok pid=%lu", pi.dwProcessId);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    } else {
        Log(L"CreateProcessW failed err=%lu", ::GetLastError());
    }
}

// ---------------------------------------------------------------------------
// IExplorerCommand implementation. One class, parameterized by CmdKind.

class OpenZipCommand;
class SubCommandEnum;

class OpenZipCommand : public IExplorerCommand, public IObjectWithSite {
public:
    explicit OpenZipCommand(CmdKind kind) : kind_(kind) {}

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IExplorerCommand) {
            *ppv = static_cast<IExplorerCommand*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    // IExplorerCommand
    IFACEMETHODIMP GetTitle(IShellItemArray* items, LPWSTR* name) override;
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override {
        if (!icon) return E_POINTER;
        *icon = nullptr;
        wchar_t module[MAX_PATH];
        DWORD n = ::GetModuleFileNameW(g_module, module, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return E_NOTIMPL;
        wchar_t buf[MAX_PATH + 16];
        ::swprintf_s(buf, L"%s,-%d", module, IDI_OPENZIP);
        return CopyToTaskMem(buf, icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        if (tip) *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override {
        if (!guid) return E_POINTER;
        switch (kind_) {
            case CmdKind::Parent: *guid = kCLSID_OpenZipCommand; break;
            case CmdKind::Here:   *guid = kCLSID_ExtractHere;    break;
            case CmdKind::Folder: *guid = kCLSID_ExtractToFolder; break;
        }
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL okToBeSlow, EXPCMDSTATE* state) override {
        Log(L"GetState kind=%d items=%p okSlow=%d", static_cast<int>(kind_), items, okToBeSlow);
        if (!state) return E_POINTER;
        *state = ECS_ENABLED;
        if (!items) { *state = ECS_HIDDEN; return S_OK; }

        DWORD count = 0;
        if (FAILED(items->GetCount(&count)) || count != 1) {
            *state = ECS_HIDDEN;
            return S_OK;
        }
        std::wstring path = FirstSelectedPath(items);
        Log(L"GetState path=%s", path.c_str());
        if (!EndsWithIgnoreCase(path, L".zip")) {
            *state = ECS_HIDDEN;
        }
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override;
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        if (!flags) return E_POINTER;
        *flags = (kind_ == CmdKind::Parent) ? ECF_HASSUBCOMMANDS : ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** out) override;

    // IObjectWithSite — Explorer hands us a site; we don't need it but must accept it.
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site_) { site_->Release(); site_ = nullptr; }
        site_ = site;
        if (site_) site_->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!site_) { *ppv = nullptr; return E_FAIL; }
        return site_->QueryInterface(riid, ppv);
    }

private:
    LONG ref_ = 1;
    CmdKind kind_;
    IUnknown* site_ = nullptr;
};

class SubCommandEnum : public IEnumExplorerCommand {
public:
    SubCommandEnum() = default;

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumExplorerCommand) {
            *ppv = static_cast<IEnumExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** rgelt, ULONG* pceltFetched) override {
        ULONG fetched = 0;
        for (ULONG i = 0; i < celt && cursor_ < kCount; ++i) {
            CmdKind kind = (cursor_ == 0) ? CmdKind::Here : CmdKind::Folder;
            auto* cmd = new (std::nothrow) OpenZipCommand(kind);
            if (!cmd) return E_OUTOFMEMORY;
            rgelt[i] = cmd;
            ++cursor_;
            ++fetched;
        }
        if (pceltFetched) *pceltFetched = fetched;
        return (fetched < celt) ? S_FALSE : S_OK;
    }
    IFACEMETHODIMP Skip(ULONG celt) override {
        cursor_ = (cursor_ + celt > kCount) ? kCount : cursor_ + celt;
        return S_OK;
    }
    IFACEMETHODIMP Reset() override { cursor_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** out) override {
        if (!out) return E_POINTER;
        auto* c = new (std::nothrow) SubCommandEnum();
        if (!c) return E_OUTOFMEMORY;
        c->cursor_ = cursor_;
        *out = c;
        return S_OK;
    }

private:
    static constexpr ULONG kCount = 2;
    LONG ref_ = 1;
    ULONG cursor_ = 0;
};

IFACEMETHODIMP OpenZipCommand::GetTitle(IShellItemArray* items, LPWSTR* name) {
    Log(L"GetTitle kind=%d", static_cast<int>(kind_));
    if (!name) return E_POINTER;
    switch (kind_) {
        case CmdKind::Parent: return CopyToTaskMem(L"OpenZip", name);  // brand, untranslated
        case CmdKind::Here:   return CopyToTaskMem(TitleExtractHere(), name);
        case CmdKind::Folder: {
            std::wstring path = FirstSelectedPath(items);
            std::wstring t = TitleExtractTo(path);
            return CopyToTaskMem(t.c_str(), name);
        }
    }
    return E_FAIL;
}

IFACEMETHODIMP OpenZipCommand::Invoke(IShellItemArray* items, IBindCtx*) {
    Log(L"Invoke kind=%d", static_cast<int>(kind_));
    if (kind_ == CmdKind::Parent) return S_OK;  // parent doesn't act
    std::wstring path = FirstSelectedPath(items);
    Log(L"Invoke path=%s", path.c_str());
    if (path.empty()) return E_INVALIDARG;
    LaunchApp(path, kind_);
    return S_OK;
}

IFACEMETHODIMP OpenZipCommand::EnumSubCommands(IEnumExplorerCommand** out) {
    Log(L"EnumSubCommands kind=%d", static_cast<int>(kind_));
    if (!out) return E_POINTER;
    *out = nullptr;
    if (kind_ != CmdKind::Parent) return E_NOTIMPL;
    auto* e = new (std::nothrow) SubCommandEnum();
    if (!e) return E_OUTOFMEMORY;
    *out = e;
    return S_OK;
}

// ---------------------------------------------------------------------------
// Class factory

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* cmd = new (std::nothrow) OpenZipCommand(CmdKind::Parent);
        if (!cmd) return E_OUTOFMEMORY;
        HRESULT hr = cmd->QueryInterface(riid, ppv);
        cmd->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ::InterlockedIncrement(&g_dll_ref_count);
        else      ::InterlockedDecrement(&g_dll_ref_count);
        return S_OK;
    }

private:
    LONG ref_ = 1;
};

}  // namespace

// ---------------------------------------------------------------------------
// DLL exports

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        ::DisableThreadLibraryCalls(hModule);
        Log(L"DLL_PROCESS_ATTACH");
    } else if (reason == DLL_PROCESS_DETACH) {
        Log(L"DLL_PROCESS_DETACH");
    }
    return TRUE;
}

// Export linkage handled by OpenZipShellExt.def — no __declspec(dllexport) here,
// otherwise we conflict with the declarations in combaseapi.h.
STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    Log(L"DllGetClassObject");
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kCLSID_OpenZipCommand) {
        Log(L"  unknown CLSID — returning CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return (g_dll_ref_count == 0) ? S_OK : S_FALSE;
}
