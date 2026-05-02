// ClassicShellExt.cpp — stub (Task 5.1 scaffold; full impl in Task 5.2)
#include <Windows.h>
#include <objbase.h>

extern "C" BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

STDAPI DllGetClassObject(REFCLSID, REFIID, void**) { return CLASS_E_CLASSNOTAVAILABLE; }
STDAPI DllCanUnloadNow() { return S_OK; }
