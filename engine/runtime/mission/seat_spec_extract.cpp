// The seat-spec extraction — the shell's GDScript walk moved verbatim onto
// the retained def rows + the shared asset store (ADR 0028, ADR 0044), composed with the
// same slot/clamp validations the Dictionary ingest applied, so the emitted
// typed table is the exact production composition.
#include <runtime/mission/seat_spec_extract.h>
#include <runtime/mission/collision_resolve.h> // find_item_def

#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <runtime/world/turret_window.h> // turret_window_limit_bam
#include <runtime/world/world.h>
#include <formats/mission/mission.h> // kItemIdOffset (items.def id <-> wire type id)
#include <formats/threedi/threedi_3di3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_set>
#include <base/io/bam.h>

using namespace opennova::def;
using namespace opennova::threedi;

namespace opennova::mission {

namespace {

constexpr double kRadToDeg = io::kDegreesPerRadian;

std::string trimmed(const char *name) {
	// strutil::trim spells its whitespace set out so canonicalization never
	// depends on the process locale (a hand-rolled std::isspace loop here
	// once could classify extra bytes under non-C locales).
	return strutil::trim(name != nullptr ? name : "");
}

// A userpoint's raw USRP name: every seat and armory compare reads it as
// authored, from byte zero, with no trim.
std::string user_point_name(const ThreediUserPoint &point) {
	return strutil::fixed_string(point.name, sizeof(point.name));
}

// sitexNN / ctrlxNN / drvrxNN select the numbered sit pose (0..30); UseGun
// always poses 0. Leading ASCII digits only, breaking at the first non-digit.
int seat_pose_index_for_user_point(const std::string &name) {
	if (!(threedi_user_point_is_sitex(name) ||
			strutil::starts_with_icase(name, "ctrlx") ||
			strutil::starts_with_icase(name, "drvrx")))
		return 0;
	int value = 0;
	bool any = false;
	for (size_t i = 5; i < name.size(); ++i) {
		const char c = name[i];
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

// graphic -> "<basename>.3di" is assets::AssetStore's own name rule; the extractor
// only needs the graphic key.
} // namespace

// sitex, ctrlx and drvrx are 5-character case-insensitive prefixes at byte
// zero, UseGun a case-insensitive whole-name compare, so embedded tokens and
// suffixed UseGun names are not seats. [orig: Entity_GetBoneSlotType
//  @ 0x434ED0 — the strnicmp legs @ 0x434F16 / @ 0x434F34 / @ 0x434F52, the
//  _stricmp @ 0x434F6E]
world::SeatType seat_type_for_user_point(std::string_view name) {
	if (threedi_user_point_is_sitex(name)) return world::SeatType::Passenger;
	if (strutil::starts_with_icase(name, "ctrlx")) return world::SeatType::Controller;
	if (strutil::starts_with_icase(name, "drvrx")) return world::SeatType::Driver;
	if (strutil::iequals(name, "UseGun")) return world::SeatType::Gunner;
	return world::SeatType::None;
}

uint32_t user_point_uses(std::string_view name) {
	uint32_t uses = 0;
	for (const UserPointName &row : kUserPointNames)
		if (row.prefix ? strutil::starts_with_icase(name, row.name) : strutil::iequals(name, row.name))
			uses |= row.uses;
	return uses;
}

// The authored 16.16 model point into the mission-local seat frame. The shell
// chain was: decode swizzle (-y, z, x)/65536 -> the render X-mirror ->
// the vehicle yaw-zero basis (RotY 180) -> godot_to_bms (x, -z, y); the
// composition collapses to (-y, x, z)/65536 over the RAW authored ints.
world::Vec3 seat_local_from_user_point(const ThreediUserPoint &point) {
	return world::Vec3{
			static_cast<float>(point.y) / -io::kFp16One,
			static_cast<float>(point.x) / io::kFp16One,
			static_cast<float>(point.z) / io::kFp16One};
}

// The authored local-Z direction into the seat yaw offset (degrees): the same
// chain over (rot_x, rot_y, rot_z) collapses to atan2(-rot_y, rot_x), with
// the shell's two degeneracy guards (a near-zero direction, then near-zero
// planar components after normalization).
int seat_yaw_offset_from_user_point(const ThreediUserPoint &point) {
	const double gx = static_cast<double>(point.rot_y) / io::kFp16OneD;
	const double gy = static_cast<double>(point.rot_z) / io::kFp16OneD;
	const double gz = static_cast<double>(point.rot_x) / io::kFp16OneD;
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
	// The load-time resolve binds one userpoint row (1-based) to each of the
	// ten occupant slots: sitex rows fill 0..7 in order and a ninth writes
	// slot 8 and ends the scan; ctrlx and drvrx share slot 8 and UseGun takes
	// 9, the last match winning. The runtime reads only these bones, so they
	// alone are seats, emitted in userpoint order beside their fixed slot.
	// [orig: EntityDef_LoadModelsAndCallbacks @ 0x439F50 — the ItemDef
	//  seatBoneIndex/controlBone/useGunBone +0x25D..+0x266 walk
	//  @ 0x43A47B..0x43A5CD, the scan end `cmp ebp, 8; jg` @ 0x43A5AF;
	//  the slot consumers Entity_FindNearestSeatOrArmory @ 0x435F37 and
	//  Entity_FindAvailableSeat @ 0x436835]
	std::array<size_t, 10> row_for_slot{}; // userpoint index + 1, 0 = none
	int passengers = 0;
	for (size_t i = 0; model.user_points != nullptr &&
			i < model.user_point_count; ++i) {
		const std::string name = user_point_name(model.user_points[i]);
		if (threedi_user_point_is_sitex(name)) {
			row_for_slot[static_cast<size_t>(passengers)] = i + 1; // [orig: @ 0x43A4F0]
			++passengers;
		} else if (strutil::starts_with_icase(name, "ctrlx") ||
				strutil::starts_with_icase(name, "drvrx")) {
			row_for_slot[8] = i + 1; // [orig: @ 0x43A532 / @ 0x43A570]
		} else if (strutil::iequals(name, "UseGun")) {
			row_for_slot[9] = i + 1; // [orig: @ 0x43A5A9]
		}
		if (passengers > THREEDI_SITEX_SEAT_LIMIT) break;
	}
	for (size_t i = 0; model.user_points != nullptr &&
			i < model.user_point_count; ++i) {
		size_t retail_slot = row_for_slot.size();
		for (size_t slot = 0; slot < row_for_slot.size(); ++slot)
			if (row_for_slot[slot] == i + 1) retail_slot = slot;
		if (retail_slot == row_for_slot.size()) continue;
		const ThreediUserPoint &up = model.user_points[i];
		const std::string name = user_point_name(up);
		world::Seat seat;
		seat.type = seat_type_for_user_point(name);
		seat.retail_slot = static_cast<uint8_t>(retail_slot);
		seat.bone_index = static_cast<uint8_t>(
				std::clamp(static_cast<int>(i) + 1, 0, 255));
		seat.pose_index = static_cast<uint8_t>(
				seat_pose_index_for_user_point(name));
		seat.source_name = name;
		seat.seat_local = seat_local_from_user_point(up);
		seat.yaw_offset = static_cast<int16_t>(std::clamp<int>(
				seat_yaw_offset_from_user_point(up),
				std::numeric_limits<int16_t>::min(),
				std::numeric_limits<int16_t>::max()));
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
		if (!strutil::starts_with_icase(user_point_name(up), "armory")) continue;
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
		// §child-emplacements; Bone_BuildAttachmentMatrix @ 0x56C630]
		if (model != nullptr && model->user_points != nullptr) {
			const std::string wanted = trimmed(row.userpoint);
			for (size_t u = 0; u < model->user_point_count; ++u) {
				const ThreediUserPoint &up = model->user_points[u];
				if (!strutil::iequals(trimmed(up.name), wanted)) continue;
				spec.anchor_found = true;
				spec.anchor.bone_index = static_cast<uint8_t>(
						std::clamp(static_cast<int>(u) + 1, 0, 255));
				spec.anchor_subobject = static_cast<int16_t>(up.subobject_index);
				spec.anchor.source_name = up.name;
				spec.anchor.seat_local = seat_local_from_user_point(up);
				spec.anchor.yaw_offset = static_cast<int16_t>(std::clamp<int>(
						seat_yaw_offset_from_user_point(up),
						std::numeric_limits<int16_t>::min(),
						std::numeric_limits<int16_t>::max()));
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
		const DefItemDef *def = find_item_def(items, item_id);
		if (def == nullptr) continue; // item_not_found
		mission::ItemSeatSpec spec;
		spec.type_id = type_id;
		spec.item_attrib2 = def->attrib2;
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

void stamp_seat_spec_turret_limits(world::World &world,
		std::vector<mission::ItemSeatSpec> &specs) {
	for (mission::ItemSeatSpec &spec : specs) {
		if (spec.primary_weapon.empty()) continue;
		const int index = world.tables.weapons.index_of(spec.primary_weapon.c_str());
		const world::WeaponTableEntry *entry =
				index >= 0 && index <= 0xFF
						? world.tables.weapons.by_index(static_cast<uint8_t>(index))
						: nullptr;
		if (entry == nullptr) continue;
		spec.turret_limits_valid = true;
		spec.turret_yaw_range_bam =
				world::turret_window_limit_bam(entry->turret_yaw_range_deg);
		spec.turret_pitch_max_bam =
				world::turret_window_limit_bam(entry->turret_pitch_max_deg);
		spec.turret_pitch_min_bam =
				world::turret_window_limit_bam(entry->turret_pitch_min_deg);
	}
}

void stamp_minus_one_slot_window(world::Entity &child, float carrier_light_transfer,
		int32_t slot4_down, int32_t slot4_up, int32_t slot4_right) {
	int32_t light_transfer_bits = 0;
	std::memcpy(&light_transfer_bits, &carrier_light_transfer, sizeof(light_transfer_bits));
	child.emplacement_down_limit_bam = light_transfer_bits; // +0x218
	child.emplacement_up_limit_bam = slot4_down;            // +0x228
	child.emplacement_right_limit_bam = slot4_up;           // +0x238
	child.emplacement_left_limit_bam = slot4_right;         // +0x248
}

void refresh_item_seat_spec(world::World &world,
		const std::vector<mission::ItemSeatSpec> &specs,
		world::Entity &p_entity, bool p_wire_header_world) {
	std::array<world::EntityHandle, 10> occupants{};
	for (const world::Seat &seat : p_entity.seats) {
		if (seat.retail_slot < occupants.size())
			occupants[seat.retail_slot] = seat.occupant;
	}

	// A promoted child (authority or complete-BMS joiner) already owns the exact
	// stored addeweap slot; keep that identity across definition refreshes even
	// when sibling types repeat. A streamed 0x0D row names its slot by its
	// subType (field 0x80, the slot index the spawn stamped; absent = slot 0),
	// which is the key the ewep class init reads the carrier def's slot anchor
	// by, on every peer at mission start.
	// [orig: Entity_SpawnWeaponOverlays subType = slot @0x40F40E;
	//  Entity_InitBoneReferences @0x4415E1..0x4415FF; Game_StartMission ->
	//  Entity_InitAllFromModels @0x52567F]
	// Clear first so a later definition refresh cannot leave stale pose/capability
	// metadata on an existing row.
	const bool preserve_authored_slot = p_wire_header_world ||
			(p_entity.emplacement_pose_metadata_resolved &&
			 p_entity.emplacement_slot != 0);
	const uint8_t authored_slot = p_wire_header_world
			? static_cast<uint8_t>(p_entity.sub_type + 1u)
			: p_entity.emplacement_slot;
	p_entity.emplacement_pose_metadata_resolved = false;
	p_entity.emplacement_local = {};
	p_entity.emplacement_yaw_offset = 0;
	p_entity.emplacement_bone = 0;
	p_entity.emplacement_kind = 0;
	p_entity.emplacement_slot = 0;
	p_entity.emplacement_attachment_flags = 0;
	p_entity.emplacement_angle_count = 0;
	p_entity.emplacement_down_limit_bam = 0;
	p_entity.emplacement_up_limit_bam = 0;
	p_entity.emplacement_right_limit_bam = 0;
	p_entity.emplacement_left_limit_bam = 0;
	if (p_entity.emplacement_parent.valid() &&
			p_entity.emplacement_parent_spawn_id != 0) {
		const world::Entity *parent =
				world.registry.get(p_entity.emplacement_parent);
		if (parent != nullptr && parent->registry_spawn_id ==
					p_entity.emplacement_parent_spawn_id &&
				p_entity.sub_type == 0xFF) {
			// An hp-0 child def's init leaves subType 0xFF before the class init
			// reads it: the anchor read at subType -1 names no userpoint, so on
			// every peer the child rides its carrier's root, with no anchor
			// offset and no designation slot.
			// [orig: Entity_InitFromModel subType = -1 @0x40DCAF ahead of
			//  Entity_InitBoneReferences @0x4415F1..0x44164A]
			p_entity.emplacement_anchor_subobject = -1;
			p_entity.emplacement_pose_metadata_resolved = true;
			// Its window reads the carrier def's tables at -1 as well.
			const mission::ItemSeatSpec *parent_spec =
					item_seat_spec_for_type(specs, static_cast<uint16_t>(parent->item_id));
			const mission::ItemEmplacementAttachmentSpec *slot4 = nullptr;
			if (parent_spec != nullptr) {
				for (const mission::ItemEmplacementAttachmentSpec &attachment :
						parent_spec->emplacement_attachments)
					if (attachment.stored_slot == 4) slot4 = &attachment;
			}
			stamp_minus_one_slot_window(p_entity, parent->light_transfer,
					slot4 != nullptr ? slot4->down_limit_bam : 0,
					slot4 != nullptr ? slot4->up_limit_bam : 0,
					slot4 != nullptr ? slot4->right_limit_bam : 0);
		} else if (parent != nullptr && parent->registry_spawn_id ==
					p_entity.emplacement_parent_spawn_id) {
			const mission::ItemSeatSpec *parent_spec =
					item_seat_spec_for_type(specs,
							static_cast<uint16_t>(parent->item_id));
			const mission::ItemEmplacementAttachmentSpec *match = nullptr;
			bool ambiguous = false;
			if (parent_spec != nullptr) {
				for (const mission::ItemEmplacementAttachmentSpec &attachment :
						parent_spec->emplacement_attachments) {
					if (attachment.child_type_id != p_entity.item_id) continue;
					if (preserve_authored_slot &&
							attachment.stored_slot != authored_slot)
						continue;
					if (match != nullptr) {
						ambiguous = true;
						break;
					}
					match = &attachment;
				}
			}
			if (match != nullptr && !ambiguous) {
				p_entity.emplacement_local = match->anchor.seat_local;
				p_entity.emplacement_yaw_offset = match->anchor.yaw_offset;
				p_entity.emplacement_bone =
						match->anchor_found ? match->anchor.bone_index : 0;
				p_entity.emplacement_anchor_subobject =
						match->anchor_found ? match->anchor_subobject : int16_t{-1};
				p_entity.emplacement_kind = static_cast<uint8_t>(match->kind);
				p_entity.emplacement_slot = match->stored_slot;
				p_entity.emplacement_attachment_flags = match->attachment_flags;
				p_entity.emplacement_angle_count = match->angle_count;
				p_entity.emplacement_down_limit_bam = match->down_limit_bam;
				p_entity.emplacement_up_limit_bam = match->up_limit_bam;
				p_entity.emplacement_right_limit_bam = match->right_limit_bam;
				p_entity.emplacement_left_limit_bam = match->left_limit_bam;
				p_entity.emplacement_pose_metadata_resolved = true;
			}
		}
	}

	const mission::ItemSeatSpec *spec =
			item_seat_spec_for_type(specs,
					static_cast<uint16_t>(p_entity.item_id));
	// The installed table is authoritative. Clearing a type from a later table
	// must also clear stale model metadata on an already-streamed exact row.
	p_entity.emplaced_config_valid = false;
	p_entity.emplaced_config = 0;
	p_entity.armory_points.clear();
	p_entity.primary_weapon.clear();
	p_entity.seats.clear();
	if (spec == nullptr) return;

	p_entity.emplaced_config_valid = spec->mount_config_valid;
	p_entity.emplaced_config = spec->mount_config_valid
			? spec->mount_config : 0;
	p_entity.armory_points = spec->armory_points;
	p_entity.primary_weapon = spec->primary_weapon;
	// The item-definition traits (has_item_def, item_attrib) come only from
	// the items.def sweep; a seat spec never stands in for a definition row.
	p_entity.seats = spec->seats;
	for (size_t seat_index = 0; seat_index < p_entity.seats.size();
			++seat_index) {
		world::Seat &seat = p_entity.seats[seat_index];
		seat.occupant = seat.retail_slot < occupants.size()
				? occupants[seat.retail_slot]
				: world::EntityHandle{};
		if (!seat.occupant.valid()) continue;
		world::Entity *occupant = world.registry.get(seat.occupant);
		if (occupant == nullptr || !occupant->mounted ||
				occupant->mount_target != p_entity.handle)
			continue;
		// mount_seat is the dense gameplay-row index, while occupancy survives
		// table refreshes by retail's fixed mountHandles slot. Keep the occupant
		// side synchronized when a later model table changes dense ordering.
		occupant->mount_seat = static_cast<int8_t>(seat_index);
		occupant->mount_type = seat.type;
		occupant->mount_bone = seat.bone_index;
		occupant->mounted_config_valid = p_entity.emplaced_config_valid;
		occupant->mounted_config = p_entity.emplaced_config_valid
				? p_entity.emplaced_config : 0;
	}
}

} // namespace opennova::mission
