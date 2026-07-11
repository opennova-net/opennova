#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace opennova::retail_hook::windows {

inline constexpr std::uint32_t kHookStartConfigVersion = 1;
inline constexpr std::size_t kMaximumPipeNameCharacters = 240;
inline constexpr std::size_t kMaximumMetadataCharacters = 128;

enum class HookRole : std::uint32_t {
    retail_host = 1,
    retail_client = 2,
};

// Copied synchronously by OpenNovaRetailHook_Start before it launches the
// worker, so the injector may release its remote argument after Start returns.
struct HookStartConfig {
    std::uint32_t structure_size{};
    std::uint32_t version{kHookStartConfigVersion};
    std::uint32_t allow_writes{};
    HookRole role{HookRole::retail_host};
    wchar_t pipe_name[kMaximumPipeNameCharacters]{};
    wchar_t run_id[kMaximumMetadataCharacters]{};
    wchar_t stream_id[kMaximumMetadataCharacters]{};
    wchar_t scenario[kMaximumMetadataCharacters]{};
    wchar_t mission[kMaximumMetadataCharacters]{};
};

static_assert(std::is_standard_layout_v<HookStartConfig>);
static_assert(std::is_trivially_copyable_v<HookStartConfig>);

}  // namespace opennova::retail_hook::windows
