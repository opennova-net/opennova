// The present-pass law (runtime/world/present_passes): the husk RENDER pick
// (husk first, huskfinal fallback — apart from the death-piece pick), the
// destroyed entity's presentation identity (the dynamic-vs-authored
// discriminator + the key it spells), and the settling graft's yaw-only tilt
// retention [orig: Entity_RaycastCollisionModel @ 0x413086; the collision
// pick @ 0x538720; the piece pick @ 0x4934af].

#include <runtime/world/entity.h>
#include <runtime/world/present_passes.h>

#include <cstdio>
#include <string>

namespace {
namespace w = opennova::world;

int failures = 0;

#define CHECK(condition)                                                        \
	do {                                                                          \
		if (!(condition)) {                                                          \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #condition);           \
			++failures;                                                                \
		}                                                                           \
	} while (0)

constexpr int32_t kInvalidHandle = static_cast<int32_t>(w::EntityHandle::kInvalid);
constexpr int64_t kNone = static_cast<int64_t>(w::kSpawnOriginNone);

void test_husk_render_pick_is_husk_first_then_huskfinal() {
	CHECK(w::husk_render_graphic("Dbuggy1X", "Dbuggy1F") == "Dbuggy1X");
	CHECK(w::husk_render_graphic("", "Dbuggy1F") == "Dbuggy1F");
	CHECK(w::husk_render_graphic("Dbuggy1X", "") == "Dbuggy1X");
	CHECK(w::husk_render_graphic("", "").empty());
}

void test_dynamic_identity_needs_a_wire_handle_zero_bms_and_the_sentinel_origin() {
	// A synthetic wire-only row: bms 0, the promotion sentinel, a live handle.
	CHECK(w::husk_identity_is_dynamic(0, kNone, 0x1004));
	// Handle zero is a valid pool-0 identity.
	CHECK(w::husk_identity_is_dynamic(0, kNone, 0));
	// No wire identity: negative or the none sentinel.
	CHECK(!w::husk_identity_is_dynamic(0, kNone, -1));
	CHECK(!w::husk_identity_is_dynamic(0, kNone, kInvalidHandle));
	// An authored BMS id remains canonical whatever the handle.
	CHECK(!w::husk_identity_is_dynamic(41, kNone, 0x1004));
	// A real authored origin remains canonical even at BMS id zero.
	const int64_t origin = static_cast<int64_t>(w::spawn_origin_pack(3, 5));
	CHECK(!w::husk_identity_is_dynamic(0, origin, 0x1004));
	// The drains widen the uint32 word: -1 is NOT the sentinel spelling.
	CHECK(!w::husk_identity_is_dynamic(0, -1, 0x1004));
}

void test_identity_key_spells_wire_or_bms_origin_parts() {
	CHECK(w::husk_identity_key(0, kNone, 0x1004) == "wire:4100");
	CHECK(w::husk_identity_key(0, kNone, 0) == "wire:0");
	// Authored: bms:kind:index, the none origin decoding to 255:16777215.
	CHECK(w::husk_identity_key(41, kNone, -1) == "41:255:16777215");
	CHECK(w::husk_identity_key(41, kNone, 0x1004) == "41:255:16777215");
	const int64_t origin = static_cast<int64_t>(w::spawn_origin_pack(3, 5));
	CHECK(w::husk_identity_key(0, origin, -1) == "0:3:5");
	CHECK(w::husk_identity_key(0, origin, 0x1004) == "0:3:5");
	// Two zero-BMS siblings with distinct origins own distinct keys; two
	// synthetic siblings with the same sentinel own distinct wire keys.
	CHECK(w::husk_identity_key(0, static_cast<int64_t>(w::spawn_origin_pack(4, 6)), -1) == "0:4:6");
	CHECK(w::husk_identity_key(0, kNone, 0x1005) == "wire:4101");
}

void test_settle_keeps_the_carved_tilt_only_for_yaw_only_poses() {
	CHECK(w::husk_settle_keeps_carved_tilt(0.0f, 0.0f));
	CHECK(w::husk_settle_keeps_carved_tilt(0.000001f, -0.000001f));
	CHECK(!w::husk_settle_keeps_carved_tilt(17.0f, 0.0f));
	CHECK(!w::husk_settle_keeps_carved_tilt(0.0f, -12.0f));
	CHECK(!w::husk_settle_keeps_carved_tilt(0.5f, 0.5f));
}

} // namespace

int main() {
	test_husk_render_pick_is_husk_first_then_huskfinal();
	test_dynamic_identity_needs_a_wire_handle_zero_bms_and_the_sentinel_origin();
	test_identity_key_spells_wire_or_bms_origin_parts();
	test_settle_keeps_the_carved_tilt_only_for_yaw_only_poses();
	if (failures != 0) {
		std::printf("present_passes_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("present_passes_test passed\n");
	return 0;
}
