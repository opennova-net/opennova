// BMS mission file parser implementation.
#include "mission/bms.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <type_traits>
#include <utility>

namespace opennova::bms {

namespace {

std::string fixed_string(const char *data, size_t max_len) {
    size_t len = 0;
    while (len < max_len && data[len] != '\0') {
        ++len;
    }
    return std::string(data, len);
}

constexpr uint32_t kKnownBmsiAttributeMask =
    static_cast<uint32_t>(BmsiAttributeFlags::Blind) |
    static_cast<uint32_t>(BmsiAttributeFlags::Guarding) |
    static_cast<uint32_t>(BmsiAttributeFlags::RemoveIfLessThan) |
    static_cast<uint32_t>(BmsiAttributeFlags::RemoveIfMoreThan) |
    static_cast<uint32_t>(BmsiAttributeFlags::Multiplayer) |
    static_cast<uint32_t>(BmsiAttributeFlags::Berserk) |
    static_cast<uint32_t>(BmsiAttributeFlags::FlyingOrganic) |
    static_cast<uint32_t>(BmsiAttributeFlags::Coward) |
    static_cast<uint32_t>(BmsiAttributeFlags::Attribute17) |
    static_cast<uint32_t>(BmsiAttributeFlags::AdvancedAmmo) |
    static_cast<uint32_t>(BmsiAttributeFlags::Indestructible) |
    static_cast<uint32_t>(BmsiAttributeFlags::NavigationWaypoint) |
    static_cast<uint32_t>(BmsiAttributeFlags::Reflective) |
    static_cast<uint32_t>(BmsiAttributeFlags::NoShadow);

// Helper class for reading binary data
class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size), pos_(0) {}

    // Asked against the bytes left, not as `pos_ + count <= size_`: that sum
    // overflows for a large stream-derived count and wraps to a value that
    // passes, which would let a malformed .bms read past the buffer. Every
    // mutator below keeps pos_ <= size_, so remaining() never underflows.
    bool has_bytes(size_t count) const { return count <= remaining(); }
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

    uint8_t read_u8() {
        if (!has_bytes(1)) return 0;
        return data_[pos_++];
    }

    int8_t read_i8() {
        return static_cast<int8_t>(read_u8());
    }

    uint16_t read_u16() {
        if (!has_bytes(2)) return 0;
        uint16_t v = data_[pos_] | (data_[pos_ + 1] << 8);
        pos_ += 2;
        return v;
    }

    int16_t read_i16() {
        return static_cast<int16_t>(read_u16());
    }

    uint32_t read_u32() {
        if (!has_bytes(4)) return 0;
        uint32_t v = data_[pos_] | (data_[pos_ + 1] << 8) |
                     (data_[pos_ + 2] << 16) | (data_[pos_ + 3] << 24);
        pos_ += 4;
        return v;
    }

    int32_t read_i32() {
        return static_cast<int32_t>(read_u32());
    }

    float read_f32() {
        uint32_t v = read_u32();
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }

    // Read fixed-point 16.16 as float
    float read_fixed16() {
        int32_t v = read_i32();
        return v / 65536.0f;
    }

    void read_bytes(uint8_t* out, size_t count) {
        if (!has_bytes(count)) {
            std::memset(out, 0, count);
            return;
        }
        std::memcpy(out, data_ + pos_, count);
        pos_ += count;
    }

    void read_bytes(std::vector<uint8_t>& out, size_t count) {
        out.resize(count);
        read_bytes(out.data(), count);
    }

    // Read fixed-size string field (preserves all bytes for roundtrip)
    void read_fixed_string(char* out, size_t max_len) {
        read_bytes(reinterpret_cast<uint8_t*>(out), max_len);
    }

    void skip(size_t count) {
        if (!has_bytes(count)) { // was `pos_ + count > size_` — same overflow
            pos_ = size_;
        } else {
            pos_ += count;
        }
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_;
};

// Helper class for writing binary data
class Writer {
public:
    void write_u8(uint8_t v) {
        data_.push_back(v);
    }

    void write_i8(int8_t v) {
        write_u8(static_cast<uint8_t>(v));
    }

    void write_u16(uint16_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
    }

    void write_i16(int16_t v) {
        write_u16(static_cast<uint16_t>(v));
    }

    void write_u32(uint32_t v) {
        data_.push_back(v & 0xFF);
        data_.push_back((v >> 8) & 0xFF);
        data_.push_back((v >> 16) & 0xFF);
        data_.push_back((v >> 24) & 0xFF);
    }

    void write_i32(int32_t v) {
        write_u32(static_cast<uint32_t>(v));
    }

    void write_f32(float f) {
        uint32_t v;
        std::memcpy(&v, &f, sizeof(v));
        write_u32(v);
    }

    // Write float as fixed-point 16.16
    void write_fixed16(float f) {
        int32_t v = static_cast<int32_t>(f * 65536.0f);
        write_i32(v);
    }

    void write_bytes(const uint8_t* src, size_t count) {
        data_.insert(data_.end(), src, src + count);
    }

    void write_bytes(const std::vector<uint8_t>& src) {
        data_.insert(data_.end(), src.begin(), src.end());
    }

    // Write fixed-size string field preserving all bytes (for roundtrip fidelity)
    void write_fixed_string(const char* src, size_t max_len) {
        data_.insert(data_.end(), src, src + max_len);
    }

    void write_zeros(size_t count) {
        data_.insert(data_.end(), count, 0);
    }

