// ForceCloseOreUI.cpp — Windows DLL mod for Minecraft Bedrock
// Hooks OreUI initialization via Microsoft Detours (Dobby is Android/ARM only).
// Single-file build; no extra headers.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <detours.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using Json   = nlohmann::ordered_json;

// ============================================================
// Global state
// ============================================================
static std::string   g_configDir;
static std::string   g_logFilePath;
static volatile bool g_hookValid = false;

// 扫描总预算（毫秒，相对 GetTickCount64）。0 表示不限制。
static uint64_t g_scanDeadline = 0;

static uint64_t nowMs() { return GetTickCount64(); }

static bool outOfTime() {
    return g_scanDeadline != 0 && nowMs() >= g_scanDeadline;
}

// ============================================================
// Logging (console + file, same dual-channel idea as Android)
// ============================================================
static void WriteLog(const char* level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[ForceCloseOreUI][%s] %s\n", level, buf);
    fflush(stdout);

    if (!g_logFilePath.empty()) {
        if (FILE* f = fopen(g_logFilePath.c_str(), "a")) {
            fprintf(f, "[%s] %s\n", level, buf);
            fclose(f);
        }
    }
}

#define LOGI(...) WriteLog("INFO",  __VA_ARGS__)
#define LOGE(...) WriteLog("ERROR", __VA_ARGS__)

// ============================================================
// Session header written to debug.log on startup
// ============================================================
static void writeLogHeader() {
    if (g_logFilePath.empty()) return;

    char timebuf[32]{};
    time_t now = time(nullptr);
    if (struct tm* t = localtime(&now))
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    // Read Windows build number from PE of ntdll (reliable on Win10+)
    DWORD build = 0;
    {
        wchar_t ntdllPath[MAX_PATH]{};
        GetSystemDirectoryW(ntdllPath, MAX_PATH);
        wcscat_s(ntdllPath, L"\\ntdll.dll");
        DWORD dummy = 0;
        DWORD sz = GetFileVersionInfoSizeW(ntdllPath, &dummy);
        if (sz) {
            std::vector<BYTE> vbuf(sz);
            if (GetFileVersionInfoW(ntdllPath, 0, sz, vbuf.data())) {
                VS_FIXEDFILEINFO* info = nullptr;
                UINT infoLen = 0;
                if (VerQueryValueW(vbuf.data(), L"\\", (LPVOID*)&info, &infoLen) && info)
                    build = LOWORD(info->dwFileVersionLS);
            }
        }
    }

    FILE* f = fopen(g_logFilePath.c_str(), "a");
    if (!f) return;
    fprintf(f,
        "\n====================================================\n"
        "[INFO] === SESSION START: %s ===\n"
        "[INFO] Windows Build : %lu\n"
        "[INFO] PID           : %lu\n"
        "====================================================\n",
        timebuf[0] ? timebuf : "unknown",
        build,
        GetCurrentProcessId());
    fclose(f);
}

// ============================================================
// Game classes
// ============================================================
class OreUIConfig {
public:
    void* mUnknown1;
    void* mUnknown2;
    std::function<bool()> mUnknown3;
    std::function<bool()> mUnknown4;
};

class OreUi {
public:
    std::unordered_map<std::string, OreUIConfig> mConfigs;
};

// ============================================================
// Signatures — x64 Windows hardcoded fallback
// ============================================================
static const std::vector<const char*> SIG_FALLBACK = {
    // 按版本从新到旧排列，第一条命中即用。
    // 最前面这条是放宽版（栈偏移全部通配），能同时覆盖多个版本；
    // 后面是各版本实测过的精确版，放宽版失手时兜底。

    // 放宽版：覆盖 1.26.40 ~ 1.26.50 这一代
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 44 88 8D ? ? ? ? 44 88 85 ? ? ? ? 48 89 D7",

    // 1.26.60（上游 1fd8606 新增，与上面那条只差结尾 4 字节）
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC ? ? ? ? 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 44 88 8D ? ? ? ? 45 89 C6",

    // 1.26.50
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC E8 01 00 00 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 44 88 8D 5E",

    // 1.26.20 / 1.26.30
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC D8 01 00 00 48 8D AC 24 ? ? ? ? 48 C7 85 ? ? ? ? ? ? ? ? 45 89 CE",

    // 早期版本（2025-10 ~ 2026-03）
    "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 68 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 49 8B E9 4C 89 44 24 ? 4C 8B EA 48 8B F9 48 89 4C 24",
    "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 78 48 8B 05 ? ? ? ? 48 33 C4 48 89 44 24 ? 49 8B E9 4C 89 44 24",
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 18 02 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24",
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC B8 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 49 8B F1 4C 89 44 24",
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC 98 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 4D 8B F1 4C 89 44 24",
};

