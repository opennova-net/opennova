#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <opennova/retail_hook/windows/named_pipe_trace.h>
#include <opennova/retail_hook/windows/retail_cli.h>
#include <parity/parity.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace windows = opennova::retail_hook::windows;
namespace parity = opennova::parity;

namespace {

void print_usage() {
    std::wcout
        << L"Usage: retail_parity_inspector.exe "
           L"--pipe <name> --trace <bundle> --scenario <name> "
           L"--role-set <baseline|candidate>\n";
}

[[nodiscard]] const wchar_t* role_set_name(
    windows::ParityRoleSet role_set) noexcept {
    return role_set == windows::ParityRoleSet::baseline
        ? L"baseline"
        : L"candidate";
}

[[nodiscard]] std::string utf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int source_size = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        source_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            source_size,
            result.data(),
            required,
            nullptr,
            nullptr) != required) {
        return {};
    }
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::vector<std::wstring> arguments(argv, argv + argc);
    if (argc == 2 &&
        (arguments[1] == L"--help" || arguments[1] == L"-h")) {
        print_usage();
        return 0;
    }
    const windows::InspectorCliResult parsed =
        windows::parse_inspector_cli(arguments);
    if (!parsed) {
        std::wcerr << L"status=error phase=arguments detail="
                   << parsed.error << L"\n";
        print_usage();
        return 2;
    }

    const windows::InspectorOptions& options = parsed.options;
    std::wcout << L"status=waiting pipe=" << options.pipe_name
               << L" trace=" << options.trace_path
               << L" scenario=" << options.scenario
               << L" role_set=" << role_set_name(options.role_set)
               << L"\n";
    const windows::PipeRecordResult recorded =
        windows::record_trace_pipe(
            options.pipe_name,
            options.trace_path,
            windows::PipeRecordOptions{
                2,
                utf8(options.scenario),
                options.role_set == windows::ParityRoleSet::baseline
                    ? windows::PipeProducerTopology::retail_baseline
                    : windows::PipeProducerTopology::
                          opennova_host_retail_client,
            });
    if (!recorded.success) {
        std::wcerr << L"status=error phase=record win32_error="
                   << recorded.win32_error << L" bytes="
                   << recorded.bytes_written << L" connected="
                   << recorded.producers_connected << L" completed="
                   << recorded.producers_completed << L" invalid="
                   << recorded.invalid_chunks << L" detail="
                   << std::wstring(
                          recorded.detail.begin(),
                          recorded.detail.end())
                   << L"\n";
        return 3;
    }

    std::ifstream file(
        std::filesystem::path(options.trace_path),
        std::ios::binary);
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>(),
    };
    const parity::TraceReadResult trace = parity::read_trace(bytes);
    if (!trace.complete()) {
        std::wcerr << L"status=error phase=validate trace_status="
                   << static_cast<unsigned>(trace.status)
                   << L" offset=" << trace.error_offset << L"\n";
        return 4;
    }

    std::uint64_t dropped = 0;
    for (const parity::Event& event : trace.trace.events) {
        const auto* diagnostic =
            std::get_if<parity::DiagnosticEvent>(&event);
        if (diagnostic == nullptr ||
            diagnostic->code != "retail_capture_counts") {
            continue;
        }
        for (const parity::Field& field : diagnostic->context) {
            if (field.name.find("dropped") == std::string::npos &&
                field.name != "pipe_write_failures") {
                continue;
            }
            if (const auto* value =
                    std::get_if<std::uint64_t>(&field.value)) {
                dropped += *value;
            }
        }
    }

    std::wcout << L"status=complete bytes=" << recorded.bytes_written
               << L" events=" << trace.trace.events.size()
               << L" producers=" << recorded.producers_completed
               << L" invalid=" << recorded.invalid_chunks
               << L" dropped=" << dropped
               << L" scenario=" << options.scenario
               << L" role_set=" << role_set_name(options.role_set)
               << L"\n";
    for (const parity::Event& event : trace.trace.events) {
        if (const auto* checkpoint =
                std::get_if<parity::Checkpoint>(&event)) {
            std::wcout << L"checkpoint="
                       << std::wstring(
                              checkpoint->name.begin(),
                              checkpoint->name.end())
                       << L" frame=" << checkpoint->frame_index
                       << L" stream="
                       << std::wstring(
                              checkpoint->identity.stream_id.begin(),
                              checkpoint->identity.stream_id.end())
                       << L"\n";
        }
    }
    return 0;
}
