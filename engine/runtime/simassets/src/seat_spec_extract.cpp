// The seat-spec extraction — the shell's GDScript walk moved verbatim onto
// the retained def rows + the sim's parse cache (ADR 0028), composed with the
// same slot/clamp validations the Dictionary ingest applied, so the emitted
// typed table is the exact production composition.
#include "simassets/seat_spec_extract.h"

#include <io/strutil.h>
#include <mission/mission.h> // kItemIdOffset (items.def id <-> wire type id)
#include <threedi/threedi_3di3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <unordered_set>

namespace opennova::simassets {

namespace {

constexpr double kRadToDeg = 180.0 / 3.14159265358979323846;

std::string trimmed(const char *name) {
	std::string out(name != nullptr ? name : "");
	const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
	while (!out.empty() && is_space(static_cast<unsigned char>(out.front())))
		out.erase(out.begin());
	while (!out.empty() && is_space(static_cast<unsigned char>(out.back())))
		out.pop_back();
	return out;
}

// Entity_GetBoneSlotType performs a case-insensitive comparison at byte zero
// of the model's USRP row name; whitespace trimming is reimpl-side hygiene,
// and embedded tokens are not seats.
// [orig: strnicmp(name, "sitex"/"ctrlx"/"UseGun"/"drvrx", 5/6) @ 0x434ED0]
std::string canonical_seat_name(const char *name) {
	return strutil::to_lower(trimmed(name));
}

bool begins_with(const std::string &value, const char *prefix) {
	const size_t n = std::strlen(prefix);
	return value.size() >= n && value.compare(0, n, prefix) == 0;
}

world::SeatType seat_type_for_user_point(const std::string &canonical) {
	if (begins_with(canonical, "sitex")) return world::SeatType::Passenger;
	if (begins_with(canonical, "ctrlx")) return world::SeatType::Controller;
	if (begins_with(canonical, "usegun")) return world::SeatType::Gunner;
	if (begins_with(canonical, "drvrx")) return world::SeatType::Driver;
	return world::SeatType::None;
}

// sitexNN / ctrlxNN / drvrxNN select the numbered sit pose (0..30); UseGun
// always poses 0. Leading ASCII digits only, breaking at the first non-digit.
int seat_pose_index_for_user_point(const std::string &canonical) {
	if (!(begins_with(canonical, "sitex") || begins_with(canonical, "ctrlx") ||
			begins_with(canonical, "drvrx")))
		return 0;
	int value = 0;
	bool any = false;
	for (size_t i = 5; i < canonical.size(); ++i) {
		const char c = canonical[i];
		if (c < '0' || c > '9') break;
		any = true;
		value = value * 10 + (c - '0');
		if (value > 1000) break; // the clamp below caps anyway
	}
	if (!any) return 0;
	return std::clamp(value, 0, 30);
}

bool item_has_runtime_metadata(const mission::ItemSeatSpec &spec) {
	return spec.mount_config_valid || !spec.seats.empty() ||
			!spec.armory_points.empty() || !spec.primary_weapon.empty() ||
			!spec.emplacement_attachments.empty();
}

const DefItemDef *find_item(const DefItemsFile &items, int item_id) {
	// Last-wins over duplicate definition ids — the same load-order overwrite
	// the id-keyed item map exposed (see simassets item_traits).
	const DefItemDef *found = nullptr;
	for (size_t i = 0; i < items.count; ++i)
		if (items.entries[i].id == item_id) found = &items.entries[i];
	return found;
}

// graphic -> "<basename>.3di" is SimModelCache's own name rule; the extractor
// only needs the graphic key.
} // namespace

// The authored 16.16 model point into the mission-local seat frame. The shell
// chain was: decode swizzle (-y, z, x)/65536 -> the render X-mirror ->
// the vehicle yaw-zero basis (RotY 180) -> godot_to_bms (x, -z, y); the
// composition collapses to (-y, x, z)/65536 over the RAW authored ints.
world::Vec3 seat_local_from_user_point(const ThreediUserPoint &point) {
	return world::Vec3{
			static_cast<float>(point.y) / -65536.0f,
			static_cast<float>(point.x) / 65536.0f,
			static_cast<float>(point.z) / 65536.0f};
}

// The authored local-Z direction into the seat yaw offset (degrees): the same
// chain over (rot_x, rot_y, rot_z) collapses to atan2(-rot_y, rot_x), with
// the shell's two degeneracy guards (a near-zero direction, then near-zero
// planar components after normalization).
int seat_yaw_offset_from_user_point(const ThreediUserPoint &point) {
	const double gx = static_cast<double>(point.rot_y) / 65536.0;
	const double gy = static_cast<double>(point.rot_z) / 65536.0;
	const double gz = static_cast<double>(point.rot_x) / 65536.0;
	const double len_sq = gx * gx + gy * gy + gz * gz;
	if (len_sq < 1.0e-6) return 0;
	const double len = std::sqrt(len_sq);
	const double local_x = -gx / len;
	const double local_y = gz / len;
	if (std::fabs(local_x) < 1.0e-6 && std::fabs(local_y) < 1.0e-6) return 0;
	return static_cast<int>(
			std::round(std::atan2(local_x, local_y) * kRadToDeg));
}

namespace {

void extract_seats(const Threedi3di3 &model,
		std::vector<world::Seat> &r_seats) {
	// Retail keeps the gameplay/userpoint list and the ten occupant handles in
	// different layouts: sitex rows fill 0..7, ctrlx/drvrx share 8, UseGun is
	// 9. Carry the fixed slot beside the dense gameplay record, first-claim
	// wins (the ingest's used-slot rule).
	// [orig: ItemDef seatBoneIndex/controlBone/useGunBone +0x25D..+0x266]
	int passenger_slot = 0;
	bool retail_slots_used[10] = {};
	for (size_t i = 0; model.user_points != nullptr &&
			i < model.user_point_count; ++i) {
		const ThreediUserPoint &up = model.user_points[i];
		const std::string canonical = canonical_seat_name(up.name);
		const world::SeatType type = seat_type_for_user_point(canonical);
		if (type == world::SeatType::None) continue;
		int retail_slot = -1;
		switch (type) {
			case world::SeatType::Passenger:
				if (passenger_slot < 8) retail_slot = passenger_slot;
				++passenger_slot;
				break;
			case world::SeatType::Controller:
			case world::SeatType::Driver:
				retail_slot = 8;
				break;
			case world::SeatType::Gunner:
				retail_slot = 9;
				break;
			default:
				break;
		}
		world::Seat seat;
		seat.type = type;
		if (retail_slot >= 0 && retail_slot < 10 &&
				!retail_slots_used[retail_slot]) {
			seat.retail_slot = static_cast<uint8_t>(retail_slot);
			retail_slots_used[retail_slot] = true;
		}
		seat.bone_index = static_cast<uint8_t>(
				std::clamp(static_cast<int>(i) + 1, 0, 255));
		seat.pose_index = static_cast<uint8_t>(
				seat_pose_index_for_user_point(canonical));
		seat.source_name = up.name;
		seat.seat_local = seat_local_from_user_point(up);
		seat.yaw_offset = static_cast<int16_t>(std::clamp(
				seat_yaw_offset_from_user_point(up), -32768, 32767));
		r_seats.push_back(seat);
	}
}

void extract_armory_points(const Threedi3di3 &model,
		std::vector<world::Vec3> &r_points) {
	// The "armory*" userpoint locals (prefix match) — the floating
	// armory-label anchors on Armory-attrib items.
	// [orig: strnicmp(name, "armory", 6) @ 0x436226/@ 0x5a372b]
	for (size_t i = 0; model.user_points != nullptr &&
			i < model.user_point_count; ++i) {
		const ThreediUserPoint &up = model.user_points[i];
		if (!begins_with(canonical_seat_name(up.name), "armory")) continue;
		r_points.push_back(seat_local_from_user_point(up));
	}
}

void extract_attachments(const DefItemDef &def, const Threedi3di3 *model,
		std::vector<mission::ItemEmplacementAttachmentSpec> &r_attachments) {
	for (size_t i = 0; i < def.emplacement_attachments_count &&
			def.emplacement_attachments != nullptr; ++i) {
		const DefItemEmplacementAttachment &row =
				def.emplacement_attachments[i];
		const int child_type_id = row.item_id - mission::kItemIdOffset;
		if (child_type_id <= 0) continue;
		mission::ItemEmplacementAttachmentSpec spec;
		spec.child_type_id = child_type_id;
		spec.kind = static_cast<mission::EmplacementAttachmentKind>(
				std::clamp(row.kind, 0, 2));
		spec.stored_slot = static_cast<uint8_t>(
				std::clamp(static_cast<int>(i) + 1, 0, 4));
		// The last authored G/C record owns its designation slot.
		if (static_cast<int>(i) + 1 == def.emplacement_c_slot)
			spec.attachment_flags |= 1;
		if (static_cast<int>(i) + 1 == def.emplacement_g_slot)
			spec.attachment_flags |= 2;
		spec.anchor.type = world::SeatType::Gunner;
		spec.anchor.attachment_frame = true;
		spec.anchor.source_name = row.userpoint;
		spec.angle_count = row.angle_count == 4 ? 4 : 0;
		spec.down_limit_bam = row.down_angle;
		spec.up_limit_bam = row.up_angle;
		spec.right_limit_bam = row.right_angle;
		spec.left_limit_bam = row.left_angle;
		// The authored anchor resolves its model userpoint case-insensitively
		// (whole name), first match; a missing anchor copies the parent root
		// (bone 0, zero local). [orig: docs/world/itemdef-re.md
		// §child-emplacements; build_bone_attachment_matrix @ 0x56C630]
		if (model != nullptr && model->user_points != nullptr) {
			const std::string wanted = trimmed(row.userpoint);
			for (size_t u = 0; u < model->user_point_count; ++u) {
				const ThreediUserPoint &up = model->user_points[u];
				if (!strutil::iequals(trimmed(up.name), wanted)) continue;
				spec.anchor_found = true;
				spec.anchor.bone_index = static_cast<uint8_t>(
						std::clamp(static_cast<int>(u) + 1, 0, 255));
				spec.anchor.source_name = up.name;
				spec.anchor.seat_local = seat_local_from_user_point(up);
				spec.anchor.yaw_offset = static_cast<int16_t>(std::clamp(
						seat_yaw_offset_from_user_point(up), -32768, 32767));
				break;
			}
		}
		r_attachments.push_back(std::move(spec));
	}
}

} // namespace

void extract_item_seat_specs(const DefItemsFile &items,
                             const ModelLookupFn &model_for,
                             const std::vector<int> &seed_item_ids,
                             SeatSpecExtraction &out) {
	out.specs.clear();
	out.graphic_by_type.clear();
	std::deque<int> pending(seed_item_ids.begin(), seed_item_ids.end());
	std::unordered_set<int32_t> seen_types;
	while (!pending.empty()) {
		const int item_id = pending.front();
		pending.pop_front();
		const int32_t type_id = item_id - mission::kItemIdOffset;
		if (item_id == 0 || type_id <= 0 || seen_types.count(type_id) != 0)
			continue;
		seen_types.insert(type_id);
		const DefItemDef *def = find_item(items, item_id);
		if (def == nullptr) continue; // item_not_found
		mission::ItemSeatSpec spec;
		spec.type_id = type_id;
		// Mounted gunner overlay selection reads the TARGET item definition's
		// phrase_set dword at +0x86c; presence is carried independently
		// because zero is a witnessed retail configuration.
		// [orig: ItemDef_ParseProperty @ 0x49f9db..0x49fa0a; consumer @ 0x4b1884]
		spec.mount_config_valid = def->phrase_set_valid != 0;
		spec.mount_config = spec.mount_config_valid ? def->phrase_set : 0;
		spec.primary_weapon = def->primary_weapon;
		const Threedi3di3 *model = nullptr;
		const std::string graphic(def->graphic);
		if (!graphic.empty() && model_for) {
			model = model_for(graphic);
		}
		// Authored attachment rows survive a missing model (parent-root
		// anchors); seats and armory locals need the model userpoints.
		extract_attachments(*def, model, spec.emplacement_attachments);
		if (model != nullptr) {
			extract_seats(*model, spec.seats);
			// "armory*" userpoints label/scan only on Armory-attrib items —
			// the same attrib gate the original applies before its walk
			// [orig: itemDef->attrib & 0x80000 @ 0x4361ee/@ 0x5a36f5].
			if ((def->attrib & DEF_ITEM_ATTRIB_ARMORY) != 0)
				extract_armory_points(*model, spec.armory_points);
		}
		for (const mission::ItemEmplacementAttachmentSpec &attachment :
				spec.emplacement_attachments) {
			const int child_item_id =
					attachment.child_type_id + mission::kItemIdOffset;
			if (attachment.child_type_id > 0 &&
					seen_types.count(attachment.child_type_id) == 0)
				pending.push_back(child_item_id);
		}
		if (!item_has_runtime_metadata(spec)) continue;
		if (model != nullptr) out.graphic_by_type[type_id] = graphic;
		out.specs.push_back(std::move(spec));
	}
	std::sort(out.specs.begin(), out.specs.end(),
			[](const mission::ItemSeatSpec &a, const mission::ItemSeatSpec &b) {
				return a.type_id < b.type_id;
			});
}

} // namespace opennova::simassets
