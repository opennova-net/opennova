#ifdef _WIN32

#if !defined(_M_IX86) && !defined(__i386__)
#error "modsuperoed_hook must be built as 32-bit x86"
#endif

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr unsigned long long kExpectedExeSize = 2473984ull;
constexpr size_t kRvaDocCtor = 0x00006760;
constexpr size_t kRvaDocOpen = 0x00006c60;
constexpr size_t kRvaExport3di = 0x0000da80;
// IDA: ExportSubobjectTemp_Build @ 0x457360 (RVA 0x57360)
// Per `mcp__ida-pro-mcp__decompile @ 0x457360`:
//   void __cdecl ExportSubobjectTemp_Build(ExportSubobjectTemp* temp,
//                                          LodHeader* lod, int subObjIndex);
// First 9 bytes: 55 8B EC 81 EC 50 01 00 00  ->
//   push ebp;  mov ebp, esp;  sub esp, 150h
// Three whole instructions, ≥5 bytes total — clean detour boundary.
constexpr size_t kRvaExportSubobjectTempBuild = 0x00057360;
constexpr size_t kExportSubobjectTempBuildPatchSize = 9;

using FnDocCtor = void *(__thiscall *)(void *self);
using FnDocOpen = int(__thiscall *)(void *self, const char *path);
using FnExport3di = void(__thiscall *)(void *self, const char *path,
                                       const char *title, unsigned char show_msg);

// IDA-derived layouts used by the diagnostic dump.
// Source of truth: `mcp__ida-pro-mcp__type_query` against ModSuperOed.exe.i64.
// (Inline structs to avoid pulling in IDA-side headers.)
struct OedFace {                                  // sizeof = 0x6c (108)
    int32_t   matIndex;        // 0x00
    int32_t   srcMatId;        // 0x04
    int32_t   smoothingGroup;  // 0x08
    uint32_t  vert[4];         // 0x0c
    uint32_t  uv[4];           // 0x1c
    uint32_t  edge_visibility[4]; // 0x2c
    float     faceBasis[3][3]; // 0x3c  (m[0]=normal, m[1]=tangent, m[2]=binormal)
    uint32_t  planeIndex;      // 0x60
    uint32_t  srcObjIndex;     // 0x64
    uint8_t   matSlot;         // 0x68
    uint8_t   hasDollar;       // 0x69
    uint8_t   collOverlap;     // 0x6a
    uint8_t   pad6b;           // 0x6b
};
static_assert(sizeof(OedFace) == 0x6c, "OedFace layout mismatch");

struct OedSubObject {                             // sizeof = 0x54 (84)
    char      name[32];
    uint32_t  flags;
    uint32_t  normalCount;
    uint32_t  attachIndex;
    uint32_t  unk2c;
    uint32_t  vertCount;
    void *    verts;
    uint32_t  uvCount;
    void *    uvs;
    int32_t   faceCount;       // 0x40
    OedFace * faces;           // 0x44
    uint32_t  colorCount;
    void *    colors;
    uint32_t  collisionCount;
};
static_assert(sizeof(OedSubObject) == 0x54, "OedSubObject layout mismatch");

struct OedExportTriFaceTemp {                     // sizeof = 0x6c (108)
    float normal[3][3];        // 0x00
    float tangent[3][3];       // 0x24
    float binormal[3][3];      // 0x48
};
static_assert(sizeof(OedExportTriFaceTemp) == 0x6c,
              "OedExportTriFaceTemp layout mismatch");

struct OedExportSubobjectTemp {                   // sizeof = 0x0c (12)
    OedExportTriFaceTemp * data;
    uint32_t size;
    uint32_t capacity;
};
static_assert(sizeof(OedExportSubobjectTemp) == 0x0c,
              "OedExportSubobjectTemp layout mismatch");

using FnExportSubobjectTempBuild =
    void(__cdecl *)(OedExportSubobjectTemp *temp, void *lod, int subObjIndex);

