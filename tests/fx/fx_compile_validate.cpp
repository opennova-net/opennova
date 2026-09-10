// Compile-validates the authored .fx shader sources (tests/fixtures/fx/)
// through the same front door retail uses: D3DXCreateEffect with the loader's
// define set — FFPTRANSPOSE + a filter macro, TEX_UVXFORM for the EffectAlt_UV
// twin [orig: HLSLEffect_LoadFromFile @ 0x5ae690], and the fixed-function
// file's 24 variant define sets [orig: HLSLEffect_InitFixedFunctionShaders
// @ 0x5af790: {TEX_SINGLE,TEX_MULTIPLE} x {,SELFLUM} x
// {BLEND_NONE,BLEND_ALPHA,BLEND_ADD} x {,TEX_UVXFORM}]. A shader that fails
// here is a shader retail silently drops at boot — this test is the loud
// version of that silence.
//
// Needs the D3DX9 runtime (d3dx9_36.dll) and a creatable D3D9 device; when
// either is missing (CI, headless boxes) the whole test reports Skipped (77).
// The include handler resolves from the source dir, mirroring the VFS-backed
// resolution the game performs.

#ifndef _WIN32
#include <cstdio>
int main() {
	std::printf("SKIP: fx_compile_validate is Windows-only (D3DX9 effect compiler)\n");
	return 77;
}
#else

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// --- The minimal d3dx9 surface, declared by hand: the Windows SDK no longer
// ships d3dx9.h, and only these entry points and vtables are touched. Shapes
// per the last shipped D3DX9 headers (June 2010 DirectX SDK). ---
struct D3DXMACRO {
	LPCSTR Name;
	LPCSTR Definition;
};

typedef enum _D3DXINCLUDE_TYPE {
	D3DXINC_LOCAL = 0,
	D3DXINC_SYSTEM = 1,
} D3DXINCLUDE_TYPE;

struct ID3DXInclude {
	virtual HRESULT __stdcall Open(D3DXINCLUDE_TYPE include_type, LPCSTR file_name,
	                               LPCVOID parent_data, LPCVOID *data, UINT *bytes) = 0;
	virtual HRESULT __stdcall Close(LPCVOID data) = 0;
};

struct ID3DXBuffer : public IUnknown {
	virtual LPVOID __stdcall GetBufferPointer() = 0;
	virtual DWORD __stdcall GetBufferSize() = 0;
};

typedef HRESULT(WINAPI *D3DXCreateEffectFn)(IDirect3DDevice9 *device, LPCVOID src_data,
                                            UINT src_len, const D3DXMACRO *defines,
                                            ID3DXInclude *include, DWORD flags, void *pool,
                                            IUnknown **effect, ID3DXBuffer **errors);

namespace {

bool read_bytes(const fs::path &p, std::vector<uint8_t> &b) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	b.resize(static_cast<size_t>(sz));
	if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return f.good() || b.empty();
}

// Include handler over the source dir; owns each opened buffer until Close.
struct FxIncludeHandler : public ID3DXInclude {
	fs::path dir;
	std::map<const void *, std::vector<uint8_t> *> open_buffers;

	HRESULT __stdcall Open(D3DXINCLUDE_TYPE, LPCSTR file_name, LPCVOID, LPCVOID *data,
	                       UINT *bytes) override {
		auto *buf = new std::vector<uint8_t>();
		if (!read_bytes(dir / file_name, *buf) || buf->empty()) {
			delete buf;
			std::fprintf(stderr, "  include miss: %s\n", file_name);
			return E_FAIL;
		}
		*data = buf->data();
		*bytes = static_cast<UINT>(buf->size());
		open_buffers[buf->data()] = buf;
		return S_OK;
	}
	HRESULT __stdcall Close(LPCVOID data) override {
		auto it = open_buffers.find(data);
		if (it != open_buffers.end()) {
			delete it->second;
			open_buffers.erase(it);
		}
		return S_OK;
	}
};

struct Variant {
	std::string label;
	std::vector<const char *> defines;
};

int failures = 0;

