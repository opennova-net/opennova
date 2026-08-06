// CBIN encoding/decoding library.
// CBIN is a NovaLogic obfuscated text format used for credits (.kda files).
// Format: "CBIN" magic + XOR-obfuscated payload decoded with ROL32 cipher.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace cbin {

// Magic number "CBIN" in little-endian
constexpr uint32_t kMagic = 0x4E494243;  // 'CBIN'

// Header layout (20 bytes)
struct Header {
    uint32_t magic;           // 0x00: 'CBIN' (0x4E494243)
    uint32_t string_offset;   // 0x04: Offset to encoded string blob
    uint32_t blob_length;     // 0x08: Length of encoded string blob
    uint32_t string_count;    // 0x0C: Number of strings in table
    uint32_t xor_key;         // 0x10: Initial XOR key for deobfuscation
};

static_assert(sizeof(Header) == 20, "Header must be 20 bytes");

// Structured entry types for the visual editor
enum class EntryType {
    Text,      // Text with font
    Color,     // Color change (~Crrggbb)
    Newline,   // Line break (<CR>)
    Image,     // Inline image (~F0|index|filename)
    Justify,   // Text alignment (~JL, ~JC, ~JR)
};

// Justify alignment
enum class Justify { Left = -1, Center = 0, Right = 1 };

// A structured credits entry for the visual editor
struct Entry {
    EntryType type = EntryType::Text;

    // For Text entries
    std::string text;
    std::string font;  // Font name (from type=2 secondary value)

    // For Color entries (RGB, e.g., 0xFF0000 = red)
    uint32_t color = 0xFFFFFF;

    // For Image entries
    std::string image_path;
    int image_display_x = 0;  // ~F format: X offset from viewport left (first number)
    int image_display_y = 0;  // ~F format: Y offset from viewport top (second number)
    bool use_simple_image_format = false;  // true for ~Ipath, false for ~Fx|y|path

    // For Justify entries
    Justify justify = Justify::Center;

    // Binary format: type field from name entry (1=single value, 2=text+font pair)
    // Used by encoder to recreate original binary structure
    int binary_type = 1;

    // Helper constructors
    static Entry make_text(const std::string& text, const std::string& font = "");
    static Entry make_color(uint32_t rgb);
    static Entry make_newline();
    static Entry make_image(const std::string& path, int display_x = 0, int display_y = 0);
    static Entry make_justify(Justify j);
};

// Parsed CBIN content with structured entries
struct Credits {
    // [ENV] section - rendering settings
    float scroll_rate = 0.5f;
    int vertical_space = 14;
    int center_x = 400;
    bool has_top_y = false;
    int top_y = 0;
    bool has_bottom_y = false;
    int bottom_y = 0;
    std::map<std::string, std::string> env_extra;  // Any other ENV values

    // [TEXT] section - structured entries
    std::vector<Entry> entries;

    // Original string table (preserved for byte-for-byte roundtrip)
    // If non-empty, encoder will reuse this string table order
    std::vector<std::string> original_strings;

    // Original XOR key (preserved for byte-for-byte roundtrip)
    // If 0, a random key will be generated on encode
    uint32_t xor_key = 0;

    // Get/set ENV value (handles type conversion)
    std::string get_env(const std::string& key) const;
    void set_env(const std::string& key, const std::string& value);
    bool has_bhd_bounds() const;
};

// Legacy raw text entry (for backwards compatibility)
struct TextEntry {
    std::string content;
    std::string font;
};

// Special tokens in TEXT content
namespace tokens {
    constexpr const char* kNewline = "<CR>";
    constexpr const char* kColorPrefix = "~C";
    constexpr const char* kJustifyRight = "~JR";
    constexpr const char* kJustifyLeft = "~JL";
    constexpr const char* kJustifyCenter = "~JC";
    constexpr const char* kImagePrefix = "~F0|";
}

// Check if data starts with CBIN magic.
bool is_cbin(const uint8_t* data, size_t size);

// Check if file is CBIN format.
bool is_cbin_file(const std::string& path);

// Decode CBIN data and parse into structured Credits object.
bool decode_credits(const uint8_t* data, size_t size, Credits& out, std::string& error);

// Decode CBIN file to Credits structure.
bool decode_file_credits(const std::string& path, Credits& out, std::string& error);

// Encode Credits structure to CBIN binary format.
bool encode(const Credits& credits, std::vector<uint8_t>& out, std::string& error);

// Encode Credits to file.
bool encode_file(const Credits& credits, const std::string& path, std::string& error);

}  // namespace cbin