static std::string              g_loadedDetourType;
static std::vector<std::string> g_loadedSignatures;

// ============================================================
// Config directory — %APPDATA%\Minecraft Bedrock\mods\ForceCloseOreUI\
//                   or mods\ForceCloseOreUI\ (relative, for custom launchers)
// ============================================================
static std::string getConfigDir() {
    if (!g_configDir.empty()) return g_configDir;

    std::vector<std::string> candidates;
    if (const char* appData = std::getenv("APPDATA"))
        if (*appData)
            candidates.push_back(
                (fs::path(appData) / "Minecraft Bedrock" / "mods" / "ForceCloseOreUI").string() + "\\");
    candidates.push_back((fs::path("mods") / "ForceCloseOreUI").string() + "\\");

    for (auto& dir : candidates) {
        if (!CreateDirectoryA(dir.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            continue;

        // Test writability
        std::string test = dir + ".test";
        HANDLE h = CreateFileA(test.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            LOGI("Path not writable: %s (err=%lu)", dir.c_str(), GetLastError());
            continue;
        }
        CloseHandle(h);

        g_configDir   = dir;
        g_logFilePath = dir + "debug.log";
        writeLogHeader();
        LOGI("Using config dir: %s", dir.c_str());
        return g_configDir;
    }

    // Last-resort fallback
    g_configDir   = "mods\\ForceCloseOreUI\\";
    g_logFilePath = g_configDir + "debug.log";
    LOGE("All preferred paths failed; using relative path.");
    return g_configDir;
}

// ============================================================
// MC version fingerprint — PE timestamp of the main exe
// (changes on every new MC release, no file-read permissions needed)
// ============================================================
static std::string getMcVersionStr() {
    HMODULE hMain = GetModuleHandleW(nullptr);
    if (!hMain) return "";
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hMain);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "";
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(hMain) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%08X", nt->FileHeader.TimeDateStamp);
    return buf;
}

// ============================================================
// Module info — reads PE header in-memory, no psapi needed
// ============================================================
struct ModuleInfo {
    uintptr_t base = 0;
    size_t    size = 0;
    std::vector<std::pair<uintptr_t, size_t>> segments; // r-x regions
};

static bool findMinecraftModule(ModuleInfo& out) {
    HMODULE hMain = GetModuleHandleW(nullptr);
    if (!hMain) return false;

    auto* base = reinterpret_cast<const BYTE*>(hMain);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    out.base = reinterpret_cast<uintptr_t>(hMain);
    out.size = nt->OptionalHeader.SizeOfImage;

    // Enumerate executable pages within the image
    MEMORY_BASIC_INFORMATION mbi{};
    uintptr_t addr = out.base;
    while (addr < out.base + out.size &&
           VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            out.segments.emplace_back(
                reinterpret_cast<uintptr_t>(mbi.BaseAddress), mbi.RegionSize);
        addr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    if (out.segments.empty()) return false;
    LOGI("Module at 0x%llx (%zu bytes, %zu exec segment(s))",
         static_cast<unsigned long long>(out.base),
         out.size, out.segments.size());
    for (auto& [s, sz] : out.segments)
        LOGI("  r-x: 0x%llx - 0x%llx (%zu bytes)",
             static_cast<unsigned long long>(s),
             static_cast<unsigned long long>(s + sz), sz);
    return true;
}

// ============================================================
// Signature pattern scan — segment-exact, same logic as Android
// ============================================================
// 找出 pattern 里最长的一段连续固定字节，作为扫描锚点。
// 锚点让绝大多数位置在 1 次比较后就被排除，而不是每个位置都走完整条模式。
static void findAnchor(const std::vector<int>& pattern, size_t& offset,
                       size_t& len) {
    size_t bestLen = 0, bestOff = 0;
    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == -1) { ++i; continue; }
        size_t j = i;
        while (j < pattern.size() && pattern[j] != -1) ++j;
        if (j - i > bestLen) { bestLen = j - i; bestOff = i; }
        i = j;
    }
    offset = bestOff;
    len    = bestLen;
}