struct X86Detour {
    BYTE *target = nullptr;
    BYTE original[16]{};
    size_t patch_size = 0;
    BYTE *trampoline = nullptr;
    bool installed = false;
};

struct IatPatch {
    void **slot = nullptr;
    void *original = nullptr;
};

HMODULE g_self = nullptr;
HMODULE g_exe_base = nullptr;
void *g_doc_base = nullptr;
DWORD g_main_thread_id = 0;
HHOOK g_msg_hook = nullptr;

X86Detour g_doc_ctor_detour;
X86Detour g_doc_open_detour;
X86Detour g_export_subobject_temp_build_detour;
FnDocCtor g_orig_doc_ctor = nullptr;
FnDocOpen g_orig_doc_open = nullptr;
FnExportSubobjectTempBuild g_orig_export_subobject_temp_build = nullptr;
FnExport3di g_export_3di = nullptr;
std::atomic<uint32_t> g_export_subobject_temp_build_call_index{0};
using FnMessageBoxA = int(WINAPI *)(HWND, LPCSTR, LPCSTR, UINT);
FnMessageBoxA g_orig_message_box_a = nullptr;
IatPatch g_message_box_a_patch;

std::atomic<bool> g_auto_load_requested{false};
std::atomic<bool> g_auto_load_done{false};
std::atomic<bool> g_auto_export_requested{false};
std::atomic<bool> g_auto_export_ready{false};
std::atomic<bool> g_auto_export_done{false};
std::atomic<bool> g_exit_queued{false};

DWORD g_import_delay_ms = 5000;
DWORD g_export_delay_ms = 5000;
DWORD g_timeout_ms = 120000;

char g_project_path[MAX_PATH]{};
char g_export_path[MAX_PATH]{};
char g_export_title[MAX_PATH]{};
char g_log_dir[MAX_PATH]{};

void ensure_dir(const char *path) {
    if (path && path[0]) {
        CreateDirectoryA(path, nullptr);
    }
}

void set_path(char *dst, size_t dst_size, const std::string &value) {
    if (!dst || dst_size == 0 || value.empty()) {
        return;
    }
    std::strncpy(dst, value.c_str(), dst_size - 1);
    dst[dst_size - 1] = '\0';
}

std::string narrow(LPCWSTR wide) {
    if (!wide) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide, -1, &out[0], needed, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') {
        out.pop_back();
    }
    return out;
}

const char *log_base_dir() {
    static char base[MAX_PATH]{};
    if (base[0]) {
        return base;
    }
    if (g_log_dir[0]) {
        std::strncpy(base, g_log_dir, sizeof(base) - 1);
        base[sizeof(base) - 1] = '\0';
    } else {
        GetModuleFileNameA(g_self, base, MAX_PATH);
        char *slash = std::strrchr(base, '\\');
        if (slash) {
            *slash = '\0';
        }
    }
    ensure_dir(base);
    return base;
}

