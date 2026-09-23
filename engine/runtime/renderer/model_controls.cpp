#include <runtime/renderer/model_controls.h>

#include <runtime/world/ai.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opennova::renderer {
namespace {

constexpr double kPartAnimTickSeconds = 0.016;

int32_t signed_dword(int64_t value) {
	// Retail's global CTRL bus stores signed dwords, including exact 0x10000
	// endpoints and negative angular controls.
	const uint32_t low = static_cast<uint32_t>(value);
	int32_t result;
	std::memcpy(&result, &low, sizeof(result));
	return result;
}

void erase_ordinal(std::vector<int> &order, int ordinal) {
	order.erase(std::remove(order.begin(), order.end(), ordinal), order.end());
}

} // namespace

bool ModelControls::valid(int ordinal) {
	return ordinal >= 0 && ordinal < threedi::THREEDI_CTRL_REGISTER_COUNT;
}

bool ModelControls::has(int ordinal) const {
	return valid(ordinal) && present_[static_cast<size_t>(ordinal)];
}

int32_t ModelControls::value(int ordinal) const {
	return has(ordinal) ? values_[static_cast<size_t>(ordinal)] : 0;
}

void ModelControls::write_value(int ordinal, int32_t value) {
	if (!has(ordinal)) {
		present_.set(static_cast<size_t>(ordinal));
		present_order_.push_back(ordinal);
	}
	values_[static_cast<size_t>(ordinal)] = value;
}

void ModelControls::erase_owner(int ordinal) {
	if (owners_.erase(ordinal)) erase_ordinal(owner_order_, ordinal);
}

// Publish one dedicated retail writer into the register's single current
// value. The owner tag is lifecycle bookkeeping only: a stale teardown cannot
// clear a later writer's store, and overwritten values are never stacked or
// restored. [orig: global CTRL value slots @0x83FCE8, stride 8]
bool ModelControls::store(int ordinal, int64_t value, const std::string &owner) {
	if (!valid(ordinal)) return false;
	const int32_t next = signed_dword(value);
	const auto current = owners_.find(ordinal);
	if (has(ordinal) && values_[static_cast<size_t>(ordinal)] == next &&
			(owner.empty() ? current == owners_.end()
					: current != owners_.end() && current->second == owner)) return false;
	write_value(ordinal, next);
	if (owner.empty()) {
		erase_owner(ordinal);
	} else {
		if (current == owners_.end()) owner_order_.push_back(ordinal);
		owners_[ordinal] = owner;
	}
	return true;
}

bool ModelControls::clear(int ordinal) {
	if (!has(ordinal)) return false;
	present_.reset(static_cast<size_t>(ordinal));
	values_[static_cast<size_t>(ordinal)] = 0;
	erase_ordinal(present_order_, ordinal);
	erase_owner(ordinal);
	return true;
}

bool ModelControls::clear_owned(int ordinal, const std::string &owner) {
	if (owner.empty()) return false;
	const auto found = owners_.find(ordinal);
	return found != owners_.end() && found->second == owner && clear(ordinal);
}

std::vector<int> ModelControls::owned_registers(const std::string &owner) const {
	std::vector<int> out;
	if (!owner.empty()) {
		for (int ordinal : owner_order_) {
			if (owners_.at(ordinal) == owner) out.push_back(ordinal);
		}
	}
	return out;
}

ControlRegisterValues ModelControls::runtime_values(int32_t flicker, int32_t swing) const {
	ControlRegisterValues out = values_;
	if (!has(threedi::THREEDI_CTRL_FLICKER)) out[threedi::THREEDI_CTRL_FLICKER] = flicker;
	if (!has(threedi::THREEDI_CTRL_SWING)) out[threedi::THREEDI_CTRL_SWING] = swing;
	return out;
}

void ModelControls::sample_weather(int32_t flicker, int32_t swing) {
	write_value(threedi::THREEDI_CTRL_FLICKER, flicker);
	write_value(threedi::THREEDI_CTRL_SWING, swing);
}