static uintptr_t ResolveSignature(const ModuleInfo& mod, const char* sig) {
    std::vector<int> pattern;
    for (const char* p = sig; *p;) {
        if (*p == ' ')  { ++p; continue; }
        if (*p == '?')  { pattern.push_back(-1); ++p; if (*p == '?') ++p; continue; }
        pattern.push_back(static_cast<int>(strtol(p, nullptr, 16)));
        p += 2;
    }
    if (pattern.empty()) return 0;

    size_t anchorOff = 0, anchorLen = 0;
    findAnchor(pattern, anchorOff, anchorLen);
    const int anchorFirst = anchorLen ? pattern[anchorOff] : -1;

    for (auto& [segBase, segSize] : mod.segments) {
        if (segSize < pattern.size()) continue;
        auto* b = reinterpret_cast<const uint8_t*>(segBase);
        for (size_t i = 0; i + pattern.size() <= segSize; ++i) {
            if (anchorLen && b[i + anchorOff] != static_cast<uint8_t>(anchorFirst))
                continue;
            bool ok = true;
            for (size_t j = 0; j < pattern.size(); ++j)
                if (pattern[j] != -1 && b[i + j] != static_cast<uint8_t>(pattern[j]))
                    { ok = false; break; }
            if (ok) return segBase + i;
        }
    }
    return 0;
}

// ============================================================
// Config helpers (shared with Android version verbatim)
// ============================================================
static std::string trimAscii(std::string v) {
    auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    v.erase(v.begin(), std::find_if_not(v.begin(), v.end(), sp));
    v.erase(std::find_if_not(v.rbegin(), v.rend(), sp).base(), v.end());
    return v;
}

static std::optional<bool> parseBoolString(std::string v) {
    v = trimAscii(std::move(v));
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (v == "true"  || v == "1" || v == "yes" || v == "on"  || v == "enabled")  return true;
    if (v == "false" || v == "0" || v == "no"  || v == "off" || v == "disabled") return false;
    return std::nullopt;
}

static std::optional<bool> readBoolLike(const Json& val) {
    try {
        if (val.is_boolean())         return val.get<bool>();
        if (val.is_number_integer())  return val.get<long long>() != 0;
        if (val.is_number_unsigned()) return val.get<unsigned long long>() != 0;
        if (val.is_number_float())    return val.get<double>() != 0.0;
        if (val.is_string())          return parseBoolString(val.get<std::string>());
        if (val.is_object()) {
            for (std::string_view k : {"value","enabled","enable","state","default"}) {
                auto it = val.find(std::string(k));
                if (it != val.end()) if (auto r = readBoolLike(*it)) return r;
            }
            auto dis = val.find("disabled");
            if (dis != val.end()) if (auto r = readBoolLike(*dis)) return !*r;
        }
    } catch (...) {}
    return std::nullopt;
}

static std::optional<bool> readNamedConfigValue(const Json& root, const std::string& name) {
    if (root.is_object()) {
        auto it = root.find(name);
        if (it != root.end()) if (auto r = readBoolLike(*it)) return r;
        for (std::string_view k : {"configs","settings","toggles","values","oreui","OreUI"}) {
            auto sec = root.find(std::string(k));
            if (sec != root.end() && sec->is_object()) {
                auto n = sec->find(name);
                if (n != sec->end()) if (auto r = readBoolLike(*n)) return r;
            }
        }
    }
    if (root.is_array()) {
        for (auto& item : root) {
            if (!item.is_object()) continue;
            bool matched = false;
            for (std::string_view k : {"name","key","id"}) {
                auto n = item.find(std::string(k));
                if (n != item.end() && n->is_string() &&
                    n->get<std::string>() == name) { matched = true; break; }
            }
            if (matched) if (auto r = readBoolLike(item)) return r;
        }
    }
    return std::nullopt;
}

