#pragma once

#include <string>
#include <vector>

namespace opennova::retail_hook::windows {

enum class RetailRole {
    host,
    client,
};

struct InjectorOptions {
    std::wstring game_directory{};
    RetailRole role{RetailRole::host};
    std::wstring pipe_name{};
    std::wstring run_id{};
    std::wstring stream_id{};
    std::wstring scenario{};
    std::wstring mission{};
    bool allow_writes{};
    std::vector<std::wstring> game_arguments{};
};

struct InjectorCliResult {
    InjectorOptions options{};
    std::wstring error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

enum class ParityRoleSet {
    baseline,
    candidate,
};

struct InspectorOptions {
    std::wstring pipe_name{};
    std::wstring trace_path{};
    std::wstring scenario{};
    ParityRoleSet role_set{ParityRoleSet::baseline};
};

struct InspectorCliResult {
    InspectorOptions options{};
    std::wstring error{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error.empty();
    }
};

[[nodiscard]] InjectorCliResult parse_injector_cli(
    const std::vector<std::wstring>& arguments);
[[nodiscard]] InspectorCliResult parse_inspector_cli(
    const std::vector<std::wstring>& arguments);

}  // namespace opennova::retail_hook::windows