void log_line(const char *fmt, ...) {
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\oed_hook.log", log_base_dir());
    FILE *file = std::fopen(path, "a");
    if (!file) {
        return;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fprintf(file, "\n");
    std::fclose(file);
}

// Phase 3 Task 3.2 diagnostic dump: Capture face-basis inputs and smoothed
// outputs of `ExportSubobjectTemp_Build @ 0x457360` so they can be diffed
// against OpenNova's `compute_smoothed_vectors` (libs/object/src/rdta.cpp).
//
// File: <log_base_dir>/oed_face_basis_dump.txt, line-oriented.
// Each float is printed as a hex bit pattern so divergence shows up at the
// ULP level. Gated on env `OED_DUMP_FACE_BASIS=1` so production OED runs
// pay zero cost.
bool face_basis_dump_enabled() {
    static int cached = -1;
    if (cached < 0) {
        char buf[8]{};
        const DWORD got = GetEnvironmentVariableA("OED_DUMP_FACE_BASIS", buf, sizeof(buf));
        cached = (got > 0 && got < sizeof(buf) && buf[0] != '0' && buf[0] != '\0') ? 1 : 0;
    }
    return cached != 0;
}

const char *face_basis_dump_path() {
    static char path[MAX_PATH]{};
    if (!path[0]) {
        std::snprintf(path, sizeof(path), "%s\\oed_face_basis_dump.txt",
                      log_base_dir());
    }
    return path;
}

void truncate_face_basis_dump() {
    if (!face_basis_dump_enabled()) {
        return;
    }
    HANDLE file = CreateFileA(face_basis_dump_path(), GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

inline uint32_t float_bits(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

void dump_face_basis_pre(uint32_t call_index, void *lod, int subObjIndex) {
    if (!face_basis_dump_enabled() || !lod) {
        return;
    }
    BYTE *lod_bytes = reinterpret_cast<BYTE *>(lod);
    OedSubObject *sub_array =
        *reinterpret_cast<OedSubObject **>(lod_bytes + 0x74);
    if (!sub_array) {
        return;
    }
    const OedSubObject &sub = sub_array[subObjIndex];
    FILE *file = std::fopen(face_basis_dump_path(), "a");
    if (!file) {
        return;
    }
    std::fprintf(file,
                 "SUBOBJ call=%u sub_index=%d face_count=%d vert_count=%u name=\"%.32s\"\n",
                 call_index, subObjIndex, sub.faceCount, sub.vertCount, sub.name);
    if (sub.faces) {
        for (int32_t i = 0; i < sub.faceCount; ++i) {
            const OedFace &f = sub.faces[i];
            std::fprintf(
                file,
                "FACE call=%u sub=%d i=%d sg=%d dollar=%u v=(%u,%u,%u) "
                "n=(0x%08x,0x%08x,0x%08x) "
                "t=(0x%08x,0x%08x,0x%08x) "
                "b=(0x%08x,0x%08x,0x%08x)\n",
                call_index, subObjIndex, i, f.smoothingGroup,
                static_cast<unsigned>(f.hasDollar),
                f.vert[0], f.vert[1], f.vert[2],
                float_bits(f.faceBasis[0][0]), float_bits(f.faceBasis[0][1]),
                float_bits(f.faceBasis[0][2]),
                float_bits(f.faceBasis[1][0]), float_bits(f.faceBasis[1][1]),
                float_bits(f.faceBasis[1][2]),
                float_bits(f.faceBasis[2][0]), float_bits(f.faceBasis[2][1]),
                float_bits(f.faceBasis[2][2]));
        }
    }
    std::fclose(file);
}

void dump_face_basis_post(uint32_t call_index, int subObjIndex,
                          OedExportSubobjectTemp *temp) {
    if (!face_basis_dump_enabled() || !temp || !temp->data) {
        return;
    }
    FILE *file = std::fopen(face_basis_dump_path(), "a");
    if (!file) {
        return;
    }
    for (uint32_t i = 0; i < temp->size; ++i) {
        const OedExportTriFaceTemp &t = temp->data[i];
        for (int j = 0; j < 3; ++j) {
            std::fprintf(
                file,
                "SMOOTH call=%u sub=%d face=%u v=%d "
                "n=(0x%08x,0x%08x,0x%08x) "
                "t=(0x%08x,0x%08x,0x%08x) "
                "b=(0x%08x,0x%08x,0x%08x)\n",
                call_index, subObjIndex, i, j,
                float_bits(t.normal[j][0]), float_bits(t.normal[j][1]),
                float_bits(t.normal[j][2]),
                float_bits(t.tangent[j][0]), float_bits(t.tangent[j][1]),
                float_bits(t.tangent[j][2]),
                float_bits(t.binormal[j][0]), float_bits(t.binormal[j][1]),
                float_bits(t.binormal[j][2]));
        }
    }
    std::fprintf(file, "END call=%u sub=%d\n", call_index, subObjIndex);
    std::fclose(file);
}

// Detour wrapper installed at 0x457360. Calls the trampoline in between the
// pre/post dumps so we observe both the inputs (face basis fed to the
// smoothing accumulator) and the outputs (post-NormalizeVec3 smoothed
// normal/tangent/binormal per face vertex).
void __cdecl detour_export_subobject_temp_build(OedExportSubobjectTemp *temp,
                                                void *lod, int subObjIndex) {
    const uint32_t idx = g_export_subobject_temp_build_call_index.fetch_add(1);
    dump_face_basis_pre(idx, lod, subObjIndex);
    if (g_orig_export_subobject_temp_build) {
        g_orig_export_subobject_temp_build(temp, lod, subObjIndex);
    }
    dump_face_basis_post(idx, subObjIndex, temp);
}

bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    const size_t needle_len = std::strlen(needle);
    for (const char *cursor = haystack; *cursor; ++cursor) {
        size_t i = 0;
        for (; i < needle_len; ++i) {
            const char a = cursor[i];
            const char b = needle[i];
            if (!a) {
                return false;
            }
            const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a + 32) : a;
            const char lb = (b >= 'A' && b <= 'Z') ? static_cast<char>(b + 32) : b;
            if (la != lb) {
                break;
            }
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

bool is_unused_material_prompt(const char *text) {
    return contains_case_insensitive(text, "material") &&
           contains_case_insensitive(text, "referenc") &&
           contains_case_insensitive(text, "remove");
}

int WINAPI detour_message_box_a(HWND hwnd, LPCSTR text, LPCSTR caption, UINT type) {
    log_line("MessageBoxA caption=\"%s\" text=\"%s\" type=0x%08lx",
             caption ? caption : "", text ? text : "", static_cast<unsigned long>(type));
    if (is_unused_material_prompt(text)) {
        log_line("HOOK_ERROR unused-material prompt detected; dismissing with IDNO to unblock headless run");
        return IDNO;
    }
    return g_orig_message_box_a ? g_orig_message_box_a(hwnd, text, caption, type) : IDOK;
}

void truncate_log() {
    char path[MAX_PATH]{};
    std::snprintf(path, sizeof(path), "%s\\oed_hook.log", log_base_dir());
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
    }
}

bool current_exe_size_matches() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attr)) {
        log_line("Failed to stat current exe: %lu", GetLastError());
        return false;
    }
    ULARGE_INTEGER size{};
    size.LowPart = attr.nFileSizeLow;
    size.HighPart = attr.nFileSizeHigh;
    if (size.QuadPart != kExpectedExeSize) {
        log_line("Unexpected ModSuperOED size: %llu != %llu",
                 static_cast<unsigned long long>(size.QuadPart), kExpectedExeSize);
        return false;
    }
    return true;
}

void default_export_path() {
    if (g_export_path[0] || !g_project_path[0]) {
        return;
    }
    std::strncpy(g_export_path, g_project_path, sizeof(g_export_path) - 1);
    g_export_path[sizeof(g_export_path) - 1] = '\0';
    char *dot = std::strrchr(g_export_path, '.');
    if (dot) {
        *dot = '\0';
    }
    std::strncat(g_export_path, ".3di",
                 sizeof(g_export_path) - std::strlen(g_export_path) - 1);
}

void build_export_title() {
    if (g_export_title[0]) {
        return;
    }
    const char *name = std::strrchr(g_export_path, '\\');
    name = name ? name + 1 : g_export_path;
    if (!name || !name[0]) {
        name = "export.3di";
    }
    std::strncpy(g_export_title, name, sizeof(g_export_title) - 1);
    g_export_title[sizeof(g_export_title) - 1] = '\0';
}

DWORD env_dword(const char *name, DWORD fallback) {
    char buf[64]{};
    const DWORD got = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (got == 0 || got >= sizeof(buf)) {
        return fallback;
    }
    return std::strtoul(buf, nullptr, 10);
}

void apply_overrides_from_cmdline() {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            std::string arg = narrow(argv[i]);
            auto starts = [&](const char *key) {
                return arg.rfind(key, 0) == 0;
            };
            if (starts("--oed-proj=")) {
                set_path(g_project_path, sizeof(g_project_path),
                         arg.substr(std::strlen("--oed-proj=")));
            } else if (starts("--oed-3di=")) {
                set_path(g_export_path, sizeof(g_export_path),
                         arg.substr(std::strlen("--oed-3di=")));
            } else if (starts("--oed-title=")) {
                set_path(g_export_title, sizeof(g_export_title),
                         arg.substr(std::strlen("--oed-title=")));
            } else if (starts("--oed-log-dir=")) {
                set_path(g_log_dir, sizeof(g_log_dir),
                         arg.substr(std::strlen("--oed-log-dir=")));
            } else if (starts("--oed-import-delay-ms=")) {
                g_import_delay_ms =
                    std::strtoul(arg.c_str() + std::strlen("--oed-import-delay-ms="),
                                 nullptr, 10);
            } else if (starts("--oed-export-delay-ms=")) {
                g_export_delay_ms =
                    std::strtoul(arg.c_str() + std::strlen("--oed-export-delay-ms="),
                                 nullptr, 10);
            } else if (starts("--oed-timeout-ms=")) {
                g_timeout_ms =
                    std::strtoul(arg.c_str() + std::strlen("--oed-timeout-ms="),
                                 nullptr, 10);
            }
        }
        LocalFree(argv);
    }

    char env_path[MAX_PATH]{};
    if (!g_project_path[0]) {
        DWORD got = GetEnvironmentVariableA("OED_AUTO_3DP", env_path, sizeof(env_path));
        if (got > 0 && got < sizeof(env_path)) {
            set_path(g_project_path, sizeof(g_project_path), env_path);
        }
    }
    if (!g_export_path[0]) {
        DWORD got = GetEnvironmentVariableA("OED_AUTO_3DI", env_path, sizeof(env_path));
        if (got > 0 && got < sizeof(env_path)) {
            set_path(g_export_path, sizeof(g_export_path), env_path);
        }
    }
    if (!g_log_dir[0]) {
        DWORD got = GetEnvironmentVariableA("OED_LOG_DIR", env_path, sizeof(env_path));
        if (got > 0 && got < sizeof(env_path)) {
            set_path(g_log_dir, sizeof(g_log_dir), env_path);
        }
    }

    g_import_delay_ms = env_dword("OED_IMPORT_DELAY_MS", g_import_delay_ms);
    g_export_delay_ms = env_dword("OED_EXPORT_DELAY_MS", g_export_delay_ms);
    g_timeout_ms = env_dword("OED_TIMEOUT_MS", g_timeout_ms);
    default_export_path();
    build_export_title();
}