struct ConfigDocument {
    Json     raw = Json::object(), canonical = Json::object();
    fs::path source, target;
    bool     dirty = false;
};

static ConfigDocument loadConfigDocument() {
    std::string configPath = getConfigDir() + "config.json";
    ConfigDocument doc;
    doc.target = fs::path(configPath);

    if (GetFileAttributesA(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        doc.dirty = true;
        return doc;
    }
    doc.source = doc.target;

    try {
        std::ifstream f(doc.source, std::ios::binary);
        if (f.is_open()) {
            Json parsed = Json::parse(f, nullptr, false, true);
            if (!parsed.is_discarded()) {
                doc.raw = parsed;
                if (parsed.is_object())
                    doc.canonical = std::move(parsed);
                else { doc.dirty = true; doc.canonical = Json::object(); }
            } else { doc.dirty = true; }
        } else { doc.dirty = true; }
    } catch (...) { doc.dirty = true; }
    return doc;
}

// Atomic write via temp file + MoveFileEx (equivalent of rename() on Android)
static bool saveConfigDocument(const fs::path& path, const Json& cfg) {
    if (path.empty()) return false;
    std::string pathStr = path.string();
    std::string tmpStr  = pathStr + ".tmp";

    FILE* fp = fopen(tmpStr.c_str(), "wb");
    if (!fp) { LOGE("fopen tmp failed: %s (err=%lu)", tmpStr.c_str(), GetLastError()); return false; }

    std::string payload = cfg.dump(4);
    size_t written = fwrite(payload.data(), 1, payload.size(), fp);
    fputc('\n', fp);
    fclose(fp);

    if (written != payload.size()) {
        LOGE("Incomplete write: %zu/%zu bytes", written, payload.size());
        DeleteFileA(tmpStr.c_str());
        return false;
    }

    if (!MoveFileExA(tmpStr.c_str(), pathStr.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        LOGE("MoveFileEx failed (err=%lu): %s -> %s",
             GetLastError(), tmpStr.c_str(), pathStr.c_str());
        DeleteFileA(tmpStr.c_str());
        return false;
    }
    LOGI("Config saved: %s (%zu bytes)", pathStr.c_str(), payload.size());
    return true;
}

// ============================================================
// Signature cache — signatures.json
// ============================================================
static std::vector<std::string>& loadSignatures() {
    if (!g_loadedSignatures.empty()) return g_loadedSignatures;

    std::string sigPath = getConfigDir() + "signatures.json";
    if (GetFileAttributesA(sigPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        try {
            std::ifstream f(sigPath);
            Json j = Json::parse(f, nullptr, false, true);
            if (!j.is_discarded() && j.contains("signatures") && j["signatures"].is_array()) {
                for (auto& s : j["signatures"])
                    if (s.is_string()) g_loadedSignatures.push_back(s.get<std::string>());
                if (j.contains("detour") && j["detour"].is_string())
                    g_loadedDetourType = j["detour"].get<std::string>();
                if (!g_loadedSignatures.empty()) {
                    LOGI("Loaded %zu sig(s) from signatures.json (detour=%s)",
                         g_loadedSignatures.size(), g_loadedDetourType.c_str());
                    return g_loadedSignatures;
                }
            }
        } catch (...) {}
    }

    LOGI("Using built-in fallback signatures.");
    for (auto s : SIG_FALLBACK) g_loadedSignatures.push_back(s);
    g_loadedDetourType.clear();
    return g_loadedSignatures;
}

static void writeMatchedSignature(const std::string& sig, const std::string& detourType,
                                   bool forceOverwrite = false) {
    std::string sigPath = getConfigDir() + "signatures.json";
    if (!forceOverwrite &&
        GetFileAttributesA(sigPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    Json j;
    j["signatures"] = Json::array();
    j["signatures"].push_back(sig);
    j["detour"] = detourType;
    j["mc_id"]  = getMcVersionStr();
    saveConfigDocument(fs::path(sigPath), j);
    LOGI("Signature cached (detour=%s, mc_id=%s).",
         detourType.c_str(), getMcVersionStr().c_str());
}

// ============================================================
// Hook function pointers — V1 (OreUi is arg 1) and V10 (arg 10)
// ============================================================
using OreUiInitFuncV1  = void(*)(OreUi&, void*, void*, void*, void*, void*);
using OreUiInitFuncV10 = void(*)(void*, void*, void*, void*, void*,
                                  void*, void*, void*, void*, OreUi&, void*);

static OreUiInitFuncV1  orig_v1  = nullptr;
static OreUiInitFuncV10 orig_v10 = nullptr;

// ============================================================
// applyConfig — identical logic to Android version
// ============================================================
static void applyConfig(OreUi& ore_ui, const char* label) {
    size_t sz = ore_ui.mConfigs.size();
    if (sz == 0) {
        // 函数被调用了，但 map 还是空的：钩子位置是对的，配置会在后续调用时生效。
        // 这里也置 valid —— 否则会被当成"没被调用"，正确钩子被拆掉。
        LOGI("[%s] mConfigs empty (called, nothing to apply yet).", label);
        g_hookValid = true;
        return;
    }
    if (sz > 2000) {
        // 明显畸形：这个地址不是我们要的函数，或者 detour 签名不对。不置 valid。
        LOGE("[%s] Sanity FAILED! mConfigs.size=%zu", label, sz);
        return;
    }

    g_hookValid = true;
    LOGI("[%s] Valid! %zu OreUI config(s).", label, sz);

    ConfigDocument doc = loadConfigDocument();
    if (!doc.canonical.is_object()) doc.canonical = Json::object();

    enum Section { NONE, MODE, SAFE } cur = NONE;
    bool mode_enabled = false;
    std::unordered_map<std::string, bool> mode_entries, safe_entries;

    for (auto& [k, v] : doc.canonical.items()) {
        if (k == "mode")      { if (auto r = readBoolLike(v)) mode_enabled = *r; cur = MODE; continue; }
        if (k == "safe_mode") { cur = SAFE; continue; }
        if (auto r = readBoolLike(v)) {
            if (cur == MODE)       mode_entries[k] = *r;
            else if (cur == SAFE)  safe_entries[k] = *r;
        }
    }
    LOGI("[%s] mode=%s", label, mode_enabled ? "ON" : "OFF");

    bool dirty = false;
    for (auto& [name, cfg] : ore_ui.mConfigs) {
        bool value = false;
        if (mode_enabled && mode_entries.count(name)) value = mode_entries[name];
        else if (safe_entries.count(name))            value = safe_entries[name];

        if (!doc.canonical.contains(name)) { doc.canonical[name] = value; dirty = true; }

        if (!value) {
            cfg.mUnknown3 = [] { return false; };
            cfg.mUnknown4 = [] { return false; };
        }
        LOGI("[%s]   %s -> %s", label, name.c_str(), value ? "kept" : "disabled");
    }

    if (!doc.canonical.contains("mode"))      { doc.canonical["mode"]      = true;  dirty = true; }
    if (!doc.canonical.contains("safe_mode")) { doc.canonical["safe_mode"] = false; dirty = true; }

    if (dirty) {
        // Rebuild sectioned format (same as Android)
        std::string out = "{\n";
        out += "  \"mode\": ";
        out += mode_enabled ? "true" : "false";
        for (auto& [name, _] : ore_ui.mConfigs) {
            if (safe_entries.count(name) && !mode_entries.count(name)) continue;
            bool val = mode_entries.count(name) ? mode_entries.at(name) : false;
            out += ",\n    \"" + name + "\": ";
            out += val ? "true" : "false";
        }
        out += ",\n  \"safe_mode\": false";
        for (auto& [name, _] : ore_ui.mConfigs) {
            if (!safe_entries.count(name)) continue;
            bool val = safe_entries.at(name);
            out += ",\n    \"" + name + "\": ";
            out += val ? "true" : "false";
        }
        out += "\n}\n";

        std::string pathStr = doc.target.string();
        std::string tmpStr  = pathStr + ".tmp";
        if (FILE* fp = fopen(tmpStr.c_str(), "wb")) {
            fwrite(out.data(), 1, out.size(), fp);
            fclose(fp);
            MoveFileExA(tmpStr.c_str(), pathStr.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            LOGI("[%s] Config saved (sectioned, %zu bytes).", label, out.size());
        }
    }
}

// ============================================================
// Default config on first run
// ============================================================
static void ensureConfigExists() {
    std::string configPath = getConfigDir() + "config.json";
    if (GetFileAttributesA(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Json defaults = Json::object();
        defaults["mode"]      = true;
        defaults["safe_mode"] = false;
        saveConfigDocument(fs::path(configPath), defaults);
        LOGI("config.json created (entries filled on first hook).");
    }
}

// ============================================================
// Detours attach / detach helpers
// ============================================================
static bool detoursAttach(void** pOrig, void* pDetour) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(pOrig, pDetour);
    LONG err = DetourTransactionCommit();
    if (err != NO_ERROR) LOGE("DetourTransactionCommit failed: %ld", err);
    return err == NO_ERROR;
}

static void detoursDetach(void** pOrig, void* pDetour) {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(pOrig, pDetour);
    DetourTransactionCommit();
}

// ============================================================
// Wait for the detour to be called.
// 返回 true = 已确认调用（g_hookValid 被 applyConfig 置位）
// 返回 false = 观察期结束仍未被调用，或全局预算耗尽
// ============================================================
static const int kWarmupWaitMs   = 2500;   // 快速判定：多数情况一两秒内就会调用
static const int kExtendedWaitMs = 30000;  // 观察期：等游戏自己进到那个界面

static bool waitForHook(const char* name, uint64_t deadline) {
    const uint64_t t0 = nowMs();

    // 快速窗口：多数情况函数很快就调用了
    while (!g_hookValid && nowMs() - t0 < (uint64_t)kWarmupWaitMs) Sleep(50);
    if (g_hookValid) return true;

    // 观察期：等游戏自己进到调用这个函数的界面。受全局预算约束。
    LOGI("  [%s] not called after %d ms; waiting up to %d ms more...",
         name, kWarmupWaitMs, kExtendedWaitMs);
    const uint64_t observeUntil = t0 + (uint64_t)(kWarmupWaitMs + kExtendedWaitMs);
    while (!g_hookValid) {
        if (outOfTime()) {
            LOGE("  [%s] scan budget exhausted while waiting.", name);
            return false;
        }
        if (nowMs() >= observeUntil) break;
        if (deadline != 0 && nowMs() >= deadline) break;
        Sleep(50);
    }
    return g_hookValid;
}

// ============================================================
// Hook callbacks — applyConfig runs BEFORE orig (same order as Android)
// ============================================================
static void detour_v1(OreUi& a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    try { applyConfig(a1, "V1"); } catch (...) { LOGE("[V1] applyConfig threw"); }
    orig_v1(a1, a2, a3, a4, a5, a6);
}

static void detour_v10(void* a1, void* a2, void* a3, void* a4, void* a5,
                        void* a6, void* a7, void* a8, void* a9,
                        OreUi& a10, void* a11) {
    try { applyConfig(a10, "V10"); } catch (...) { LOGE("[V10] applyConfig threw"); }
    orig_v10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

// ============================================================
// Self-validating hook attempt at one address
// Returns true on success. forceWrite=true for AutoFallback path.
// ============================================================
static bool tryHookAt(uintptr_t addr, const char* sigStr,
                       bool isTrusted, bool forceWrite = false) {
    auto* raw = reinterpret_cast<uint8_t*>(addr);
    LOGI("  Matched 0x%llx — Bytes: %02X %02X %02X %02X %02X %02X %02X %02X "
         "%02X %02X %02X %02X %02X %02X %02X %02X",
         static_cast<unsigned long long>(addr),
         raw[0],  raw[1],  raw[2],  raw[3],
         raw[4],  raw[5],  raw[6],  raw[7],
         raw[8],  raw[9],  raw[10], raw[11],
         raw[12], raw[13], raw[14], raw[15]);

    // Trusted path: skip validation, hook directly
    if (isTrusted) {
        if (g_loadedDetourType == "V1" && !orig_v1) {
            orig_v1 = reinterpret_cast<OreUiInitFuncV1>(addr);
            if (detoursAttach(reinterpret_cast<void**>(&orig_v1), (void*)detour_v1)) {
                LOGI("  -> V1 Hooked (Trusted, skip validation)"); return true;
            }
            orig_v1 = nullptr;
        } else if (g_loadedDetourType == "V10" && !orig_v10) {
            orig_v10 = reinterpret_cast<OreUiInitFuncV10>(addr);
            if (detoursAttach(reinterpret_cast<void**>(&orig_v10), (void*)detour_v10)) {
                LOGI("  -> V10 Hooked (Trusted, skip validation)"); return true;
            }
            orig_v10 = nullptr;
        }
        return false;
    }

    // Untrusted path: hook, then wait for the callback to fire.
    //
    // 关键：超时没被调用 ≠ 地址错了。
    // 旧实现固定等 10 秒，然后 detach 换下一种 detour —— 但 OreUI 的配置函数
    // 往往只在进入某个界面时才调用一次，从启动到那时可能十几秒。10 秒就判死，
    // 会把正确的钩子丢掉，表现成 "全部签名失败"。
    // 现在：超时后保留钩子继续等（观察期），只有确认钩子确实坏了才 detach。

    // Try V1
    g_hookValid = false;
    orig_v1 = reinterpret_cast<OreUiInitFuncV1>(addr);
    if (detoursAttach(reinterpret_cast<void**>(&orig_v1), (void*)detour_v1)) {
        if (waitForHook("V1", g_scanDeadline)) {
            writeMatchedSignature(sigStr, "V1", forceWrite);
            LOGI("  -> V1 VALID!"); return true;
        }
        LOGE("  -> V1 installed but never called; wrong detour for this build.");
        detoursDetach(reinterpret_cast<void**>(&orig_v1), (void*)detour_v1);
        orig_v1 = nullptr;
    }

    // Try V10
    g_hookValid = false;
    orig_v10 = reinterpret_cast<OreUiInitFuncV10>(addr);
    if (detoursAttach(reinterpret_cast<void**>(&orig_v10), (void*)detour_v10)) {
        if (waitForHook("V10", g_scanDeadline)) {
            writeMatchedSignature(sigStr, "V10", forceWrite);
            LOGI("  -> V10 VALID!"); return true;
        }
        LOGE("  -> V10 installed but never called; wrong detour for this build.");
        detoursDetach(reinterpret_cast<void**>(&orig_v10), (void*)detour_v10);
        orig_v10 = nullptr;
    }

    return false;
}

// ============================================================
// Full hook installation with trusted-sig fast path + AutoFallback
// ============================================================
static bool tryInstallHook(const ModuleInfo& mod) {
    auto& sigs = loadSignatures();
    if (sigs.empty()) return false;

    // Validate trusted signature: invalidate if MC was updated
    bool isTrusted = !g_loadedDetourType.empty();
    if (isTrusted) {
        std::string sigPath = getConfigDir() + "signatures.json";
        try {
            std::ifstream f(sigPath);
            Json j = Json::parse(f, nullptr, false, true);
            if (!j.is_discarded()) {
                std::string savedId   = j.value("mc_id", "");
                std::string currentId = getMcVersionStr();
                if (!savedId.empty() && !currentId.empty() && savedId != currentId) {
                    LOGI("[Trusted] MC updated (%s -> %s), re-validating.",
                         savedId.c_str(), currentId.c_str());
                    isTrusted = false;
                }
            }
        } catch (...) { isTrusted = false; }
    }

    LOGI("Scanning %zu sig(s) (trusted=%s)...", sigs.size(), isTrusted ? "YES" : "NO");

    for (size_t i = 0; i < sigs.size(); ++i) {
        LOGI("  Sig[%zu/%zu]...", i + 1, sigs.size());
        uintptr_t addr = ResolveSignature(mod, sigs[i].c_str());
        if (!addr) { LOGI("  -> not found"); continue; }
        if (tryHookAt(addr, sigs[i].c_str(), isTrusted)) return true;
    }

    // AutoFallback: trusted sig expired, retry with all built-in sigs
    if (isTrusted) {
        LOGI("[AutoFallback] Trusted sig expired. Retrying with %zu built-in sig(s)...",
             SIG_FALLBACK.size());
        for (size_t i = 0; i < SIG_FALLBACK.size(); ++i) {
            LOGI("[AutoFallback] Sig[%zu/%zu]...", i + 1, SIG_FALLBACK.size());
            uintptr_t addr = ResolveSignature(mod, SIG_FALLBACK[i]);
            if (!addr) { LOGI("[AutoFallback]   -> not found"); continue; }
            // forceWrite=true: overwrite stale signatures.json
            if (tryHookAt(addr, SIG_FALLBACK[i], /*isTrusted=*/false, /*forceWrite=*/true)) {
                LOGI("[AutoFallback] Sig[%zu] matched — signatures.json updated.", i);
                return true;
            }
        }
        LOGI("[AutoFallback] All built-in sigs exhausted. New MC version needs a new signature.");
    }

    LOGI("All signatures exhausted.");
    return false;
}

// ============================================================
// Background injection thread (same 500ms warmup + 50 retries)
// ============================================================
static DWORD WINAPI InjectionThread(LPVOID) {
    LOGI("=== Injection thread started (tid=%lu) ===", GetCurrentThreadId());
    // Let MC finish its own early initialization before we scan
    Sleep(500);

    const int MAX_WAIT_MS = 30000, POLL_MS = 100;
    int waited = 0;
    ModuleInfo mod{};

    while (waited < MAX_WAIT_MS) {
        if (findMinecraftModule(mod)) {
            LOGI("Engine found at 0x%llx (%zu bytes) after %d ms",
                 static_cast<unsigned long long>(mod.base), mod.size, waited);
            break;
        }
        if (waited % 1000 == 0) LOGI("Waiting for module... (%d ms)", waited);
        Sleep(POLL_MS);
        waited += POLL_MS;
    }

    if (!mod.base) {
        LOGE("Module not found after %d ms.", MAX_WAIT_MS);
        return 0;
    }

    // 总预算。旧实现是 50 次 × (最多等 10 秒 x 2 detour)，最坏要跑十几分钟；
    // 现在每次挂钩自己带观察期，这里只兜住总量。
    g_scanDeadline = nowMs() + 300 * 1000;   // 5 分钟

    for (int attempt = 1; attempt <= 50; ++attempt) {
        if (outOfTime()) {
            LOGE("Scan budget exhausted at attempt %d.", attempt);
            break;
        }
        LOGI("=== Attempt %d/50 ===", attempt);
        if (tryInstallHook(mod)) {
            LOGI("=== Hook verified on attempt %d! ===", attempt);
            return 0;
        }
        Sleep(200);
    }

    LOGE("Failed. No signature matched or the target function was never called; "
         "a new signature may be needed for this MC version.");
    return 0;
}

// ============================================================
// DllMain — equivalent of __attribute__((constructor)) on Android
// ============================================================
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        getConfigDir();
        LOGI("=== ForceCloseOreUI Started (MC PE timestamp: %s) ===",
             getMcVersionStr().c_str());
        ensureConfigExists();

        HANDLE h = CreateThread(nullptr, 0, InjectionThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        else   LOGE("CreateThread failed (err=%lu)", GetLastError());
    }
    return TRUE;
}