void compile_one(D3DXCreateEffectFn create_effect, IDirect3DDevice9 *device,
                 const fs::path &src_dir, const char *name, const Variant &variant) {
	std::vector<uint8_t> src;
	if (!read_bytes(src_dir / name, src) || src.empty()) {
		std::fprintf(stderr, "FAIL: source missing: %s\n", name);
		++failures;
		return;
	}

	std::vector<D3DXMACRO> macros;
	for (const char *d : variant.defines) macros.push_back({d, "1"});
	macros.push_back({nullptr, nullptr});

	FxIncludeHandler include;
	include.dir = src_dir;

	IUnknown *effect = nullptr;
	ID3DXBuffer *errors = nullptr;
	const HRESULT hr = create_effect(device, src.data(), static_cast<UINT>(src.size()),
	                                 macros.data(), &include, 0, nullptr, &effect, &errors);
	if (FAILED(hr)) {
		std::fprintf(stderr, "FAIL: %s [%s] hr=0x%08lX\n", name, variant.label.c_str(),
		             static_cast<unsigned long>(hr));
		if (errors != nullptr && errors->GetBufferPointer() != nullptr) {
			std::fprintf(stderr, "%.*s\n", static_cast<int>(errors->GetBufferSize()),
			             static_cast<const char *>(errors->GetBufferPointer()));
		}
		++failures;
	} else {
		std::printf("ok: %s [%s]\n", name, variant.label.c_str());
	}
	if (errors != nullptr) errors->Release();
	if (effect != nullptr) effect->Release();
}

} // namespace

int main() {
	const fs::path src_dir(FX_SOURCE_DIR);

	HMODULE d3dx = LoadLibraryA("d3dx9_36.dll");
	if (d3dx == nullptr) {
		std::printf("SKIP: d3dx9_36.dll not available (install the DirectX End-User Runtime "
		            "to compile-validate the fx sources)\n");
		return 77;
	}
	auto create_effect =
	    reinterpret_cast<D3DXCreateEffectFn>(GetProcAddress(d3dx, "D3DXCreateEffect"));
	if (create_effect == nullptr) {
		std::printf("SKIP: d3dx9_36.dll exports no D3DXCreateEffect\n");
		return 77;
	}

	IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (d3d == nullptr) {
		std::printf("SKIP: Direct3DCreate9 failed (no D3D9 runtime)\n");
		return 77;
	}
	HWND wnd = CreateWindowExA(0, "STATIC", "fx_compile_validate", WS_OVERLAPPED, 0, 0, 1, 1,
	                           nullptr, nullptr, nullptr, nullptr);
	D3DPRESENT_PARAMETERS pp{};
	pp.Windowed = TRUE;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.BackBufferFormat = D3DFMT_UNKNOWN;
	pp.hDeviceWindow = wnd;
	IDirect3DDevice9 *device = nullptr;
	HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
	                               D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
	if (FAILED(hr)) {
		hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, wnd,
		                       D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
	}
	if (FAILED(hr) || device == nullptr) {
		std::printf("SKIP: no creatable D3D9 device (headless box?)\n");
		d3d->Release();
		return 77;
	}

	// The loader's base define set. TRILINEAR stands in for the filter macro
	// slot; ANISO only swaps sampler states, not code shape.
	const Variant base{"FFPTRANSPOSE+TRILINEAR", {"FFPTRANSPOSE", "TRILINEAR"}};
	const Variant base_uv{"FFPTRANSPOSE+TRILINEAR+TEX_UVXFORM",
	                      {"FFPTRANSPOSE", "TRILINEAR", "TEX_UVXFORM"}};

	// Standalone effects (the underscore-prefixed files are includes; the
	// loose override walk skips them and archived they fail the standalone
	// compile by design, exactly like retail's own include set).
	const char *const effects[] = {"phongt.fx",    "skbasic.fx",   "skbdifft2.fx",
	                               "skbdiffo.fx",  "skbdiffo2.fx", "skbphongt.fx",
	                               "skbphongo.fx"};
	const char *const effects_alt_uv[] = {"phongt.fx", "skbasic.fx"};

	for (const char *name : effects) compile_one(create_effect, device, src_dir, name, base);
	for (const char *name : effects_alt_uv)
		compile_one(create_effect, device, src_dir, name, base_uv);

	// _ffp.fx: the 24 fixed-function variants.
	const char *const tex[] = {"TEX_SINGLE", "TEX_MULTIPLE"};
	const char *const blend[] = {"BLEND_NONE", "BLEND_ALPHA", "BLEND_ADD"};
	for (const char *t : tex) {
		for (int lum = 0; lum < 2; ++lum) {
			for (const char *b : blend) {
				for (int uv = 0; uv < 2; ++uv) {
					Variant v;
					v.defines = {"FFPTRANSPOSE", "TRILINEAR", t, b};
					v.label = std::string(t) + "+" + b;
					if (lum) {
						v.defines.push_back("SELFLUM");
						v.label += "+SELFLUM";
					}
					if (uv) {
						v.defines.push_back("TEX_UVXFORM");
						v.label += "+UV";
					}
					compile_one(create_effect, device, src_dir, "_ffp.fx", v);
				}
			}
		}
	}

	device->Release();
	d3d->Release();
	if (wnd != nullptr) DestroyWindow(wnd);

	if (failures) {
		std::fprintf(stderr, "%d fx compile failures\n", failures);
		return 1;
	}
	std::printf("OK: every authored effect compiles under the loader's define sets\n");
	return 0;
}
#endif