bool write_rel_jump(BYTE *at, const void *to) {
    const intptr_t rel = reinterpret_cast<const BYTE *>(to) - at - 5;
    if (rel < INT32_MIN || rel > INT32_MAX) {
        return false;
    }
    at[0] = 0xE9;
    const int32_t rel32 = static_cast<int32_t>(rel);
    std::memcpy(at + 1, &rel32, sizeof(rel32));
    return true;
}

bool install_detour(const char *name, BYTE *target, size_t patch_size, void *replacement,
                    X86Detour &detour, void **original_out) {
    if (patch_size < 5 || patch_size > sizeof(detour.original)) {
        log_line("%s detour failed: unsupported patch size %u", name,
                 static_cast<unsigned>(patch_size));
        return false;
    }
    detour.target = target;
    detour.patch_size = patch_size;
    std::memcpy(detour.original, target, patch_size);
    detour.trampoline = static_cast<BYTE *>(
        VirtualAlloc(nullptr, patch_size + 5, MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!detour.trampoline) {
        log_line("%s detour failed: trampoline allocation error %lu", name, GetLastError());
        return false;
    }

    std::memcpy(detour.trampoline, detour.original, patch_size);
    if (!write_rel_jump(detour.trampoline + patch_size, target + patch_size)) {
        log_line("%s detour failed: trampoline jump out of range", name);
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
        return false;
    }

    DWORD old_protect = 0;
    if (!VirtualProtect(target, patch_size, PAGE_EXECUTE_READWRITE, &old_protect)) {
        log_line("%s detour failed: VirtualProtect error %lu", name, GetLastError());
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
        return false;
    }
    std::memset(target, 0x90, patch_size);
    const bool ok = write_rel_jump(target, replacement);
    DWORD ignored = 0;
    VirtualProtect(target, patch_size, old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), target, patch_size);
    if (!ok) {
        log_line("%s detour failed: target jump out of range", name);
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
        return false;
    }

    detour.installed = true;
    *original_out = detour.trampoline;
    log_line("%s detour installed at %p", name, target);
    return true;
}

