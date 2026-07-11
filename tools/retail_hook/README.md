# Retail validation hook

This is a research-only, 32-bit Windows tool for checking OpenNova's reverse-
engineered Joint Operations layouts against a running retail process. The hook
reads process memory through the declarations in `libs/retail_abi`; it does not
carry a second set of private struct definitions.

## Supported executable

The only accepted build is the patched Joint Operations 1.7.5.7 executable
distributed by the original onHook tooling, with this SHA-256:

```text
9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57
```

The injector checks the file before launch, and the injected DLL checks the
running executable again before opening a validation session. Any mismatch is
rejected; adding another retail build requires a separately witnessed profile.

## Build and test

From the repository root in a Visual Studio developer environment:

```powershell
cmake -S . -B build-retail-hook -A Win32 `
  -DBUILD_SHARED_LIB=OFF `
  -DOPENNOVA_ENABLE_MODSUPEROED_TOOLS=OFF `
  -DOPENNOVA_ENABLE_RETAIL_HOOK_TOOLS=ON
cmake --build build-retail-hook --config Release --target `
  retail_hook_validation_test opennova_retail_hook opennova_retail_injector
ctest --test-dir build-retail-hook -C Release --output-on-failure `
  -R "^retail_hook_validation$"
```

The DLL and injector are written to
`build-retail-hook/tools/retail_hook/windows/Release/`.
The Windows targets are intentionally unavailable from a 64-bit configure;
the portable validation core and its synthetic-memory test remain host-neutral.

## Run

Pass a user-owned retail executable and the built DLL to the injector. Any
remaining arguments are forwarded to the game:

```powershell
.\build-retail-hook\tools\retail_hook\windows\Release\opennova_retail_injector.exe `
  "C:\Games\Joint Operations\Jointops.exe" `
  ".\build-retail-hook\tools\retail_hook\windows\Release\opennova_retail_hook.dll" `
  [game arguments...]
```

The injector starts the game under a controlled launch, waits for the Windows
loader to reach input-idle, then loads the DLL and starts its worker. The
validation window samples automatically every 750 ms.
Press F5 to sample immediately, use the mouse wheel or vertical scrollbar to
scroll, and press Esc to close the validation window.

The shipped Windows hook opens a read-only session: mutation access is disabled
and the process-memory adapter rejects writes. Keep retail executables, DLLs,
memory dumps, captures, and other proprietary retail bytes out of this repository;
the project contains source and synthetic tests only.
