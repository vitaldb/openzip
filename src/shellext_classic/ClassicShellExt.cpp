#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <combaseapi.h>
#include <new>
#include <string>
#include <vector>
#include <filesystem>

#include "resource.h"

namespace fs = std::filesystem;

namespace {

// {A8F3C7E4-1B2D-4F5E-9C8A-3B6D2E5F2000}
constexpr GUID kCLSID_Classic = {
    0xA8F3C7E4, 0x1B2D, 0x4F5E,
    {0x9C, 0x8A, 0x3B, 0x6D, 0x2E, 0x5F, 0x20, 0x00}};

LONG g_dll_ref_count = 0;
HMODULE g_module = nullptr;

bool IsWin11OrLater() {
    using RtlGetVersionFn = LONG (WINAPI*)(OSVERSIONINFOEXW*);
    static auto fn = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!fn) return false;
    OSVERSIONINFOEXW v{}; v.dwOSVersionInfoSize = sizeof(v);
    if (fn(&v) != 0) return false;
    return v.dwBuildNumber >= 22000;
}

bool IsKoreanLocale() {
    LANGID mui = ::GetUserDefaultUILanguage();
    LANGID loc = LANGIDFROMLCID(::GetUserDefaultLCID());
    return PRIMARYLANGID(mui) == LANG_KOREAN || PRIMARYLANGID(loc) == LANG_KOREAN;
}

std::wstring AppExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameW(g_module, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"OpenZipApp.exe";
}

void Log(const wchar_t* msg) {
    wchar_t logdir[MAX_PATH];
    if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, logdir))) return;
    ::wcscat_s(logdir, L"\\OpenZip");
    ::CreateDirectoryW(logdir, nullptr);
    wchar_t path[MAX_PATH]; ::wcscpy_s(path, logdir); ::wcscat_s(path, L"\\shellext.log");
    HANDLE h = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    ::SetFilePointer(h, 0, nullptr, FILE_END);
    SYSTEMTIME st; ::GetLocalTime(&st);
    char line[512];
    int n = ::_snprintf_s(line, sizeof(line), _TRUNCATE,
        "[%02d:%02d:%02d.%03d pid=%lu] [classic] %ls\r\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ::GetCurrentProcessId(), msg);
    if (n > 0) { DWORD w; ::WriteFile(h, line, static_cast<DWORD>(n), &w, nullptr); }
    ::CloseHandle(h);
}

bool LaunchApp(const std::wstring& cmdline) {
    std::wstring c = cmdline;
    wchar_t cwd[MAX_PATH] = L"";
    ::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, cwd);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE,
                               0, nullptr, *cwd ? cwd : nullptr, &si, &pi);
    if (ok) { ::CloseHandle(pi.hThread); ::CloseHandle(pi.hProcess); }
    return ok != FALSE;
}