void remove_detour(X86Detour &detour) {
    if (detour.installed && detour.target) {
        DWORD old_protect = 0;
        if (VirtualProtect(detour.target, detour.patch_size, PAGE_EXECUTE_READWRITE,
                           &old_protect)) {
            std::memcpy(detour.target, detour.original, detour.patch_size);
            DWORD ignored = 0;
            VirtualProtect(detour.target, detour.patch_size, old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), detour.target, detour.patch_size);
        }
    }
    if (detour.trampoline) {
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
    }
    detour = {};
}

bool patch_iat_function(HMODULE module,
                        const char *dll_name,
                        const char *function_name,
                        void *replacement,
                        IatPatch &patch,
                        void **original_out) {
    BYTE *base = reinterpret_cast<BYTE *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const IMAGE_DATA_DIRECTORY &dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        return false;
    }

    auto *imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress);
    for (; imports->Name; ++imports) {
        const char *import_dll = reinterpret_cast<const char *>(base + imports->Name);
        if (lstrcmpiA(import_dll, dll_name) != 0) {
            continue;
        }
        auto *orig_thunk = reinterpret_cast<IMAGE_THUNK_DATA32 *>(
            base + (imports->OriginalFirstThunk ? imports->OriginalFirstThunk
                                                : imports->FirstThunk));
        auto *first_thunk = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + imports->FirstThunk);
        for (; orig_thunk->u1.AddressOfData; ++orig_thunk, ++first_thunk) {
            if (orig_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
                continue;
            }
            auto *by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(
                base + orig_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), function_name) != 0) {
                continue;
            }

            void **slot = reinterpret_cast<void **>(&first_thunk->u1.Function);
            DWORD old_protect = 0;
            if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
                log_line("IAT patch %s failed: VirtualProtect error %lu",
                         function_name, GetLastError());
                return false;
            }
            patch.slot = slot;
            patch.original = *slot;
            *slot = replacement;
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(void *), old_protect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void *));
            *original_out = patch.original;
            log_line("IAT patched %s!%s at %p (orig=%p replacement=%p)",
                     dll_name, function_name, slot, patch.original, replacement);
            return true;
        }
    }
    log_line("IAT import %s!%s not found", dll_name, function_name);
    return false;
}