    std::vector<uint8_t>& data() { return data_; }
    size_t size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;
};

bool parse_header(Reader& r, Header& h, std::string& error) {
    size_t start = r.position();

    r.read_bytes(reinterpret_cast<uint8_t*>(h.magic), 4);
    if (h.magic[0] != 'B' || h.magic[1] != 'M' || h.magic[2] != 'S') {
        error = "Invalid BMS magic";
        return false;
    }
    // [orig: version gate `byte_A761D3 < 19` @0x40f5aa Mission_LoadBMSFile / @0x40e30a BMS_LoadAndValidateHeader]
    if (static_cast<uint8_t>(h.magic[3]) < kMinVersion) {
        error = "Unsupported BMS version " + std::to_string(static_cast<uint8_t>(h.magic[3])) +
                " (minimum " + std::to_string(kMinVersion) + ")";
        return false;
    }

    r.read_fixed_string(h.mission_name, 32);
    r.read_fixed_string(h.designer, 32);
    r.read_fixed_string(h.terrain, 48);
    r.read_fixed_string(h.default_str, 16);
    h.climate = static_cast<ClimateType>(r.read_u32());
    h.attrib_flags = static_cast<AttribFlags>(r.read_u32());
    r.read_bytes(h.unknown0, 12);
    h.water_override = r.read_u16();
    h.unknown1 = r.read_u32();
    h.fog_override = r.read_u16();
    r.read_bytes(h.fog_color, 3);
    h.unknown2 = r.read_u8();
    h.num_items = r.read_u32();
    h.num_buildings = r.read_u32();
    h.num_markers = r.read_u32();
    h.num_people = r.read_u32();
    h.num_events = r.read_u32();
    h.weather_type = static_cast<WeatherType>(r.read_u32());
    r.read_bytes(h.win_conditions, 8);
    r.read_bytes(h.lose_conditions, 8);
    r.read_bytes(h.unknown3, 16);
    r.read_fixed_string(h.environment, 16);
    r.read_bytes(h.unknown4, 10);
    r.read_bytes(h.water_color, 3);
    h.murk = r.read_u16();
    h.something1 = r.read_u8();
    h.wind_speed = r.read_u32();
    h.wind_direction = r.read_u32();
    r.read_bytes(h.unknown5, 4);
    h.health = r.read_u32();
    h.mana = r.read_u32();
    h.music = r.read_u32();
    h.reverb = r.read_u32();
    r.read_fixed_string(h.terrain_tile, 16);
    r.read_fixed_string(h.mission_briefing, 256);
    h.unknown6 = r.read_i16();
    h.mission_type = static_cast<MissionType>(r.read_u8());
    h.max_saves = r.read_u8();
    r.read_bytes(h.win_scores, 8);
    r.read_bytes(h.lose_scores, 8);
    h.map_zoom = r.read_f32();
    h.area_trigger_count = r.read_i16();
    h.weapon_loadout_chunk_len = r.read_u16();
    h.bonus_expiration = r.read_u16();
    h.secondary_chunk_len = r.read_u16();
    h.start_time = r.read_u16();
    h.minutes_per_day = r.read_u16();
    r.read_bytes(h.unknown9, 28);

    size_t bytes_read = r.position() - start;
    if (bytes_read != kHeaderSize) {
        error = "Header size mismatch: expected " + std::to_string(kHeaderSize) +
                " got " + std::to_string(bytes_read);
        return false;
    }

    return true;
}

void write_header(Writer& w, const Header& h) {
    w.write_bytes(reinterpret_cast<const uint8_t*>(h.magic), 4);
    w.write_fixed_string(h.mission_name, 32);
    w.write_fixed_string(h.designer, 32);
    w.write_fixed_string(h.terrain, 48);
    w.write_fixed_string(h.default_str, 16);
    w.write_u32(static_cast<uint32_t>(h.climate));
    w.write_u32(static_cast<uint32_t>(h.attrib_flags));
    w.write_bytes(h.unknown0, 12);
    w.write_u16(h.water_override);
    w.write_u32(h.unknown1);
    w.write_u16(h.fog_override);
    w.write_bytes(h.fog_color, 3);
    w.write_u8(h.unknown2);
    w.write_u32(h.num_items);
    w.write_u32(h.num_buildings);
    w.write_u32(h.num_markers);
    w.write_u32(h.num_people);
    w.write_u32(h.num_events);
    w.write_u32(static_cast<uint32_t>(h.weather_type));
    w.write_bytes(h.win_conditions, 8);
    w.write_bytes(h.lose_conditions, 8);
    w.write_bytes(h.unknown3, 16);
    w.write_fixed_string(h.environment, 16);
    w.write_bytes(h.unknown4, 10);
    w.write_bytes(h.water_color, 3);
    w.write_u16(h.murk);
    w.write_u8(h.something1);
    w.write_u32(h.wind_speed);
    w.write_u32(h.wind_direction);
    w.write_bytes(h.unknown5, 4);
    w.write_u32(h.health);
    w.write_u32(h.mana);
    w.write_u32(h.music);
    w.write_u32(h.reverb);
    w.write_fixed_string(h.terrain_tile, 16);
    w.write_fixed_string(h.mission_briefing, 256);
    w.write_i16(h.unknown6);
    w.write_u8(static_cast<uint8_t>(h.mission_type));
    w.write_u8(h.max_saves);
    w.write_bytes(h.win_scores, 8);
    w.write_bytes(h.lose_scores, 8);
    w.write_f32(h.map_zoom);
    w.write_i16(h.area_trigger_count);
    w.write_u16(h.weapon_loadout_chunk_len);
    w.write_u16(h.bonus_expiration);
    w.write_u16(h.secondary_chunk_len);
    w.write_u16(h.start_time);
    w.write_u16(h.minutes_per_day);
    w.write_bytes(h.unknown9, 28);
}

bool parse_entity(Reader& r, Entity& e, std::string& error) {
    size_t start = r.position();

    e.type_id = r.read_i32();
    e.name_index = r.read_i32();
    e.id = r.read_i32();
    e.bmsi_attributes = r.read_u32();
    if ((e.bmsi_attributes & ~kKnownBmsiAttributeMask) != 0) {
        error = "BMS entity has unsupported AI attribute bits";
        return false;
    }
    e.x = r.read_i32();
    e.y = r.read_i32();
    e.z = r.read_i32();
    e.wp_distance = r.read_i32();
    e.perception2 = r.read_i32();
    e.perfectionist2 = r.read_i32();
    e.min_engagement_distance = r.read_i32();
    e.max_engagement_distance = r.read_i32();
    e.wp_number = r.read_i32();
    e.w_accuracy2 = r.read_i16();
    e.w_accuracy1 = r.read_i16();
    e.yaw = r.read_i16();
    e.pitch = r.read_i16();
    e.roll = r.read_i16();
    e.spawns = r.read_i16();
    e.crouch_timer = r.read_u8();
    e.unk15a = r.read_u8();
    e.shoot_timer = r.read_i16();
    e.wp_adv_trigger = r.read_i16();
    e.attention = r.read_i16();
    e.alert_state = r.read_u8();
    e.team = r.read_u8();
    e.no_more_than = r.read_u8();
    e.no_less_than = r.read_u8();
    e.weapon_types = r.read_i16();
    e.group_id = r.read_u8();
    e.waypoint_id = r.read_u8();
    e.obliqueness = r.read_u8();
    e.map_symbol = r.read_u8();
    e.unk22 = r.read_i16();
    e.blink_parent = r.read_i16();
    e.blink_group = r.read_i16();
    e.group_rel_lo = r.read_i16();
    e.group_rel_hi = r.read_i16();
    e.advancetimer = r.read_i32();
    e.ttool_index = r.read_i32();
    e.wp_goals = r.read_i32();
    r.read_fixed_string(e.name1, 8);
    r.read_fixed_string(e.name2, 8);
    r.read_fixed_string(e.gen_string, sizeof(e.gen_string));
    e.gen_reserved0 = r.read_u8();
    e.grenades = r.read_u8();
    e.ref_num = r.read_u8(); // engine-consumed (entity+533), NOT reserved [orig: @0x40e9f0]
    e.mission_critical = r.read_u8();
    e.lfp_group = r.read_u8();
    if (e.gen_reserved0 != 0) {
        error = "BMS entity has nonzero gen_string reserved bytes";
        return false;
    }
    e.max_attack_distance = r.read_i32();
    e.next_ssn = r.read_i32();
    e.color_override = r.read_u8();
    e.team_budget = r.read_u8();
    e.unk42b = r.read_i16();
    e.unk43 = r.read_i32();
    if (e.unk22 != 0 || e.unk42b != 0 || e.unk43 != 0) {
        error = "BMS entity has nonzero reserved fields";
        return false;
    }

    size_t bytes_read = r.position() - start;
    if (bytes_read != kEntitySize) {
        error = "Entity size mismatch: expected " + std::to_string(kEntitySize) +
                " got " + std::to_string(bytes_read);
        return false;
    }

    // .mis-interchange defaults for a BMS-sourced entity (transient; NOT part of the 172-byte
    // record read above). BMS entity z is always absolute (engine spawn extractor,
    // docs/mission/bms-event-runtime-re.md §6.6), so a .mis export of this entity must declare
    // height_lock, else the original editor treats z as terrain-relative and the object floats
    // by the local terrain height [orig: MisLdr_WriteNileProjectXml @ 0x10004930, misldr.dll —
    // scene Y = z/65536 - (height_lock ? extra_bheight/65536 : 0)].
    e.mis_extra_bheight = 0;
    e.mis_height_lock = 1;

    return true;
}

void write_entity(Writer& w, const Entity& e) {
    w.write_i32(e.type_id);
    w.write_i32(e.name_index);
    w.write_i32(e.id);
    w.write_u32(e.bmsi_attributes & kKnownBmsiAttributeMask);
    w.write_i32(e.x);
    w.write_i32(e.y);
    w.write_i32(e.z);
    w.write_i32(e.wp_distance);
    w.write_i32(e.perception2);
    w.write_i32(e.perfectionist2);
    w.write_i32(e.min_engagement_distance);
    w.write_i32(e.max_engagement_distance);
    w.write_i32(e.wp_number);
    w.write_i16(e.w_accuracy2);
    w.write_i16(e.w_accuracy1);
    w.write_i16(e.yaw);
    w.write_i16(e.pitch);
    w.write_i16(e.roll);
    w.write_i16(e.spawns);
    w.write_u8(e.crouch_timer);
    w.write_u8(e.unk15a);
    w.write_i16(e.shoot_timer);
    w.write_i16(e.wp_adv_trigger);
    w.write_i16(e.attention);
    w.write_u8(e.alert_state);
    w.write_u8(e.team);
    w.write_u8(e.no_more_than);
    w.write_u8(e.no_less_than);
    w.write_i16(e.weapon_types);
    w.write_u8(e.group_id);
    w.write_u8(e.waypoint_id);
    w.write_u8(e.obliqueness);
    w.write_u8(e.map_symbol);
    w.write_i16(0);
    w.write_i16(e.blink_parent);
    w.write_i16(e.blink_group);
    w.write_i16(e.group_rel_lo);
    w.write_i16(e.group_rel_hi);
    w.write_i32(e.advancetimer);
    w.write_i32(e.ttool_index);
    w.write_i32(e.wp_goals);
    w.write_fixed_string(e.name1, 8);
    w.write_fixed_string(e.name2, 8);
    w.write_fixed_string(e.gen_string, sizeof(e.gen_string));
    w.write_u8(0);
    w.write_u8(e.grenades);
    w.write_u8(e.ref_num);
    w.write_u8(e.mission_critical);
    w.write_u8(e.lfp_group);
    w.write_i32(e.max_attack_distance);
    w.write_i32(e.next_ssn);
    w.write_u8(e.color_override);
    w.write_u8(e.team_budget);
    w.write_i16(0);
    w.write_i32(0);
}

bool parse_waypoint_record(Reader& r, WaypointRecord& wp, std::string& error) {
    size_t start = r.position();

    wp.flags = static_cast<WaypointFlags>(r.read_u32());
    wp.marker_count = r.read_u32();

    // The on-disk record is a fixed 136 bytes: flags(4) + marker_count(4) + a 128-byte data region
    // (kMaxWaypointSlots = 32 marker-index u32s). The engine reads the whole waypoint block as one
    // fixed blob (fread(Buffer, 0x88, 0x80) @0x40fb56) and NEVER validates marker_count at load -- so
    // a record may carry a count > 32 (CP19.bms ships one with marker_count == 39). Preserve the count
    // verbatim for byte-exact round-trip, but cap the slot split at the 128-byte region so the padding
    // math can't underflow (this also hardens against an outright corrupt count).
    // (The engine also forces flags |= 1 when marker_count == 1 [orig: @0x40fb7a] -- a runtime
    //  normalization applied after load, so we do NOT replicate it here; it would break round-trip.)
    constexpr uint32_t kMaxWaypointSlots = (kWaypointRecordSize - 8) / 4; // 32
    const uint32_t slots = wp.marker_count < kMaxWaypointSlots ? wp.marker_count : kMaxWaypointSlots;

    wp.waypoint_numbers.clear();
    for (uint32_t i = 0; i < slots; i++) {
        wp.waypoint_numbers.push_back(r.read_u32());
    }

    // Read remaining bytes as padding (128-byte region minus the slots we consumed).
    size_t used = slots * 4;
    size_t remaining = 128 - used;
    wp.padding.resize(remaining);
    r.read_bytes(wp.padding.data(), remaining);

    size_t bytes_read = r.position() - start;
    if (bytes_read != kWaypointRecordSize) {
        error = "Waypoint record size mismatch: expected " + std::to_string(kWaypointRecordSize) +
                " got " + std::to_string(bytes_read);
        return false;
    }

    return true;
}

void write_waypoint_record(Writer& w, const WaypointRecord& wp) {
    w.write_u32(static_cast<uint32_t>(wp.flags));
    w.write_u32(wp.marker_count);

    for (uint32_t n : wp.waypoint_numbers) {
        w.write_u32(n);
    }

    // Write padding
    w.write_bytes(wp.padding);
}

bool parse_group_record(Reader& r, GroupRecord& gr, std::string& error) {
    const int32_t flags = r.read_i32();
    const int32_t zero_after_flags = r.read_i32();
    const int32_t value = r.read_i32();
    const int32_t constant10 = r.read_i32();
    int32_t tail[4] = {};
    for (int i = 0; i < 4; ++i) {
        tail[i] = r.read_i32();
    }
    if ((flags & ~0x3) != 0) {
        error = "BMS group record has unsupported flag bits";
        return false;
    }
    if (zero_after_flags != 0 || tail[0] != 0 || tail[1] != 0 || tail[2] != 0 || tail[3] != 0) {
        error = "BMS group record has nonzero reserved bytes";
        return false;
    }
    const bool empty_record = flags == 0 && zero_after_flags == 0 && value == 0 && constant10 == 0 &&
                              tail[0] == 0 && tail[1] == 0 && tail[2] == 0 && tail[3] == 0;
    if (constant10 != 10 && !empty_record) {
        error = "BMS group record constant is not 10";
        return false;
    }
    gr.flags = flags;
    gr.value = value;
    return true;
}

void write_group_record(Writer& w, const GroupRecord& gr) {
    w.write_i32(gr.flags & 0x3);
    w.write_i32(0);
    w.write_i32(gr.value);
    w.write_i32(10);
    w.write_zeros(16);
}

bool parse_layer_record(Reader& r, LayerRecord& lr, std::string& /*error*/) {
    r.read_fixed_string(lr.name, kLayerRecordSize);
    return true;
}

void write_layer_record(Writer& w, const LayerRecord& lr) {
    w.write_fixed_string(lr.name, kLayerRecordSize);
}

bool parse_area_trigger(Reader& r, AreaTrigger& at, std::string& /*error*/) {
    // [orig: interleaved per-axis layout, no swap — Entity_IsTeamInTriggerBounds @0x43c75c]
    at.id = r.read_i32();      // off 0
    at.x_min = r.read_i32();   // off 4
    at.x_max = r.read_i32();   // off 8
    at.y_min = r.read_i32();   // off 12
    at.y_max = r.read_i32();   // off 16
    at.z_min = r.read_i32();   // off 20
    at.z_max = r.read_i32();   // off 24
    at.flags = r.read_u32();   // off 28
    return true;
}

void write_area_trigger(Writer& w, const AreaTrigger& at) {
    w.write_i32(at.id);
    w.write_i32(at.x_min);
    w.write_i32(at.x_max);
    w.write_i32(at.y_min);
    w.write_i32(at.y_max);
    w.write_i32(at.z_min);
    w.write_i32(at.z_max);
    w.write_u32(at.flags);
}

bool parse_event(Reader& r, Event& e, std::string& error) {
    e.flags = static_cast<EventFlags>(r.read_i32());
    if ((static_cast<uint32_t>(e.flags) & ~kEventKnownFlagMask) != 0) {
        error = "BMS event has unsupported flag bits";
        return false;
    }
    e.trigger_index = r.read_i32();
    e.action_index = r.read_i32();

    // Upper 10 bits contain the value (lower 22 bits are always zero). Read unsigned and shift
    // logically: a signed arithmetic >> would sign-extend any value >= 512 into a negative result.
    e.reset_after = static_cast<int32_t>(r.read_u32() >> 22);
    e.delay = static_cast<int32_t>(r.read_u32() >> 22);

    e.unknown5 = r.read_u8();
    e.trigger_count = r.read_u8();
    e.action_count = r.read_u8();
    e.unknown6 = r.read_u8();
    if (e.unknown5 != 0 || e.unknown6 != 0) {
        error = "BMS event has nonzero reserved runtime bytes";
        return false;
    }

    return true;
}

void write_event(Writer& w, const Event& e) {
    w.write_i32(static_cast<int32_t>(static_cast<uint32_t>(e.flags) & kEventKnownFlagMask));
    w.write_i32(e.trigger_index);
    w.write_i32(e.action_index);
    // Reconstruct raw value: upper 10 bits contain value, lower 22 bits are zero. Shift as
    // unsigned -- e.reset_after << 22 on a signed int32 is UB for values >= 512 (1023 << 22
    // overflows INT32_MAX); the written byte pattern is identical to the well-defined form.
    w.write_u32(static_cast<uint32_t>(e.reset_after) << 22);
    w.write_u32(static_cast<uint32_t>(e.delay) << 22);
    w.write_u8(0);
    w.write_u8(e.trigger_count);
    w.write_u8(e.action_count);
    w.write_u8(0);
}

bool parse_trigger(Reader& r, Trigger& t, std::string& error) {
    t.condition_flags = r.read_i32();
    t.main_type = static_cast<TriggerMainType>(r.read_i32());
    t.sub_type = r.read_i32();
    t.param1 = r.read_i32();
    t.param2 = r.read_i32();
    t.param3 = r.read_i32();
    t.param4 = r.read_i32();
    t.unknown7 = r.read_i32();
    if (t.unknown7 != 0) {
        error = "BMS trigger has nonzero reserved field";
        return false;
    }
    return true;
}

void write_trigger(Writer& w, const Trigger& t) {
    w.write_i32(t.condition_flags);
    w.write_i32(static_cast<int32_t>(t.main_type));
    w.write_i32(t.sub_type);
    w.write_i32(t.param1);
    w.write_i32(t.param2);
    w.write_i32(t.param3);
    w.write_i32(t.param4);
    w.write_i32(0);
}

bool parse_action(Reader& r, Action& a, std::string& error) {
    a.reserved0 = r.read_i32();
    a.action_type = static_cast<ActionType>(r.read_i32());
    a.action_sub_type = r.read_i32();
    a.param1 = r.read_i32();
    a.param2 = r.read_i32();
    a.param3 = r.read_i32();
    a.param4 = r.read_i32();
    a.reserved1 = r.read_i32();
    if (a.reserved0 != 0 || a.reserved1 != 0) {
        error = "BMS action has nonzero reserved field";
        return false;
    }
    return true;
}

void write_action(Writer& w, const Action& a) {
    w.write_i32(0);
    w.write_i32(static_cast<int32_t>(a.action_type));
    w.write_i32(a.action_sub_type);
    w.write_i32(a.param1);
    w.write_i32(a.param2);
    w.write_i32(a.param3);
    w.write_i32(a.param4);
    w.write_i32(0);
}

bool parse_bounding_box(Reader& r, BoundingBox& bb, std::string& error) {
    // 0x24-byte record: min/max XYZ (16.16) + type/ref metadata + reserved zero.
    bb.min_x = r.read_i32();
    bb.min_y = r.read_i32();
    bb.min_z = r.read_i32();
    bb.max_x = r.read_i32();
    bb.max_y = r.read_i32();
    bb.max_z = r.read_i32();
    bb.type = r.read_i32();
    bb.ref_id = r.read_i32();
    bb.reserved0 = r.read_i32();
    if (bb.reserved0 != 0) {
        error = "BMS bounding box has nonzero reserved field";
        return false;
    }
    return true;
}

void write_bounding_box(Writer& w, const BoundingBox& bb) {
    w.write_i32(bb.min_x);
    w.write_i32(bb.min_y);
    w.write_i32(bb.min_z);
    w.write_i32(bb.max_x);
    w.write_i32(bb.max_y);
    w.write_i32(bb.max_z);
    w.write_i32(bb.type);
    w.write_i32(bb.ref_id);
    w.write_i32(0);
}

// Reject a record count that cannot fit in the bytes left in the buffer. The pool/chunk counts come
// straight from the (possibly corrupt or hostile) header; a negative or huge value handed to
// std::vector::resize() throws std::length_error / std::bad_alloc, and nothing up the load chain
// catches C++ exceptions, so it would abort the process instead of failing the parse cleanly.
// record_size must be > 0.
bool count_fits(const Reader& r, int64_t count, size_t record_size,
                const char* what, std::string& error) {
    if (count < 0 || static_cast<uint64_t>(count) > r.remaining() / record_size) {
        error = std::string("BMS ") + what + " count " + std::to_string(count) +
                " is invalid (" + std::to_string(r.remaining()) + " bytes remain)";
        return false;
    }
    return true;
}

bool read_nul_field(const std::vector<uint8_t>& raw, size_t& pos, std::string& out) {
    const size_t start = pos;
    while (pos < raw.size() && raw[pos] != 0) {
        ++pos;
    }
    if (pos >= raw.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(raw.data() + start), pos - start);
    ++pos;
    return true;
}

bool contains_nul(const std::string& value) {
    return value.find('\0') != std::string::npos;
}

bool write_weapon_loadout_chunk(const WeaponLoadout& loadout, std::vector<uint8_t>& out, std::string& error);

size_t find_nul(const std::vector<uint8_t>& raw, size_t pos, size_t limit) {
    while (pos < limit && raw[pos] != 0) {
        ++pos;
    }
    return pos;
}

size_t loadout_parse_limit(const std::vector<uint8_t>& raw) {
    if (raw.empty() || raw[0] == 0) {
        return 0;
    }
    for (size_t i = 0; i + 1 < raw.size(); ++i) {
        if (raw[i] == 0 && raw[i + 1] == 0) {
            return i;
        }
    }
    return raw.size();
}

bool is_loadout_name_at(const std::vector<uint8_t>& raw, size_t pos, size_t limit) {
    return pos + 4 <= limit && raw[pos] == 'W' && raw[pos + 1] == 'P' &&
           raw[pos + 2] == 'N' && raw[pos + 3] == '_';
}

std::string loadout_string_at(const std::vector<uint8_t>& raw, size_t pos, size_t limit) {
    const size_t end = find_nul(raw, pos, limit);
    return std::string(reinterpret_cast<const char*>(raw.data() + pos), end - pos);
}

std::string sanitize_loadout_value(const std::string& value) {
    if (value.empty()) {
        return "-1";
    }
    size_t pos = 0;
    if (value[pos] == '-' || value[pos] == '+') {
        ++pos;
    }
    const size_t digits_start = pos;
    while (pos < value.size() && value[pos] >= '0' && value[pos] <= '9') {
        ++pos;
    }
    if (pos == digits_start) {
        return "-1";
    }
    return value.substr(0, pos);
}

std::string loadout_value_after(const std::vector<uint8_t>& raw, size_t& pos, size_t limit) {
    if (pos >= limit) {
        return "-1";
    }
    const std::string value = loadout_string_at(raw, pos, limit);
    pos = find_nul(raw, pos, limit);
    if (pos < limit) {
        ++pos;
    }
    return sanitize_loadout_value(value);
}

bool parse_weapon_loadout_chunk(const std::vector<uint8_t>& raw, WeaponLoadout& out, std::string& error) {
    out.entries.clear();
    if (raw.empty()) {
        return true;
    }

    const size_t limit = loadout_parse_limit(raw);
    if (limit == 0) {
        return true;
    }
    for (size_t pos = 0; pos < limit; ++pos) {
        if (!is_loadout_name_at(raw, pos, limit)) {
            continue;
        }
        const size_t name_end = find_nul(raw, pos, limit);
        if (name_end == limit) {
            error = "BMS weapon loadout chunk has an unterminated weapon name";
            return false;
        }
        WeaponLoadoutRecord entry;
        entry.name = loadout_string_at(raw, pos, limit);
        size_t value_pos = name_end + 1;
        entry.ammo_primary = loadout_value_after(raw, value_pos, limit);
        entry.ammo_secondary = loadout_value_after(raw, value_pos, limit);
        entry.flags = loadout_value_after(raw, value_pos, limit);
        out.entries.push_back(std::move(entry));
        pos = name_end;
    }
    if (out.entries.empty()) {
        error = "BMS weapon loadout chunk has no weapon names";
        return false;
    }
    return true;
}

bool write_weapon_loadout_chunk(const WeaponLoadout& loadout, std::vector<uint8_t>& out, std::string& error) {
    out.clear();
    for (const WeaponLoadoutRecord& entry : loadout.entries) {
        if (entry.name.empty()) {
            error = "BMS weapon loadout entries require a name";
            return false;
        }
        const std::string flags = entry.flags.empty() ? "-1" : entry.flags;
        if (contains_nul(entry.name) || contains_nul(entry.ammo_primary) ||
            contains_nul(entry.ammo_secondary) || contains_nul(flags)) {
            error = "BMS weapon loadout entries cannot contain embedded NUL bytes";
            return false;
        }
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.push_back(0);
        out.insert(out.end(), entry.ammo_primary.begin(), entry.ammo_primary.end());
        out.push_back(0);
        out.insert(out.end(), entry.ammo_secondary.begin(), entry.ammo_secondary.end());
        out.push_back(0);
        out.insert(out.end(), flags.begin(), flags.end());
        out.push_back(0);
    }
    if (!out.empty()) {
        out.push_back(0);
    }
    return true;
}

bool parse_item_availability_chunk(const std::vector<uint8_t>& raw,
                                   std::vector<ItemAvailabilityEntry>& out,
                                   std::string& error) {
    out.clear();
    if (raw.empty()) {
        return true;
    }

    size_t pos = 0;
    while (true) {
        if (pos >= raw.size()) {
            error = "BMS item availability chunk missing terminator";
            return false;
        }
        if (raw[pos] == 0) {
            ++pos;
            break;
        }

        ItemAvailabilityEntry entry;
        if (!read_nul_field(raw, pos, entry.name)) {
            error = "BMS item availability chunk has an unterminated name";
            return false;
        }
        if (pos >= raw.size()) {
            error = "BMS item availability chunk missing status byte";
            return false;
        }
        entry.status = raw[pos++];
        out.push_back(std::move(entry));
    }

    if (pos != raw.size()) {
        error = "BMS item availability chunk has trailing bytes";
        return false;
    }
    return true;
}

bool write_item_availability_chunk(const std::vector<ItemAvailabilityEntry>& entries,
                                   std::vector<uint8_t>& out,
                                   std::string& error) {
    out.clear();
    for (const ItemAvailabilityEntry& entry : entries) {
        if (entry.name.empty()) {
            error = "BMS item availability entries require a name";
            return false;
        }
        if (contains_nul(entry.name)) {
            error = "BMS item availability names cannot contain embedded NUL bytes";
            return false;
        }
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.push_back(0);
        out.push_back(entry.status);
    }
    if (!out.empty()) {
        out.push_back(0);
    }
    return true;
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

bool is_bms(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    return data[0] == 'B' && data[1] == 'M' && data[2] == 'S';
}

bool parse(const uint8_t* data, size_t size, File& out, std::string& error) {
    if (!is_bms(data, size)) {
        error = "Not a BMS file (invalid magic)";
        return false;
    }

    Reader r(data, size);

    // Section read order + sizes verified byte-for-byte against the engine loader
    // [orig: Mission_LoadBMSFile @0x40f7b6 (Jointops.exe)]:
    //   header 0x268 -> weapon-loadout chunk (hdr+0x242) -> secondary chunk (hdr+0x246, seeked) ->
    //   items/buildings/markers/organics (0xAC each; counts @hdr+0xA4/0xA8/0xAC/0xB0) ->
    //   waypoints 0x88 x128 -> groups 0x20 x64 -> layers 0x14 x32 ->
    //   area triggers 0x20 x (hdr+0x240) -> event block -> bbox count(i32) + boxes 0x24.
    // The event block (counts + arrays) is read by EventTrigger_LoadAllData @0x453eb0. This order is
    // exercised end-to-end by tests/mission/mission_corpus_test.cpp (canonical idempotence over the
    // full shipped mission set).
    if (!parse_header(r, out.header, error)) {
        return false;
    }

    // Parse weapon loadout [orig: word_A76412 @hdr+0x242 bytes; read+sanitized in SP, seeked in MP]
    // Guard the length like the record arrays below: read_bytes zero-fills and does NOT advance on an
    // underflow, so a chunk length larger than the bytes remaining would silently mis-align every
    // section after it (a corrupt/truncated file would parse to garbage instead of failing cleanly).
    if (!count_fits(r, out.header.weapon_loadout_chunk_len, 1, "weapon loadout chunk", error)) return false;
    std::vector<uint8_t> loadout_chunk;
    r.read_bytes(loadout_chunk, out.header.weapon_loadout_chunk_len);
    if (!parse_weapon_loadout_chunk(loadout_chunk, out.loadout, error)) {
        return false;
    }
    if (!write_weapon_loadout_chunk(out.loadout, loadout_chunk, error)) {
        return false;
    }
    out.header.weapon_loadout_chunk_len = static_cast<uint16_t>(loadout_chunk.size());

    // Parse the second chunk the engine always seeks past after the loadout.
    // [orig: word_A76416 @hdr+0x246 bytes; fseek at @0x40f751 (SP) / @0x40f6d1 (MP) in Mission_LoadBMSFile]
    // Usually empty; parsed as an item-availability list so a mission that uses it stays modeled.
    if (!count_fits(r, out.header.secondary_chunk_len, 1, "secondary chunk", error)) return false;
    std::vector<uint8_t> item_availability_chunk;
    r.read_bytes(item_availability_chunk, out.header.secondary_chunk_len);
    if (!parse_item_availability_chunk(item_availability_chunk, out.item_availability, error)) {
        return false;
    }

    // Parse entities
    if (!count_fits(r, out.header.num_items, kEntitySize, "item", error)) return false;
    out.items.resize(out.header.num_items);
    for (uint32_t i = 0; i < out.header.num_items; i++) {
        if (!parse_entity(r, out.items[i], error)) {
            return false;
        }
        out.items[i].type = ItemType::Item;
    }

    if (!count_fits(r, out.header.num_buildings, kEntitySize, "building", error)) return false;
    out.buildings.resize(out.header.num_buildings);
    for (uint32_t i = 0; i < out.header.num_buildings; i++) {
        if (!parse_entity(r, out.buildings[i], error)) {
            return false;
        }
        out.buildings[i].type = ItemType::Building;
    }

    if (!count_fits(r, out.header.num_markers, kEntitySize, "marker", error)) return false;
    out.markers.resize(out.header.num_markers);
    for (uint32_t i = 0; i < out.header.num_markers; i++) {
        if (!parse_entity(r, out.markers[i], error)) {
            return false;
        }
        out.markers[i].type = ItemType::Marker;
    }

    if (!count_fits(r, out.header.num_people, kEntitySize, "organic", error)) return false;
    out.organics.resize(out.header.num_people);
    for (uint32_t i = 0; i < out.header.num_people; i++) {
        if (!parse_entity(r, out.organics[i], error)) {
            return false;
        }
        out.organics[i].type = ItemType::Organic;
    }

    // Parse waypoint records (fixed count: 128)
    out.waypoint_records.resize(kWaypointRecordCount);
    for (int i = 0; i < kWaypointRecordCount; i++) {
        if (!parse_waypoint_record(r, out.waypoint_records[i], error)) {
            return false;
        }
    }

    // Parse group records (fixed count: 64)
    out.group_records.resize(kGroupRecordCount);
    for (int i = 0; i < kGroupRecordCount; i++) {
        if (!parse_group_record(r, out.group_records[i], error)) {
            return false;
        }
    }

    // Parse layer records (fixed count: 32)
    out.layer_records.resize(kLayerRecordCount);
    for (int i = 0; i < kLayerRecordCount; i++) {
        if (!parse_layer_record(r, out.layer_records[i], error)) {
            return false;
        }
    }

    // Parse area triggers
    if (!count_fits(r, out.header.area_trigger_count, kAreaTriggerSize, "area-trigger", error)) return false;
    out.area_triggers.resize(out.header.area_trigger_count);
    for (int16_t i = 0; i < out.header.area_trigger_count; i++) {
        if (!parse_area_trigger(r, out.area_triggers[i], error)) {
            return false;
        }
    }

    // Read event/trigger/action counts, then each array. This dedicated 3-count block -- NOT the
    // header's num_events @hdr+0xB4 -- is the loader's source of truth for the event count
    // [orig: EventTrigger_LoadAllData @0x453eb0 reads dword_AE0700/0708/0710 then sizes the arrays
    // 24/32/32]. header.num_events is round-tripped verbatim but is not consulted to read events here.
    out.events_count = r.read_i32();
    out.trigger_count = r.read_i32();
    out.action_count = r.read_i32();

    // Parse events
    if (!count_fits(r, out.events_count, kEventSize, "event", error)) return false;
    out.events.resize(out.events_count);
    for (int32_t i = 0; i < out.events_count; i++) {
        if (!parse_event(r, out.events[i], error)) {
            return false;
        }
    }

    // Parse triggers
    if (!count_fits(r, out.trigger_count, kTriggerSize, "trigger", error)) return false;
    out.triggers.resize(out.trigger_count);
    for (int32_t i = 0; i < out.trigger_count; i++) {
        if (!parse_trigger(r, out.triggers[i], error)) {
            return false;
        }
    }

    // Parse actions
    if (!count_fits(r, out.action_count, kActionSize, "action", error)) return false;
    out.actions.resize(out.action_count);
    for (int32_t i = 0; i < out.action_count; i++) {
        if (!parse_action(r, out.actions[i], error)) {
            return false;
        }
    }

    // Read bounding box count and parse
    out.bounding_box_count = r.read_i32();
    if (!count_fits(r, out.bounding_box_count, kBoundingBoxSize, "bounding-box", error)) return false;
    out.bounding_boxes.resize(out.bounding_box_count);
    for (int32_t i = 0; i < out.bounding_box_count; i++) {
        if (!parse_bounding_box(r, out.bounding_boxes[i], error)) {
            return false;
        }
    }

    // Verify we consumed all data
    if (r.position() != size) {
        error = "Did not consume all data: at " + std::to_string(r.position()) +
                " but size is " + std::to_string(size);
        return false;
    }

    return true;
}

bool parse_file(const std::string& path, File& out, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "Cannot open file: " + path;
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(size);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        error = "Cannot read file: " + path;
        return false;
    }

    return parse(data.data(), data.size(), out, error);
}

bool encode_header_blob(const File& file, std::vector<uint8_t>& out, std::string& error) {
    std::vector<uint8_t> loadout_chunk;
    if (!write_weapon_loadout_chunk(file.loadout, loadout_chunk, error)) {
        return false;
    }
    std::vector<uint8_t> item_availability_chunk;
    if (!write_item_availability_chunk(file.item_availability, item_availability_chunk, error)) {
        return false;
    }

    File header_file = file;
    header_file.header.weapon_loadout_chunk_len = static_cast<uint16_t>(loadout_chunk.size());
    header_file.header.secondary_chunk_len = static_cast<uint16_t>(item_availability_chunk.size());

    Writer w;
    write_header(w, header_file.header);
    out = w.data();
    if (out.size() != kHeaderSize) {
        error = "Encoded BMS header size mismatch";
        return false;
    }
    return true;
}

bool write(const File& file, std::vector<uint8_t>& out, std::string& error) {
    Writer w;
    std::vector<uint8_t> header_blob;
    if (!encode_header_blob(file, header_blob, error)) {
        return false;
    }
    std::vector<uint8_t> loadout_chunk;
    if (!write_weapon_loadout_chunk(file.loadout, loadout_chunk, error)) {
        return false;
    }
    std::vector<uint8_t> item_availability_chunk;
    if (!write_item_availability_chunk(file.item_availability, item_availability_chunk, error)) {
        return false;
    }

    // Write header
    w.write_bytes(header_blob);

    // Write weapon loadout, then the item-availability chunk (mirrors the read order in parse()).
    w.write_bytes(loadout_chunk);
    w.write_bytes(item_availability_chunk);

    // Write entities
    for (const auto& e : file.items) {
        write_entity(w, e);
    }
    for (const auto& e : file.buildings) {
        write_entity(w, e);
    }
    for (const auto& e : file.markers) {
        write_entity(w, e);
    }
    for (const auto& e : file.organics) {
        write_entity(w, e);
    }

    // Write waypoint records (must be exactly 128)
    if (file.waypoint_records.size() != kWaypointRecordCount) {
        error = "Waypoint record count must be " + std::to_string(kWaypointRecordCount);
        return false;
    }
    for (const auto& wp : file.waypoint_records) {
        write_waypoint_record(w, wp);
    }

    // Write group records (must be exactly 64)
    if (file.group_records.size() != kGroupRecordCount) {
        error = "Group record count must be " + std::to_string(kGroupRecordCount);
        return false;
    }
    for (const auto& gr : file.group_records) {
        write_group_record(w, gr);
    }

    // Write layer records (must be exactly 32)
    if (file.layer_records.size() != kLayerRecordCount) {
        error = "Layer record count must be " + std::to_string(kLayerRecordCount);
        return false;
    }
    for (const auto& lr : file.layer_records) {
        write_layer_record(w, lr);
    }

    // Write area triggers
    for (const auto& at : file.area_triggers) {
        write_area_trigger(w, at);
    }

    // Write event/trigger/action counts
    w.write_i32(file.events_count);
    w.write_i32(file.trigger_count);
    w.write_i32(file.action_count);

    // Write events
    for (const auto& e : file.events) {
        write_event(w, e);
    }

    // Write triggers
    for (const auto& t : file.triggers) {
        write_trigger(w, t);
    }

    // Write actions
    for (const auto& a : file.actions) {
        write_action(w, a);
    }

    // Write bounding box count and boxes
    w.write_i32(file.bounding_box_count);
    for (const auto& bb : file.bounding_boxes) {
        write_bounding_box(w, bb);
    }

    out = std::move(w.data());
    return true;
}

bool write_file(const File& file, const std::string& path, std::string& error) {
    std::vector<uint8_t> data;
    if (!write(file, data, error)) {
        return false;
    }

    std::ofstream out_file(path, std::ios::binary);
    if (!out_file) {
        error = "Cannot create file: " + path;
        return false;
    }

    out_file.write(reinterpret_cast<const char*>(data.data()), data.size());
    if (!out_file) {
        error = "Cannot write file: " + path;
        return false;
    }

    return true;
}

namespace {

// Byte compare a vector of trivially-copyable records. Every fixed BMS record is value-initialized
// on construction (padding included) and mutators only touch named fields, so this is exact. Copies
// (EditHistory undo/baseline snapshots) preserve padding only for TRIVIAL types -- a non-trivial
// (e.g. NSDMI'd) record may be copied member-wise, leaving padding indeterminate -- which the
// static_asserts below enforce.
template <typename T>
bool pod_vectors_equal(const std::vector<T>& a, const std::vector<T>& b) {
    static_assert(std::is_trivially_copyable<T>::value, "pod_vectors_equal needs a trivially-copyable element");
    static_assert(std::is_trivial<T>::value,
                  "pod_vectors_equal needs a trivial element: non-trivial (e.g. NSDMI'd) records broke the "
                  "padding-preserving copy on libc++/arm64 and made the memcmp read indeterminate padding "
                  "(macOS-only dirty-flag regression, 2026-07)");
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0;
}

// WaypointRecord carries inner vectors, so it cannot be byte-compared; compare field-wise.
bool waypoint_records_equal(const std::vector<WaypointRecord>& a, const std::vector<WaypointRecord>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].flags != b[i].flags || a[i].marker_count != b[i].marker_count ||
            a[i].waypoint_numbers != b[i].waypoint_numbers || a[i].padding != b[i].padding) {
            return false;
        }
    }
    return true;
}