// [orig: Entity_ApplyCommand @0x43ab60 case 0x22; the per-channel publisher
//  HUD_CacheEntityDisplayInfo @0x4A3E18..0x4A3E38 -> VEHICLE_SPECIAL1/2]
int ModelControls::part_register(int channel) {
	if (channel == 1) return threedi::THREEDI_CTRL_VEHICLE_SPECIAL1;
	if (channel == 2) return threedi::THREEDI_CTRL_VEHICLE_SPECIAL2;
	return threedi::THREEDI_CTRL_REGISTER_NOT_FOUND;
}

const char *ModelControls::part_owner(int channel) {
	if (channel == 1) return "present:part_anim:1";
	if (channel == 2) return "present:part_anim:2";
	return "";
}

void ModelControls::release_part(int channel) {
	const int ordinal = part_register(channel);
	parts_.erase(std::remove_if(parts_.begin(), parts_.end(),
			[ordinal](const Part &part) { return part.ordinal == ordinal; }), parts_.end());
}

// PLAYPARTANIM's rate from ANIMTIME seconds is the shared world helper
// [orig: Entity_ApplyCommand @0x43B1A9..0x43B1F9]. The sweep step is
// world::part_anim_step. The world runtime never integrates the brain phases
// (retail's sweep integrator is unreferenced), so the presenter supplies the
// held brain phases through set_part_phase.
void ModelControls::play_part(int channel, int direction, double seconds) {
	const int ordinal = part_register(channel);
	if (ordinal < 0 || direction < -1 || direction > 1) return;
	if (direction == 0) {
		release_part(channel); // Stop freezes the part at its current value.
		return;
	}
	if (parts_.empty()) part_tick_credit_ = 0.0;
	const Part next{ordinal, direction, world::part_anim_rate_from_seconds(seconds), value(ordinal)};
	for (Part &part : parts_) {
		if (part.ordinal == ordinal) {
			part = next;
			return;
		}
	}
	parts_.push_back(next);
}

ModelControls::Restart ModelControls::restart_part(int channel, int direction, double seconds) {
	const int ordinal = part_register(channel);
	if (ordinal < 0 || direction < -1 || direction > 1) return Restart::Invalid;
	if (direction != 0) store(ordinal, direction > 0 ? 0 : world::kPartAnimPhaseOne);
	play_part(channel, direction, seconds);
	return direction == 0 ? Restart::Stopped : Restart::Seeded;
}

void ModelControls::clear_parts() {
	parts_.clear();
	part_tick_credit_ = 0.0;
}

std::vector<int> ModelControls::active_part_registers() const {
	std::vector<int> out;
	for (const Part &part : parts_) out.push_back(part.ordinal);
	return out;
}

// Advance at retail's fixed 16 ms cadence with wrapping signed-dword ADD/SUB.
// Only strict overshoot clamps and stops; landing exactly on an endpoint keeps
// the direction live for one more tick.
// [orig: Entity_UpdateSuspensionBounce @0x456740..0x4567A9]
bool ModelControls::advance_parts(double delta) {
	if (parts_.empty() || delta <= 0.0) return false;
	part_tick_credit_ += delta;
	const int ticks = static_cast<int>(std::floor((part_tick_credit_ + 0.000000001) / kPartAnimTickSeconds));
	if (ticks <= 0) return false;
	part_tick_credit_ -= static_cast<double>(ticks) * kPartAnimTickSeconds;
	bool changed = false;
	for (int tick = 0; tick < ticks && !parts_.empty(); ++tick) {
		for (auto part = parts_.begin(); part != parts_.end();) {
			const int32_t previous = part->value;
			const bool finished = world::part_anim_step(part->value, part->direction, part->rate);
			const bool inserted = !has(part->ordinal);
			store(part->ordinal, part->value);
			changed = changed || inserted || part->value != previous;
			if (finished) part = parts_.erase(part);
			else ++part;
		}
	}
	return changed;
}

} // namespace opennova::renderer
