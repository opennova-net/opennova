// CBIN encoding/decoding implementation.
// Uses ROL32 + XOR cipher to encode/decode obfuscated text data.
// [orig: the retail CBIN writer @ 0x75e250 — magic 0x4E494243 @ 0x75e311, the
//  20-byte header + fwrite, and the per-4-byte cipher loop @ 0x75e348
//  (rol key,7; byte ^= key&0xFF). Witnessed read-only; see docs/credits/cbin-re.md
//  (PAR-R5, D-CBIN), MATCHING.]
#include <formats/cbin/cbin.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>

namespace opennova::cbin {

namespace {

// Rotate left 32-bit
inline uint32_t rol32(uint32_t value, unsigned int count) {
    count &= 31;
    return (value << count) | (value >> (32 - count));
}

uint32_t read_le_u32(const uint8_t* data) {
    return io::read_u32_le(data);
}

// Encode a buffer using ROL32 + XOR cipher [orig: cipher loop @ 0x75e348 —
// `mov ebx,[key]; rol ebx,7; mov [key],ebx; mov al,[key]; xor [blob],al`].
void encode_buffer(std::vector<uint8_t>& data, uint32_t key) {
    for (size_t i = 0; i < data.size(); i++) {
        key = rol32(key, 7);
        data[i] ^= (key & 0xFF);
    }
}

// Decode a buffer using ROL32 + XOR cipher (same as encode - XOR is symmetric)
void decode_buffer(std::vector<uint8_t>& data, uint32_t key) {
    encode_buffer(data, key);  // XOR is its own inverse
}

// Parse a hex color from ~Crrggbb format
uint32_t parse_color(const std::string& s) {
    if (s.length() >= 8 && s[0] == '~' && s[1] == 'C') {
        if (auto rgb = strutil::parse_ulong(s.substr(2, 6), 16))
            return static_cast<uint32_t>(*rgb);
    }
    return 0xFFFFFF;
}

// Format a color as ~Crrggbb
std::string format_color(uint32_t rgb) {
    char buf[16];
    snprintf(buf, sizeof(buf), "~C%06X", rgb & 0xFFFFFF);
    return buf;
}

}  // namespace

// Entry helper constructors
Entry Entry::make_text(const std::string& text, const std::string& font) {
    Entry e;
    e.type = EntryType::Text;
    e.text = text;
    e.font = font;
    return e;
}

Entry Entry::make_color(uint32_t rgb) {
    Entry e;
    e.type = EntryType::Color;
    e.color = rgb;
    return e;
}

Entry Entry::make_newline() {
    Entry e;
    e.type = EntryType::Newline;
    return e;
}

Entry Entry::make_image(const std::string& path, int display_x, int display_y) {
    Entry e;
    e.type = EntryType::Image;
    e.image_path = path;
    e.image_display_x = display_x;
    e.image_display_y = display_y;
    return e;
}

Entry Entry::make_justify(Justify j) {
    Entry e;
    e.type = EntryType::Justify;
    e.justify = j;
    return e;
}

void Credits::set_env(const std::string& key, const std::string& value) {
    if (key == "scroll_rate") {
        if (auto v = strutil::parse_float(value)) scroll_rate = *v;
    } else if (key == "vertical_space") {
        if (auto v = strutil::parse_int(value)) vertical_space = *v;
    } else if (key == "center_x") {
        if (auto v = strutil::parse_int(value)) center_x = *v;
    } else if (key == "top_y") {
        if (auto v = strutil::parse_int(value)) {
            top_y = *v;
            has_top_y = true;
        }
    } else if (key == "bottom_y") {
        if (auto v = strutil::parse_int(value)) {
            bottom_y = *v;
            has_bottom_y = true;
        }
    } else {
        env_extra[key] = value;
    }
}

bool Credits::has_bhd_bounds() const {
    return has_top_y || has_bottom_y;
}

bool is_cbin(const uint8_t* data, size_t size) {
    if (data == nullptr) {
        return false;
    }
    if (size < sizeof(Header)) {
        return false;
    }
    uint32_t magic = read_le_u32(data);
    return magic == kMagic;
}


bool decode_credits(const uint8_t* data, size_t size, Credits& out, std::string& error) {
    out = Credits{};

    if (data == nullptr) {
        error = "No CBIN data";
        return false;
    }

    if (size < sizeof(Header)) {
        error = "File too small for CBIN header";
        return false;
    }

    if (!is_cbin(data, size)) {
        error = "Not a CBIN file (invalid magic)";
        return false;
    }

    // Read header fields
    uint32_t string_offset = read_le_u32(data + 0x04);
    uint32_t blob_length = read_le_u32(data + 0x08);
    uint32_t string_count = read_le_u32(data + 0x0C);
    uint32_t xor_key = read_le_u32(data + 0x10);

    // Store XOR key for byte-for-byte roundtrip
    out.xor_key = xor_key;

    // Validate
    constexpr size_t kHeaderSize = 20;
    if (string_offset < kHeaderSize) {
        error = "Invalid CBIN string offset";
        return false;
    }
    if (string_offset > size) {
        error = "CBIN string offset is beyond file size";
        return false;
    }
    if (blob_length > size - string_offset) {
        error = "Invalid CBIN string blob length";
        return false;
    }

    // Decode the encoded region (starts at offset 0x14)
    size_t encoded_length = string_offset + blob_length - kHeaderSize;

    std::vector<uint8_t> decoded(data + kHeaderSize, data + kHeaderSize + encoded_length);
    decode_buffer(decoded, xor_key);

    // String table starts at (string_offset - kHeaderSize) within decoded buffer
    size_t string_table_offset = string_offset - kHeaderSize;
    if (string_table_offset > decoded.size()) {
        error = "CBIN string table offset is beyond decoded payload";
        return false;
    }
    if (decoded.size() < sizeof(uint32_t)) {
        error = "CBIN payload too small for label count";
        return false;
    }

    // Build string table
    std::vector<std::string> strings;
    size_t pos = string_table_offset;
    while (strings.size() < string_count) {
        if (pos >= decoded.size()) {
            error = "CBIN string table is truncated";
            return false;
        }
        size_t end = pos;
        while (end < decoded.size() && decoded[end] != '\0') end++;
        if (end >= decoded.size()) {
            error = "CBIN string table is missing a terminator";
            return false;
        }
        if (end > pos) {
            strings.push_back(std::string(reinterpret_cast<char*>(decoded.data() + pos), end - pos));
        } else {
            strings.push_back("");
        }
        pos = end + 1;
    }

    // Store original string table for byte-for-byte roundtrip
    out.original_strings = strings;

    // Parse entry table structure
    uint32_t label_count = read_le_u32(decoded.data());

    struct LabelInfo {
        std::string name;
        uint32_t element_count;
    };

    std::vector<LabelInfo> labels;
    size_t offset = 4;

    // Read label headers
    size_t total_elements = 0;
    for (uint32_t i = 0; i < label_count; i++) {
        if (offset + 8 > string_table_offset) {
            error = "CBIN label table is truncated";
            return false;
        }
        uint32_t str_idx = read_le_u32(decoded.data() + offset);
        uint32_t elem_count = read_le_u32(decoded.data() + offset + 4);
        offset += 8;

        LabelInfo label;
        if (str_idx > 0 && str_idx <= strings.size()) {
            label.name = strings[str_idx - 1];
        } else {
            error = "CBIN label string index is invalid";
            return false;
        }
        label.element_count = elem_count;
        labels.push_back(label);
        if (elem_count == std::numeric_limits<uint32_t>::max() ||
            total_elements > std::numeric_limits<size_t>::max() - static_cast<size_t>(elem_count) - 1) {
            error = "CBIN element count is too large";
            return false;
        }
        total_elements += static_cast<size_t>(elem_count) + 1;
    }

    // Read element name entries (with type field)
    struct NameEntry {
        uint32_t str_idx;
        uint32_t type;  // 0=terminator, 1=single value, 2=has extra value
    };
    std::vector<NameEntry> name_entries;
    for (size_t i = 0; i < total_elements; i++) {
        if (offset + 8 > string_table_offset) {
            error = "CBIN name table is truncated";
            return false;
        }
        NameEntry entry;
        entry.str_idx = read_le_u32(decoded.data() + offset);
        entry.type = read_le_u32(decoded.data() + offset + 4);
        offset += 8;
        if (entry.type > 2) {
            error = "CBIN name entry type is invalid";
            return false;
        }
        if (entry.type == 0) {
            if (entry.str_idx != 0) {
                error = "CBIN terminator entry is invalid";
                return false;
            }
        } else if (entry.str_idx == 0 || entry.str_idx > strings.size()) {
            error = "CBIN name string index is invalid";
            return false;
        }
        name_entries.push_back(entry);
    }

    // Read ALL value entries until string table
    std::vector<std::pair<uint32_t, uint32_t>> value_entries;
    while (offset + 8 <= string_table_offset) {
        uint32_t value = read_le_u32(decoded.data() + offset);
        uint32_t flags = read_le_u32(decoded.data() + offset + 4);
        offset += 8;
        value_entries.push_back({value, flags});
    }
    if (offset != string_table_offset) {
        error = "CBIN value table is truncated";
        return false;
    }

    // Helper to get string value from entry
    auto get_value_string = [&](size_t idx, std::string& out_value) -> bool {
        out_value.clear();
        if (idx >= value_entries.size()) {
            error = "CBIN value entry is missing";
            return false;
        }
        auto [val_raw, flags] = value_entries[idx];
        if (val_raw == 0 && flags == 0) return true;
        if ((flags & 4) && val_raw > 0 && val_raw <= strings.size()) {
            out_value = strings[val_raw - 1];
            return true;
        } else if (flags & 4) {
            error = "CBIN value string index is invalid";
            return false;
        } else if (flags & 2) {
            float fval;
            std::memcpy(&fval, &val_raw, sizeof(fval));
            out_value = std::to_string(fval);
            return true;
        } else {
            out_value = std::to_string(val_raw);
            return true;
        }
    };

    // Process labels directly into Credits structure
    size_t name_idx = 0;
    size_t value_idx = 0;

    for (auto& label : labels) {
        if (label.name == "env") {
            // ENV: process environment settings
            for (uint32_t j = 0; j <= label.element_count && name_idx < name_entries.size(); j++, name_idx++) {
                auto& entry = name_entries[name_idx];
                if (entry.type == 0) continue;

                std::string name;
                if (entry.str_idx > 0 && entry.str_idx <= strings.size()) {
                    name = strings[entry.str_idx - 1];
                }
                std::string value;
                if (!get_value_string(value_idx++, value)) {
                    return false;
                }
                if (!name.empty()) {
                    out.set_env(name, value);
                }
            }
        } else if (label.name == "text") {
            // TEXT: create Entry objects from values
            // type=1: ONE value (control code or text without font)
            // type=2: TWO CONSECUTIVE values (text + font)

            for (uint32_t j = 0; j <= label.element_count && name_idx < name_entries.size(); j++, name_idx++) {
                auto& entry = name_entries[name_idx];
                if (entry.type == 0) continue;

                // Read main value
                std::string main_value;
                if (!get_value_string(value_idx++, main_value)) {
                    return false;
                }
                if (main_value.empty()) continue;

                // For type=2, also read the font value (stored consecutively)
                std::string font_value;
                if (entry.type == 2 && value_idx < value_entries.size()) {
                    if (!get_value_string(value_idx++, font_value)) {
                        return false;
                    }
                } else if (entry.type == 2) {
                    error = "CBIN text font value is missing";
                    return false;
                }

                // Create Entry based on main value content
                Entry e;
                e.binary_type = static_cast<int>(entry.type);

                // Helper to check control code prefix (case-insensitive)
                auto starts_with_tilde = [&](char c) {
                    return main_value.length() >= 2 && main_value[0] == '~' &&
                           (main_value[1] == c || main_value[1] == (c + 32));  // uppercase or lowercase
                };

                if (main_value == tokens::kNewline) {
                    e.type = EntryType::Newline;
                } else if (starts_with_tilde('C')) {
                    // ~C or ~c - Color code
                    e.type = EntryType::Color;
                    e.color = parse_color(main_value);
                } else if (starts_with_tilde('J')) {
                    // ~J or ~j - Justify (L/l=left, R/r=right, C/c or default=center)
                    e.type = EntryType::Justify;
                    if (main_value.length() >= 3) {
                        char align = main_value[2];
                        if (align == 'L' || align == 'l') {
                            e.justify = Justify::Left;
                        } else if (align == 'R' || align == 'r') {
                            e.justify = Justify::Right;
                        } else {
                            e.justify = Justify::Center;
                        }
                    } else {
                        e.justify = Justify::Center;
                    }
                } else if (starts_with_tilde('F')) {
                    // ~F or ~f - Image with x_offset|y_offset|path format
                    // Format: ~F{x}|{y}|{path}
                    e.type = EntryType::Image;
                    size_t first_pipe = main_value.find('|');
                    size_t second_pipe = main_value.find('|', first_pipe + 1);
                    if (first_pipe != std::string::npos && second_pipe != std::string::npos) {
                        // First number after ~F is X offset
                        if (auto x = strutil::parse_int(main_value.substr(2, first_pipe - 2)))
                            e.image_display_x = *x;
                        // Second number is Y offset
                        if (auto y = strutil::parse_int(main_value.substr(first_pipe + 1, second_pipe - first_pipe - 1)))
                            e.image_display_y = *y;
                        e.image_path = main_value.substr(second_pipe + 1);
                    }
                } else if (starts_with_tilde('I')) {
                    // ~I or ~i - Simple image with just path (scrolls with content)
                    e.type = EntryType::Image;
                    e.image_path = main_value.substr(2);  // Skip "~I"
                    e.image_display_x = 0;
                    e.image_display_y = 0;
                    e.use_simple_image_format = true;
                } else {
                    // Plain text - font_value is the font name
                    e.type = EntryType::Text;
                    e.text = main_value;
                    e.font = font_value;
                }

                out.entries.push_back(e);
            }
        } else {
            // Other labels: skip (advance value_idx based on type)
            for (uint32_t j = 0; j <= label.element_count && name_idx < name_entries.size(); j++, name_idx++) {
                auto& entry = name_entries[name_idx];
                if (entry.type == 1) {
                    if (value_idx >= value_entries.size()) {
                        error = "CBIN value entry is missing";
                        return false;
                    }
                    value_idx++;
                } else if (entry.type == 2) {
                    if (value_idx + 1 >= value_entries.size()) {
                        error = "CBIN value entry is missing";
                        return false;
                    }
                    value_idx += 2;  // Two consecutive values for type=2
                }
            }
        }
    }

    return true;
}

// Helper to write a uint32_t to a buffer
inline void write_u32(std::vector<uint8_t>& buf, uint32_t val) {
    io::append_u32_le(buf, val);
}

bool encode(const Credits& credits, std::vector<uint8_t>& out, std::string& error) {
    out.clear();

    // Build string table (index 0 is reserved, strings are 1-based)
    // If original_strings is available, reuse it for byte-for-byte parity
    std::vector<std::string> strings;
    std::map<std::string, uint32_t> string_map;

    // If we have original strings, use them and build the map from that
    if (!credits.original_strings.empty()) {
        strings = credits.original_strings;
        for (size_t i = 0; i < strings.size(); i++) {
            string_map[strings[i]] = static_cast<uint32_t>(i + 1);  // 1-based
        }
    }

    auto add_string = [&](const std::string& s) -> uint32_t {
        if (s.empty()) return 0;
        auto it = string_map.find(s);
        if (it != string_map.end()) {
            return it->second;
        }
        // Only add new strings if we don't have original_strings
        strings.push_back(s);
        uint32_t idx = static_cast<uint32_t>(strings.size());
        string_map[s] = idx;
        return idx;
    };

    // Pre-add section names
    uint32_t env_idx = add_string("env");
    uint32_t text_idx = add_string("text");

    // Build ENV elements
    struct Element {
        uint32_t name_idx;
        uint32_t type;  // 0=terminator, 1=single value, 2=has extra value
        uint32_t value_raw;
        uint32_t flags;
        // For type=2 entries, extra value
        uint32_t extra_value_raw = 0;
        uint32_t extra_flags = 0;
    };

    std::vector<Element> env_elements;

    auto add_env_element = [&](const std::string& name, float fval) {
        Element e;
        e.name_idx = add_string(name);
        e.type = 1;  // type field (same as int)
        std::memcpy(&e.value_raw, &fval, sizeof(float));
        e.flags = 2;  // flags bit 2 indicates float
        env_elements.push_back(e);
    };

    auto add_env_element_int = [&](const std::string& name, int ival) {
        Element e;
        e.name_idx = add_string(name);
        e.type = 1;  // type field
        e.value_raw = static_cast<uint32_t>(ival);
        e.flags = 1;  // Original uses flags=1 for integers
        env_elements.push_back(e);
    };

    add_env_element("scroll_rate", credits.scroll_rate);
    add_env_element_int("vertical_space", credits.vertical_space);
    add_env_element_int("center_x", credits.center_x);
    if (credits.has_top_y) {
        add_env_element_int("top_y", credits.top_y);
    }
    if (credits.has_bottom_y) {
        add_env_element_int("bottom_y", credits.bottom_y);
    }

    for (const auto& [key, value] : credits.env_extra) {
        if (key == "scroll_rate" || key == "vertical_space" || key == "center_x" ||
            key == "top_y" || key == "bottom_y") {
            continue;
        }
        Element e;
        e.name_idx = add_string(key);
        e.type = 1;  // Use type=1 for string env values
        e.value_raw = add_string(value);
        e.flags = 4;  // value is string index
        env_elements.push_back(e);
    }

    // Build TEXT elements
    // binary_type=1: single value (control codes)
    // binary_type=2: two consecutive values (text + font)
    std::vector<Element> text_elements;
    uint32_t text_name_idx = add_string("text");

    for (const auto& entry : credits.entries) {
        Element e;
        e.name_idx = text_name_idx;
        e.flags = 4;  // String value flag
        e.type = entry.binary_type;  // Preserve original binary type

        // For text entries with font, use type=2
        if (entry.type == EntryType::Text && !entry.font.empty()) {
            e.type = 2;
        }

        switch (entry.type) {
            case EntryType::Text:
                e.value_raw = add_string(entry.text);
                if (e.type == 2) {
                    e.extra_value_raw = add_string(entry.font);
                    e.extra_flags = 4;
                }
                text_elements.push_back(e);
                break;
            case EntryType::Color:
                e.value_raw = add_string(format_color(entry.color));
                text_elements.push_back(e);
                break;
            case EntryType::Newline:
                e.value_raw = add_string("<CR>");
                text_elements.push_back(e);
                break;
            case EntryType::Image: {
                std::string img;
                if (entry.use_simple_image_format) {
                    // ~Ipath format (scrolls with content)
                    img = "~I" + entry.image_path;
                } else {
                    // ~F{x}|{y}|path format (fixed overlay)
                    img = "~F" + std::to_string(entry.image_display_x) + "|" +
                          std::to_string(entry.image_display_y) + "|" + entry.image_path;
                }
                e.value_raw = add_string(img);
                text_elements.push_back(e);
                break;
            }
            case EntryType::Justify:
                switch (entry.justify) {
                    case Justify::Left: e.value_raw = add_string("~JL"); break;
                    case Justify::Center: e.value_raw = add_string("~JC"); break;
                    case Justify::Right: e.value_raw = add_string("~JR"); break;
                }
                text_elements.push_back(e);
                break;
        }
    }

    // Build entry table
    std::vector<uint8_t> entry_table;

    // Label count (2: env and text)
    write_u32(entry_table, 2);

    // Label entries - elem_count is number of name entries (not counting terminator)
    write_u32(entry_table, env_idx);
    write_u32(entry_table, static_cast<uint32_t>(env_elements.size()));
    write_u32(entry_table, text_idx);
    write_u32(entry_table, static_cast<uint32_t>(text_elements.size()));

    // Name entries for ENV
    for (const auto& e : env_elements) {
        write_u32(entry_table, e.name_idx);
        write_u32(entry_table, e.type);
    }
    // ENV terminator (str_idx=0, type=0)
    write_u32(entry_table, 0);
    write_u32(entry_table, 0);

    // Name entries for TEXT
    for (const auto& e : text_elements) {
        write_u32(entry_table, e.name_idx);
        write_u32(entry_table, e.type);
    }
    // TEXT terminator (str_idx=0, type=0)
    write_u32(entry_table, 0);
    write_u32(entry_table, 0);

    // Value entries
    // ENV values (one per element)
    for (const auto& e : env_elements) {
        write_u32(entry_table, e.value_raw);
        write_u32(entry_table, e.flags);
    }

    // TEXT values - for type=2, write main and font CONSECUTIVELY
    for (const auto& e : text_elements) {
        write_u32(entry_table, e.value_raw);
        write_u32(entry_table, e.flags);
        // For type=2, immediately write the font value
        if (e.type == 2) {
            write_u32(entry_table, e.extra_value_raw);
            write_u32(entry_table, e.extra_flags);
        }
    }

    // Build string blob
    std::vector<uint8_t> string_blob;
    for (const auto& s : strings) {
        for (char c : s) {
            string_blob.push_back(static_cast<uint8_t>(c));
        }
        string_blob.push_back(0);  // null terminator
    }

    // Calculate offsets
    constexpr size_t kHeaderSize = 20;
    size_t string_offset = kHeaderSize + entry_table.size();
    size_t blob_length = string_blob.size();

    // Use preserved XOR key for byte-for-byte roundtrip, or generate random
    uint32_t xor_key;
    if (credits.xor_key != 0) {
        xor_key = credits.xor_key;
    } else {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<uint32_t> dist(1, 0xFFFFFFFF);
        xor_key = dist(gen);
    }

    // Combine entry table and string blob for encoding
    std::vector<uint8_t> encoded_data;
    encoded_data.insert(encoded_data.end(), entry_table.begin(), entry_table.end());
    encoded_data.insert(encoded_data.end(), string_blob.begin(), string_blob.end());

    // Encode with XOR cipher
    encode_buffer(encoded_data, xor_key);

    // Build final output
    // Header
    write_u32(out, kMagic);
    write_u32(out, static_cast<uint32_t>(string_offset));
    write_u32(out, static_cast<uint32_t>(blob_length));
    write_u32(out, static_cast<uint32_t>(strings.size()));
    write_u32(out, xor_key);

    // Encoded data
    out.insert(out.end(), encoded_data.begin(), encoded_data.end());

    return true;
}

// Display-view collapse/re-emit — the control-code state machine formerly
// duplicated in the Godot .kda loader/saver (moved here 2026-08-08, ADR 0030).
// No [orig] witnesses; the retail read path is tracked as cbin-re.md PAR-R5.

std::vector<CreditsDisplayItem> credits_display_items(const Credits& credits) {
    std::vector<CreditsDisplayItem> items;
    items.reserve(credits.entries.size());
    uint32_t color = kDefaultDisplayColor;
    Justify justify = Justify::Center;
    for (const Entry& src : credits.entries) {
        switch (src.type) {
            case EntryType::Color:
                color = src.color;
                break;
            case EntryType::Justify:
                justify = src.justify;
                break;
            case EntryType::Text: {
                CreditsDisplayItem item;
                item.type = EntryType::Text;
                item.text = src.text;
                item.font = src.font;
                item.color = color;
                item.justify = justify;
                items.push_back(std::move(item));
                break;
            }
            case EntryType::Newline: {
                CreditsDisplayItem item;
                item.type = EntryType::Newline;
                items.push_back(std::move(item));
                break;
            }
            case EntryType::Image: {
                CreditsDisplayItem item;
                item.type = EntryType::Image;
                item.image_path = src.image_path;
                item.image_display_x = src.image_display_x;
                item.image_display_y = src.image_display_y;
                item.use_simple_image_format = src.use_simple_image_format;
                items.push_back(std::move(item));
                break;
            }
        }
    }
    return items;
}

std::vector<Entry> credits_entries_from_display(
        const std::vector<CreditsDisplayItem>& items) {
    std::vector<Entry> entries;
    entries.reserve(items.size() * 2);
    uint32_t color = kDefaultDisplayColor;
    Justify justify = Justify::Center;
    for (const CreditsDisplayItem& item : items) {
        switch (item.type) {
            case EntryType::Text: {
                if (item.color != color) {
                    entries.push_back(Entry::make_color(item.color));
                    color = item.color;
                }
                if (item.justify != justify) {
                    entries.push_back(Entry::make_justify(item.justify));
                    justify = item.justify;
                }
                entries.push_back(Entry::make_text(item.text, item.font));
                break;
            }
            case EntryType::Newline:
                entries.push_back(Entry::make_newline());
                break;
            case EntryType::Image: {
                Entry image = Entry::make_image(
                    item.image_path, item.image_display_x, item.image_display_y);
                image.use_simple_image_format = item.use_simple_image_format;
                entries.push_back(std::move(image));
                break;
            }
            default:
                break;  // control types are not display items
        }
    }
    return entries;
}

}  // namespace opennova::cbin