void remove_iat_patch(IatPatch &patch) {
    if (!patch.slot || !patch.original) {
        return;
    }
    DWORD old_protect = 0;
    if (VirtualProtect(patch.slot, sizeof(void *), PAGE_READWRITE, &old_protect)) {
        *patch.slot = patch.original;
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(void *), old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), patch.slot, sizeof(void *));
    }
    patch = {};
}

void post_ui_thread() {
    if (g_main_thread_id) {
        PostThreadMessageA(g_main_thread_id, WM_NULL, 0, 0);
    }
}

DWORD WINAPI export_timer_thread(LPVOID) {
    log_line("Auto-export timer armed for %lu ms", g_export_delay_ms);
    Sleep(g_export_delay_ms);
    g_auto_export_ready.store(true);
    post_ui_thread();
    log_line("Auto-export timer elapsed");
    return 0;
}

void arm_auto_export() {
    bool expected = false;
    if (!g_auto_export_requested.compare_exchange_strong(expected, true)) {
        return;
    }
    g_auto_export_ready.store(false);
    build_export_title();
    HANDLE thread = CreateThread(nullptr, 0, export_timer_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        log_line("Failed to create export timer thread: %lu", GetLastError());
    }
}

void queue_exit(DWORD code) {
    bool expected = false;
    if (g_exit_queued.compare_exchange_strong(expected, true)) {
        log_line("ExitProcess(%lu)", code);
        ExitProcess(code);
    }
}

