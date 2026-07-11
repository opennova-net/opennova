#include <opennova/retail_hook/windows/retail_cli.h>

#include <utility>

namespace opennova::retail_hook::windows {
namespace {

template <typename Result>
[[nodiscard]] Result fail(std::wstring detail) {
    Result result{};
    result.error = std::move(detail);
    return result;
}

[[nodiscard]] bool take_value(
    const std::vector<std::wstring>& arguments,
    std::size_t& index,
    std::wstring& value) {
    if (index + 1 >= arguments.size() ||
        arguments[index + 1] == L"--") {
        return false;
    }
    value = arguments[++index];
    return !value.empty();
}

}  // namespace

InjectorCliResult parse_injector_cli(
    const std::vector<std::wstring>& arguments) {
    InjectorCliResult result{};
    bool saw_game_directory = false;
    bool saw_role = false;
    bool saw_pipe = false;
    bool saw_run_id = false;
    bool saw_stream_id = false;
    bool saw_scenario = false;
    bool saw_mission = false;
    bool saw_separator = false;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--") {
            saw_separator = true;
            result.options.game_arguments.assign(
                arguments.begin() + static_cast<std::ptrdiff_t>(index + 1),
                arguments.end());
            break;
        }
        if (argument == L"--allow-writes") {
            if (result.options.allow_writes) {
                return fail<InjectorCliResult>(
                    L"--allow-writes was provided more than once");
            }
            result.options.allow_writes = true;
        } else if (argument == L"--game-dir") {
            if (saw_game_directory ||
                !take_value(
                    arguments, index, result.options.game_directory)) {
                return fail<InjectorCliResult>(
                    L"--game-dir requires one non-empty value");
            }
            saw_game_directory = true;
        } else if (argument == L"--pipe") {
            if (saw_pipe ||
                !take_value(arguments, index, result.options.pipe_name)) {
                return fail<InjectorCliResult>(
                    L"--pipe requires one non-empty value");
            }
            saw_pipe = true;
        } else if (argument == L"--run-id") {
            if (saw_run_id ||
                !take_value(arguments, index, result.options.run_id)) {
                return fail<InjectorCliResult>(
                    L"--run-id requires one non-empty value");
            }
            saw_run_id = true;
        } else if (argument == L"--stream-id") {
            if (saw_stream_id ||
                !take_value(arguments, index, result.options.stream_id)) {
                return fail<InjectorCliResult>(
                    L"--stream-id requires one non-empty value");
            }
            saw_stream_id = true;
        } else if (argument == L"--scenario") {
            if (saw_scenario ||
                !take_value(arguments, index, result.options.scenario)) {
                return fail<InjectorCliResult>(
                    L"--scenario requires one non-empty value");
            }
            saw_scenario = true;
        } else if (argument == L"--mission") {
            if (saw_mission ||
                !take_value(arguments, index, result.options.mission)) {
                return fail<InjectorCliResult>(
                    L"--mission requires one non-empty value");
            }
            saw_mission = true;
        } else if (argument == L"--role") {
            std::wstring value;
            if (saw_role || !take_value(arguments, index, value)) {
                return fail<InjectorCliResult>(
                    L"--role requires retail-host or retail-client");
            }
            if (value == L"retail-host") {
                result.options.role = RetailRole::host;
            } else if (value == L"retail-client") {
                result.options.role = RetailRole::client;
            } else {
                return fail<InjectorCliResult>(
                    L"--role requires retail-host or retail-client");
            }
            saw_role = true;
        } else {
            return fail<InjectorCliResult>(
                L"unknown injector argument: " + argument);
        }
    }

    if (!saw_game_directory || !saw_role || !saw_pipe ||
        !saw_run_id || !saw_stream_id || !saw_scenario || !saw_mission ||
        !saw_separator) {
        return fail<InjectorCliResult>(
            L"--game-dir, --role, --pipe, --run-id, --stream-id, "
            L"--scenario, --mission, and -- are required");
    }
    const std::vector<std::wstring> expected{
        L"/w", L"/exp", L"jox01", L"/MANY"};
    if (result.options.game_arguments != expected) {
        return fail<InjectorCliResult>(
            L"game arguments must be exactly: /w /exp jox01 /MANY");
    }
    return result;
}

InspectorCliResult parse_inspector_cli(
    const std::vector<std::wstring>& arguments) {
    InspectorCliResult result{};
    bool saw_pipe = false;
    bool saw_trace = false;
    bool saw_scenario = false;
    bool saw_role_set = false;

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring& argument = arguments[index];
        if (argument == L"--pipe") {
            if (saw_pipe ||
                !take_value(arguments, index, result.options.pipe_name)) {
                return fail<InspectorCliResult>(
                    L"--pipe requires one non-empty value");
            }
            saw_pipe = true;
        } else if (argument == L"--trace") {
            if (saw_trace ||
                !take_value(arguments, index, result.options.trace_path)) {
                return fail<InspectorCliResult>(
                    L"--trace requires one non-empty value");
            }
            saw_trace = true;
        } else if (argument == L"--scenario") {
            if (saw_scenario ||
                !take_value(arguments, index, result.options.scenario)) {
                return fail<InspectorCliResult>(
                    L"--scenario requires one non-empty value");
            }
            saw_scenario = true;
        } else if (argument == L"--role-set") {
            std::wstring value;
            if (saw_role_set || !take_value(arguments, index, value)) {
                return fail<InspectorCliResult>(
                    L"--role-set requires baseline or candidate");
            }
            if (value == L"baseline") {
                result.options.role_set = ParityRoleSet::baseline;
            } else if (value == L"candidate") {
                result.options.role_set = ParityRoleSet::candidate;
            } else {
                return fail<InspectorCliResult>(
                    L"--role-set requires baseline or candidate");
            }
            saw_role_set = true;
        } else {
            return fail<InspectorCliResult>(
                L"unknown inspector argument: " + argument);
        }
    }

    if (!saw_pipe || !saw_trace || !saw_scenario || !saw_role_set) {
        return fail<InspectorCliResult>(
            L"--pipe, --trace, --scenario, and --role-set are required");
    }
    return result;
}

}  // namespace opennova::retail_hook::windows
