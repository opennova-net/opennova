#include <opennova/retail_hook/windows/retail_cli.h>

#include <cstdio>
#include <string>
#include <vector>

namespace windows = opennova::retail_hook::windows;

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

int main() {
    const windows::InjectorCliResult host =
        windows::parse_injector_cli({
            L"retail_hook_injector.exe",
            L"--game-dir", L"C:\\Games\\Joint Operations",
            L"--role", L"retail-host",
            L"--pipe", L"parity-retail-host",
            L"--run-id", L"run-42",
            L"--stream-id", L"retail-host-42",
            L"--scenario", L"revx02-smoke",
            L"--mission", L"training-range",
            L"--", L"/w", L"/exp", L"jox01", L"/MANY",
        });
    CHECK(host);
    CHECK(host.options.role == windows::RetailRole::host);
    CHECK(!host.options.allow_writes);
    CHECK(host.options.run_id == L"run-42");
    CHECK(host.options.stream_id == L"retail-host-42");
    CHECK(host.options.scenario == L"revx02-smoke");
    CHECK(host.options.mission == L"training-range");
    CHECK(host.options.game_arguments ==
          std::vector<std::wstring>(
              {L"/w", L"/exp", L"jox01", L"/MANY"}));

    const windows::InjectorCliResult writable =
        windows::parse_injector_cli({
            L"retail_hook_injector.exe",
            L"--allow-writes",
            L"--pipe", L"parity-retail-client",
            L"--role", L"retail-client",
            L"--game-dir", L"C:\\Games\\Joint Operations",
            L"--run-id", L"run-42",
            L"--stream-id", L"retail-client-42",
            L"--scenario", L"revx02-smoke",
            L"--mission", L"training-range",
            L"--", L"/w", L"/exp", L"jox01", L"/MANY",
        });
    CHECK(writable);
    CHECK(writable.options.role == windows::RetailRole::client);
    CHECK(writable.options.allow_writes);

    const windows::InjectorCliResult missing_many =
        windows::parse_injector_cli({
            L"retail_hook_injector.exe",
            L"--game-dir", L"C:\\Games\\Joint Operations",
            L"--role", L"retail-client",
            L"--pipe", L"parity-retail-client",
            L"--run-id", L"run-42",
            L"--stream-id", L"retail-client-42",
            L"--scenario", L"revx02-smoke",
            L"--mission", L"training-range",
            L"--", L"/w", L"/exp", L"jox01",
        });
    CHECK(!missing_many);

    const windows::InjectorCliResult wrong_forwarding =
        windows::parse_injector_cli({
            L"retail_hook_injector.exe",
            L"--game-dir", L"C:\\Games\\Joint Operations",
            L"--role", L"retail-host",
            L"--pipe", L"pipe",
            L"--run-id", L"run-42",
            L"--stream-id", L"retail-host-42",
            L"--scenario", L"revx02-smoke",
            L"--mission", L"training-range",
            L"--", L"/exp", L"jox01", L"/MANY",
        });
    CHECK(!wrong_forwarding);

    const windows::InjectorCliResult incomplete_metadata =
        windows::parse_injector_cli({
            L"retail_hook_injector.exe",
            L"--game-dir", L"C:\\Games\\Joint Operations",
            L"--role", L"retail-host",
            L"--pipe", L"pipe",
            L"--run-id", L"run-42",
            L"--stream-id", L"retail-host-42",
            L"--scenario", L"revx02-smoke",
            L"--", L"/w", L"/exp", L"jox01", L"/MANY",
        });
    CHECK(!incomplete_metadata);

    const windows::InspectorCliResult inspector =
        windows::parse_inspector_cli({
            L"retail_parity_inspector.exe",
            L"--pipe", L"parity-retail-host",
            L"--trace", L"C:\\trace\\retail-host.onpt",
            L"--scenario", L"revx02-smoke",
            L"--role-set", L"baseline",
        });
    CHECK(inspector);
    CHECK(inspector.options.role_set == windows::ParityRoleSet::baseline);
    CHECK(inspector.options.scenario == L"revx02-smoke");

    std::printf("retail_hook_cli: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