LRESULT CALLBACK message_hook_proc(int code, WPARAM w_param, LPARAM l_param) {
    if (code >= 0) {
        if (g_auto_load_requested.load() && !g_auto_load_done.load() && g_doc_base &&
            g_orig_doc_open && g_project_path[0]) {
            g_auto_load_done.store(true);
            log_line("Calling OnOpenDocument(\"%s\") on thread %lu", g_project_path,
                     GetCurrentThreadId());
            const int rc = g_orig_doc_open(g_doc_base, g_project_path);
            log_line("OnOpenDocument returned %d", rc);
            if (rc) {
                arm_auto_export();
            }
        }

        if (g_auto_export_requested.load() && g_auto_export_ready.load() &&
            !g_auto_export_done.load() && g_doc_base && g_export_3di && g_export_path[0]) {
            g_auto_export_done.store(true);
            log_line("Calling Export3di(\"%s\", \"%s\") on thread %lu", g_export_path,
                     g_export_title, GetCurrentThreadId());
            g_export_3di(g_doc_base, g_export_path, g_export_title, 1);
            log_line("Export3di returned");
            queue_exit(0);
        }
    }
    return CallNextHookEx(g_msg_hook, code, w_param, l_param);
}

int __fastcall detour_doc_open(void *self, void *, const char *path) {
    g_doc_base = self;
    g_auto_export_requested.store(false);
    g_auto_export_ready.store(false);
    g_auto_export_done.store(false);
    log_line("OnOpenDocument self=%p path=\"%s\"", self, path ? path : "(null)");
    if (path && !g_project_path[0]) {
        set_path(g_project_path, sizeof(g_project_path), path);
        default_export_path();
        build_export_title();
    }
    const int rc = g_orig_doc_open ? g_orig_doc_open(self, path) : 0;
    log_line("OnOpenDocument returned %d", rc);
    if (rc && g_export_path[0]) {
        arm_auto_export();
    }
    return rc;
}

void *__fastcall detour_doc_ctor(void *self, void *) {
    g_doc_base = self;
    g_main_thread_id = GetCurrentThreadId();
    void *result = g_orig_doc_ctor ? g_orig_doc_ctor(self) : self;
    if (result) {
        g_doc_base = result;
    }

    if (!g_msg_hook) {
        g_msg_hook = SetWindowsHookExA(WH_GETMESSAGE, message_hook_proc, nullptr,
                                       g_main_thread_id);
        if (g_msg_hook) {
            log_line("Installed WH_GETMESSAGE hook on thread %lu", g_main_thread_id);
        } else {
            log_line("SetWindowsHookExA failed: %lu", GetLastError());
        }
    }
    post_ui_thread();
    return result;
}

DWORD WINAPI auto_load_thread(LPVOID) {
    if (!g_project_path[0]) {
        return 0;
    }
    Sleep(g_import_delay_ms);
    g_auto_load_requested.store(true);
    post_ui_thread();
    log_line("Auto-load armed after %lu ms for \"%s\"", g_import_delay_ms,
             g_project_path);
    return 0;
}

