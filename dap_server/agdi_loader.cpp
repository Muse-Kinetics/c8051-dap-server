// dap_server/agdi_loader.cpp
//
// Loads SiC8051F.dll from the directory containing this exe and resolves
// all AG_* function pointers.
//
// DLL search strategy: build the path as <exe_dir>\SiC8051F.dll.
// This avoids any dependency on PATH or registry state and ensures we load
// the operational copy that was placed alongside the exe after the build.

#include "agdi_loader.h"

#include <filesystem>
#include <string>

// Global loader instance accessed from all DAP server modules.
AgdiLoader g_agdi;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string GetExeDir()
{
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return ".";
    return std::filesystem::path(path).parent_path().string();
}

static void DebugLog(const std::string& s)
{
    OutputDebugStringA(s.c_str());
    fprintf(stderr, "%s", s.c_str());
}

// ---------------------------------------------------------------------------
// AgdiLoader
// ---------------------------------------------------------------------------

AgdiLoader::~AgdiLoader()
{
    Unload();
}

template<typename T>
bool AgdiLoader::Resolve(T& fn, const char* name, bool required)
{
    fn = reinterpret_cast<T>(GetProcAddress(m_module, name));
    if (!fn) {
        if (required) {
            DebugLog(std::string("agdi_loader: required export not found: ") + name + "\n");
        }
        return !required;  // optional failures are not fatal
    }
    return true;
}

bool AgdiLoader::Load()
{
    if (m_module) return true;  // already loaded

    std::string dllPath = GetExeDir() + "\\SiC8051F.dll";
    DebugLog("agdi_loader: loading " + dllPath + "\n");

    m_module = LoadLibraryA(dllPath.c_str());
    if (!m_module) {
        DebugLog("agdi_loader: LoadLibrary failed, GLE=" +
                 std::to_string(GetLastError()) + "\n");
        return false;
    }

    // Required exports — failure means the DLL is the wrong binary
    bool ok = true;
    ok &= Resolve(AG_Init,      "AG_Init",      true);
    ok &= Resolve(AG_GoStep,    "AG_GoStep",    true);
    ok &= Resolve(AG_BreakFunc, "AG_BreakFunc", true);
    ok &= Resolve(AG_MemAcc,    "AG_MemAcc",    true);
    ok &= Resolve(AG_MemAtt,    "AG_MemAtt",    true);
    ok &= Resolve(AG_BpInfo,    "AG_BpInfo",    true);
    ok &= Resolve(AG_AllReg,    "AG_AllReg",    true);
    ok &= Resolve(AG_RegAcc,    "AG_RegAcc",    true);

    // Optional exports — absent in some DLL versions
    Resolve(AG_Serial, "AG_Serial",   false);
    Resolve(DllUv3Cap, "DllUv3Cap",   false);
    Resolve(EnumUv351, "EnumUv351",   false);

    if (!ok) {
        DebugLog("agdi_loader: one or more required exports missing — unloading\n");
        Unload();
        return false;
    }

    DebugLog("agdi_loader: SiC8051F.dll loaded and all required exports resolved\n");
    return true;
}

void AgdiLoader::Unload()
{
    if (!m_module) return;

    FreeLibrary(m_module);
    m_module     = nullptr;
    AG_Init      = nullptr;
    AG_GoStep    = nullptr;
    AG_BreakFunc = nullptr;
    AG_MemAcc    = nullptr;
    AG_MemAtt    = nullptr;
    AG_BpInfo    = nullptr;
    AG_AllReg    = nullptr;
    AG_RegAcc    = nullptr;
    AG_Serial    = nullptr;
    DllUv3Cap    = nullptr;
    EnumUv351    = nullptr;

    DebugLog("agdi_loader: SiC8051F.dll unloaded\n");
}