class ClassicShellExt : public IShellExtInit, public IContextMenu {
public:
    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) *ppv = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu) *ppv = static_cast<IContextMenu*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_); if (r == 0) delete this; return r;
    }

    // IShellExtInit — Explorer hands us the selection via CF_HDROP.
    IFACEMETHODIMP Initialize(LPCITEMIDLIST, IDataObject* dobj, HKEY) override {
        items_.clear();
        if (!dobj) return E_INVALIDARG;
        FORMATETC fmt{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(dobj->GetData(&fmt, &stg))) return E_INVALIDARG;
        HDROP hd = static_cast<HDROP>(::GlobalLock(stg.hGlobal));
        if (hd) {
            UINT n = ::DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                wchar_t buf[MAX_PATH];
                if (::DragQueryFileW(hd, i, buf, MAX_PATH)) items_.emplace_back(buf);
            }
            ::GlobalUnlock(stg.hGlobal);
        }
        ::ReleaseStgMedium(&stg);
        return items_.empty() ? E_INVALIDARG : S_OK;
    }

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT idx, UINT idCmdFirst,
                                    UINT /*idCmdLast*/, UINT flags) override {
        if (IsWin11OrLater() || (flags & CMF_DEFAULTONLY) || items_.empty()) {
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        }

        bool only_zip = (items_.size() == 1) && PathMatchSpecW(items_[0].c_str(), L"*.zip");
        UINT id = idCmdFirst;

        // Insert separator before OpenZip submenu.
        ::InsertMenuW(menu, idx++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

        // Submenu: OpenZip
        HMENU sub = ::CreatePopupMenu();
        UINT sub_idx = 0;
        if (only_zip) {
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                          id_extract_here_ = id++,
                          IsKoreanLocale() ? L"여기에 풀기" : L"Extract Here");
            std::wstring stem = fs::path(items_[0]).stem().wstring();
            std::wstring extract_to = IsKoreanLocale()
                ? L"\"" + stem + L"\\\"에 풀기"
                : L"Extract to \"" + stem + L"\\\"";
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                          id_extract_to_folder_ = id++, extract_to.c_str());
            ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        }

        std::wstring base_name;
        if (items_.size() == 1) base_name = fs::path(items_[0]).stem().wstring();
        else base_name = fs::path(items_[0]).parent_path().filename().wstring();
        if (base_name.empty()) base_name = L"Archive";
        std::wstring t_bundle = IsKoreanLocale()
            ? L"\"" + base_name + L".zip\"으로 압축"
            : L"Compress to \"" + base_name + L".zip\"";

        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_bundle_ = id++, t_bundle.c_str());
        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_each_ = id++,
                      IsKoreanLocale() ? L"각각 압축" : L"Compress each separately");
        ::InsertMenuW(sub, sub_idx++, MF_BYPOSITION,
                      id_compress_prompt_ = id++,
                      IsKoreanLocale() ? L"압축 옵션…" : L"Compress with options…");

        MENUITEMINFOW mii{}; mii.cbSize = sizeof(mii);
        mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        mii.wID = id_parent_ = id++;
        mii.hSubMenu = sub;
        mii.dwTypeData = const_cast<LPWSTR>(L"OpenZip");
        ::InsertMenuItemW(menu, idx++, TRUE, &mii);

        ::InsertMenuW(menu, idx, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);

        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, id - idCmdFirst);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici) return E_INVALIDARG;
        UINT cmd_id;
        if (HIWORD(ici->lpVerb) != 0) return E_NOTIMPL;
        cmd_id = LOWORD(ici->lpVerb);

        std::wstring exe = AppExePath();
        if (exe.empty()) return E_FAIL;
        std::wstring cmdline;

        auto first_parent = [&]() -> std::wstring {
            return items_.empty() ? L"" : fs::path(items_[0]).parent_path().wstring();
        };

        if (cmd_id == id_extract_here_) {
            cmdline = L"\"" + exe + L"\" --extract \"" + items_[0] + L"\" --here";
        } else if (cmd_id == id_extract_to_folder_) {
            cmdline = L"\"" + exe + L"\" --extract \"" + items_[0] + L"\" --folder";
        } else if (cmd_id == id_compress_bundle_) {
            std::wstring parent = first_parent();
            std::wstring base = items_.size() == 1
                ? fs::path(items_[0]).stem().wstring()
                : fs::path(parent).filename().wstring();
            cmdline = L"\"" + exe + L"\" --compress --mode bundle --output \""
                    + parent + L"\\" + base + L".zip\"";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else if (cmd_id == id_compress_each_) {
            cmdline = L"\"" + exe + L"\" --compress --mode each";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else if (cmd_id == id_compress_prompt_) {
            cmdline = L"\"" + exe + L"\" --compress --mode prompt";
            for (auto& p : items_) cmdline += L" --item \"" + p + L"\"";
        } else {
            return E_NOTIMPL;
        }

        Log(cmdline.c_str());
        return LaunchApp(cmdline) ? S_OK : E_FAIL;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR* name, UINT cchMax) override {
        if (name && cchMax) name[0] = 0;
        return E_NOTIMPL;
    }

private:
    LONG ref_ = 1;
    std::vector<std::wstring> items_;
    UINT id_parent_ = 0, id_extract_here_ = 0, id_extract_to_folder_ = 0;
    UINT id_compress_bundle_ = 0, id_compress_each_ = 0, id_compress_prompt_ = 0;
};

class ClassFactory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ::InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        LONG r = ::InterlockedDecrement(&ref_); if (r == 0) delete this; return r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* c = new (std::nothrow) ClassicShellExt();
        if (!c) return E_OUTOFMEMORY;
        HRESULT hr = c->QueryInterface(riid, ppv);
        c->Release();
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

extern "C" BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        ::DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kCLSID_Classic) return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_dll_ref_count == 0 ? S_OK : S_FALSE; }