DWORD WINAPI timeout_thread(LPVOID) {
    if (g_timeout_ms == 0) {
        return 0;
    }
    Sleep(g_timeout_ms);
    if (!g_auto_export_done.load()) {
        log_line("Timed out after %lu ms", g_timeout_ms);
        queue_exit(124);
    }
    return 0;
}

bool setup_hooks() {
    if (!current_exe_size_matches()) {
        return false;
    }

    g_exe_base = GetModuleHandleA(nullptr);
    if (!g_exe_base) {
        log_line("GetModuleHandleA(nullptr) failed: %lu", GetLastError());
        return false;
    }

    BYTE *base = reinterpret_cast<BYTE *>(g_exe_base);
    g_export_3di = reinterpret_cast<FnExport3di>(base + kRvaExport3di);
    patch_iat_function(g_exe_base,
                       "USER32.dll",
                       "MessageBoxA",
                       reinterpret_cast<void *>(&detour_message_box_a),
                       g_message_box_a_patch,
                       reinterpret_cast<void **>(&g_orig_message_box_a));

    bool ok = true;
    ok = install_detour("DocCtor", base + kRvaDocCtor, 5,
                        reinterpret_cast<void *>(&detour_doc_ctor),
                        g_doc_ctor_detour, reinterpret_cast<void **>(&g_orig_doc_ctor)) &&
         ok;
    ok = install_detour("DocOpen", base + kRvaDocOpen, 7,
                        reinterpret_cast<void *>(&detour_doc_open),
                        g_doc_open_detour, reinterpret_cast<void **>(&g_orig_doc_open)) &&
         ok;
    install_detour(
        "ExportSubobjectTempBuild",
        base + kRvaExportSubobjectTempBuild,
        kExportSubobjectTempBuildPatchSize,
        reinterpret_cast<void *>(&detour_export_subobject_temp_build),
        g_export_subobject_temp_build_detour,
        reinterpret_cast<void **>(&g_orig_export_subobject_temp_build));
    log_line("Hook setup complete base=%p DocCtor=0x%zx DocOpen=0x%zx Export3di=0x%zx ExportSubobjectTempBuild=0x%zx",
             base, kRvaDocCtor, kRvaDocOpen, kRvaExport3di,
             kRvaExportSubobjectTempBuild);
    return ok;
}

void remove_hooks() {
    if (g_msg_hook) {
        UnhookWindowsHookEx(g_msg_hook);
        g_msg_hook = nullptr;
    }
    remove_detour(g_export_subobject_temp_build_detour);
    remove_detour(g_doc_open_detour);
    remove_detour(g_doc_ctor_detour);
    remove_iat_patch(g_message_box_a_patch);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        apply_overrides_from_cmdline();
        truncate_log();
        truncate_face_basis_dump();
        log_line("ModSuperOED hook loaded proj=\"%s\" out=\"%s\" log=\"%s\" "
                 "importDelayMs=%lu exportDelayMs=%lu timeoutMs=%lu",
                 g_project_path[0] ? g_project_path : "(none)",
                 g_export_path[0] ? g_export_path : "(none)",
                 g_log_dir[0] ? g_log_dir : "(none)",
                 g_import_delay_ms, g_export_delay_ms, g_timeout_ms);
        if (!setup_hooks()) {
            log_line("Hook setup failed");
            ExitProcess(125);
        }
        if (g_project_path[0]) {
            HANDLE thread = CreateThread(nullptr, 0, auto_load_thread, nullptr, 0, nullptr);
            if (thread) {
                CloseHandle(thread);
            }
        }
        HANDLE timeout = CreateThread(nullptr, 0, timeout_thread, nullptr, 0, nullptr);
        if (timeout) {
            CloseHandle(timeout);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        remove_hooks();
    }
    return TRUE;
}

#else
int main() {
    return 0;
}
#endif