bool weapon_loadout_equal(const WeaponLoadout& a, const WeaponLoadout& b) {
    if (a.entries.size() != b.entries.size()) {
        return false;
    }
    for (size_t i = 0; i < a.entries.size(); ++i) {
        if (a.entries[i].name != b.entries[i].name ||
            a.entries[i].ammo_primary != b.entries[i].ammo_primary ||
            a.entries[i].ammo_secondary != b.entries[i].ammo_secondary ||
            a.entries[i].flags != b.entries[i].flags) {
            return false;
        }
    }
    return true;
}

bool item_availability_equal(const std::vector<ItemAvailabilityEntry>& a,
                             const std::vector<ItemAvailabilityEntry>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].status != b[i].status) {
            return false;
        }
    }
    return true;
}

} // namespace

bool equal(const File& a, const File& b) {
    // Header is packed (static_assert sizeof == 616); a byte compare is exact and includes the
    // count fields. The body comparisons below are over the actual vectors, so they detect a
    // change even when a header count is momentarily out of sync with its vector (sync_counts runs
    // only at write time) -- the comparison never depends on that.
    if (std::memcmp(&a.header, &b.header, sizeof(Header)) != 0) {
        return false;
    }
    if (!weapon_loadout_equal(a.loadout, b.loadout) ||
        !item_availability_equal(a.item_availability, b.item_availability)) {
        return false;
    }
    if (!pod_vectors_equal(a.items, b.items) || !pod_vectors_equal(a.buildings, b.buildings) ||
        !pod_vectors_equal(a.markers, b.markers) || !pod_vectors_equal(a.organics, b.organics)) {
        return false;
    }
    if (!waypoint_records_equal(a.waypoint_records, b.waypoint_records) ||
        !pod_vectors_equal(a.group_records, b.group_records) ||
        !pod_vectors_equal(a.layer_records, b.layer_records)) {
        return false;
    }
    if (!pod_vectors_equal(a.area_triggers, b.area_triggers)) {
        return false;
    }
    if (a.events_count != b.events_count || a.trigger_count != b.trigger_count ||
        a.action_count != b.action_count || a.bounding_box_count != b.bounding_box_count) {
        return false;
    }
    if (!pod_vectors_equal(a.events, b.events) || !pod_vectors_equal(a.triggers, b.triggers) ||
        !pod_vectors_equal(a.actions, b.actions) || !pod_vectors_equal(a.bounding_boxes, b.bounding_boxes)) {
        return false;
    }
    return true;
}

std::vector<const Entity*> File::all_entities() const {
    std::vector<const Entity*> result;
    result.reserve(items.size() + buildings.size() + markers.size() + organics.size());
    for (const auto& e : items) result.push_back(&e);
    for (const auto& e : buildings) result.push_back(&e);
    for (const auto& e : markers) result.push_back(&e);
    for (const auto& e : organics) result.push_back(&e);
    return result;
}

std::string File::get_mission_name() const {
    return fixed_string(header.mission_name, sizeof(header.mission_name));
}

std::string File::get_designer() const {
    return fixed_string(header.designer, sizeof(header.designer));
}

std::string File::get_terrain() const {
    // terrain[48] is three 16-byte slots (terrain / cnv_file / tt_file); the terrain name is the
    // first slot. Bound to 16 so a full 16-char name does not run on into cnv_file.
    return fixed_string(header.terrain, 16);
}

} // namespace opennova::bms
