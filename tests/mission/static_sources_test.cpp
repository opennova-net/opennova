#include <runtime/mission/static_sources.h>
#include <runtime/mission/placement_traits.h>

#include <cstdio>
#include <unordered_map>

using namespace opennova::mission;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static StaticSourceTransform at(float x, float y, float z) {
	auto out = kStaticSourceIdentity;
	out[9] = x; out[10] = y; out[11] = z;
	return out;
}

static StaticInstance instance(int bms, int index, bool casts = true) {
	StaticInstance out;
	out.bms_id = bms;
	out.index = index;
	out.graphic = "house";
	out.batch_key = "house:reflective";
	out.xform = at(12, 34, -56);
	out.casts_static_shadow = casts;
	out.mirror_reflected = true;
	out.lod_instance = 4;
	return out;
}

static uint64_t resolve(const std::string &graphic) {
	if (graphic == "house") return 10;
	if (graphic == "husk") return 20;
	if (graphic == "damaged") return 30;
	return 0;
}

static void test_manual_shadow_lifecycle() {
	StaticSources sources;
	sources.register_instance(instance(100, 0), true);
	const auto placed = sources.shadow_sources(resolve);
	CHECK(placed.size() == 1);
	CHECK(placed[0].bms_id == 100 && placed[0].entity_kind == kEntityKindBuilding);
	CHECK(placed[0].entity_index == 0 && placed[0].asset_id == 10);
	CHECK(placed[0].graphic == "house" && placed[0].world_transform == at(12, 34, -56));
	CHECK(placed[0].active);
	CHECK(sources.instance(100)->batch_key == "house:reflective");
	CHECK(sources.instance(100)->lod_instance == 4 && sources.instance(100)->mirror_reflected);
	auto revision = sources.shadow_revision();
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 0, at(-4, 8, 16)));
	CHECK(sources.shadow_revision() > revision);
	revision = sources.shadow_revision();
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 0, at(-4, 8, 16)));
	CHECK(sources.shadow_revision() == revision);
	CHECK(sources.shadow_sources(resolve)[0].world_transform == at(-4, 8, 16));
	CHECK(sources.hide_instance(100));
	CHECK(sources.shadow_revision() > revision && !sources.shadow_sources(resolve)[0].active);
	revision = sources.shadow_revision();
	CHECK(!sources.hide_instance(100) && sources.shadow_revision() == revision);
	CHECK(sources.show_instance(100) && sources.shadow_revision() > revision);
	CHECK(sources.shadow_sources(resolve)[0].active);
	CHECK(!sources.hide_instance(999) && !sources.show_instance(999));
	CHECK(!sources.accepts_replacement(0, "husk"));
	CHECK(!sources.accepts_replacement(999, "husk"));
	CHECK(!sources.accepts_replacement(100, ""));
	CHECK(sources.set_shadow_replacement(100, "husk", at(7, 9, 11), true, 20));
	auto rows = sources.shadow_sources(resolve);
	CHECK(rows[0].graphic == "husk" && rows[0].asset_id == 20 && rows[0].active);
	CHECK(rows[0].world_transform == at(7, 9, 11));
	revision = sources.shadow_revision();
	CHECK(sources.set_shadow_replacement(100, "husk", at(7, 9, 11), true, 20));
	CHECK(sources.shadow_revision() == revision);
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 0, at(8, 9, 11)));
	CHECK(sources.shadow_revision() > revision);
	CHECK(sources.shadow_sources(resolve)[0].world_transform == at(8, 9, 11));
	revision = sources.shadow_revision();
	CHECK(sources.set_shadow_replacement(100, "damaged", at(8, 9, 11), true, 30));
	CHECK(sources.shadow_revision() > revision);
	revision = sources.shadow_revision();
	CHECK(sources.set_shadow_replacement(100, "damaged", at(8, 9, 11), false, 30));
	CHECK(sources.shadow_revision() > revision);
	revision = sources.shadow_revision();
	CHECK(sources.set_shadow_replacement(100, "damaged", at(8, 9, 11), false, 30));
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 0, at(8, 10, 11)));
	CHECK(sources.shadow_revision() == revision);
	CHECK(sources.shadow_sources(resolve)[0].world_transform == at(8, 10, 11));
	CHECK(sources.set_shadow_replacement(100, "damaged", at(8, 10, 11), true, 30));
	CHECK(sources.shadow_revision() > revision);
	revision = sources.shadow_revision();
	CHECK(sources.clear_shadow_replacement(100));
	CHECK(sources.shadow_revision() > revision);
	CHECK(sources.shadow_sources(resolve)[0].graphic == "house");
	CHECK(!sources.clear_shadow_replacement(100));
	CHECK(!sources.set_shadow_replacement(100, "missing", at(0, 0, 0), true, 0));
	CHECK(!sources.shadow_sources(resolve)[0].active);
}

static void test_placed_order_policy_and_epoch() {
	StaticSources sources;
	StaticTerrainShadowSource row;
	row.entity_kind = kEntityKindBuilding;
	row.entity_index = 7;
	row.bms_id = 41;
	row.item_id = 100321;
	row.team = 1;
	row.graphic = "house";
	row.asset_id = 99;
	row.world_transform = at(1, 2, 3);
	sources.record_shadow(row);
	row.bms_id = 42;
	row.entity_index = 8;
	row.team = 2;
	row.item_attrib = kItemAttribNoShadow;
	sources.record_shadow(row);
	row.bms_id = 43;
	row.entity_kind = kEntityKindOrganic;
	sources.record_shadow(row);
	row.entity_kind = kEntityKindItem;
	row.item_attrib = 0;
	row.item_attrib2 = kItemAttrib2StaticShadow;
	sources.record_shadow(row);
	auto revision = sources.shadow_revision();
	sources.register_instance(instance(41, 7), false);
	CHECK(sources.shadow_revision() == revision);
	sources.register_instance(instance(50, 9), true);
	sources.register_instance(instance(49, 10), true);
	sources.register_instance(instance(50, 9), true);
	auto rows = sources.shadow_sources(resolve);
	CHECK(rows.size() == 5);
	CHECK(rows[0].bms_id == 41 && rows[1].bms_id == 42 && rows[2].bms_id == 43);
	CHECK(rows[3].bms_id == 50 && rows[4].bms_id == 49);
	CHECK(rows[0].team == 1 && rows[1].team == 2);
	CHECK(rows[0].item_id == 100321 && rows[0].asset_id == 99);
	CHECK(rows[0].world_transform == at(12, 34, -56));
	CHECK(rows[1].item_attrib == kItemAttribNoShadow);
	CHECK(rows[2].item_attrib2 == kItemAttrib2StaticShadow);
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 7, at(5, 6, 7)));
	CHECK(sources.instance(41)->xform == at(5, 6, 7));
	CHECK(!sources.update_shadow_transform(kEntityKindOrganic, 7, at(0, 0, 0)));
	CHECK(sources.set_shadow_replacement(41, "husk", at(6, 7, 8), true, 20));
	CHECK(sources.hide_instance(41));
	CHECK(sources.shadow_sources(resolve)[0].active); // the live husk replaces the carved source
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 7, at(7, 8, 9)));
	CHECK(sources.shadow_sources(resolve)[0].world_transform == at(7, 8, 9));
	sources.invalidate_shadow_assets();
	rows = sources.shadow_sources(resolve);
	CHECK(rows[0].asset_id == 10 && rows[0].graphic == "house" && !rows[0].active);
	CHECK(rows[1].asset_id == 10);
	CHECK(rows[0].world_transform == at(7, 8, 9));
	rows[0].graphic = "detached snapshot";
	CHECK(sources.shadow_sources(resolve)[0].graphic == "house");
}

static void test_unobservable_changes_and_live_draws() {
	StaticSources sources;
	sources.register_instance(instance(200, 3, false), true);
	auto revision = sources.shadow_revision();
	CHECK(sources.update_shadow_transform(kEntityKindBuilding, 3, at(1, 2, 3)));
	CHECK(sources.set_shadow_replacement(200, "husk", at(1, 2, 3), true, 20));
	CHECK(sources.clear_shadow_replacement(200));
	CHECK(sources.shadow_revision() == revision);
	StaticTerrainShadowSource rejected;
	rejected.bms_id = 201;
	rejected.entity_kind = kEntityKindItem;
	rejected.entity_index = 4;
	sources.record_shadow(rejected);
	revision = sources.shadow_revision();
	CHECK(sources.update_shadow_transform(kEntityKindItem, 4, at(3, 2, 1)));
	CHECK(sources.set_shadow_replacement(201, "husk", at(3, 2, 1), true, 20));
	CHECK(sources.clear_shadow_replacement(201));
	CHECK(sources.shadow_revision() == revision);
	StaticEffectSource effect;
	effect.kind = kEntityKindBuilding;
	effect.entity_index = 3;
	effect.bms_id = 200;
	effect.item_id = 101;
	effect.asset_id = 10;
	effect.entity_bound_radius_q16 = 65536;
	CHECK(sources.append_effect(effect) == 0);
	CHECK(sources.append_effect(effect) == 1);
	CHECK(sources.effect_sources()[1].source_index == 1);
	CHECK(sources.effect_sources()[0].entity_bound_radius_q16 == 65536);
	StaticLightDrawSource draw;
	draw.source_index = 0;
	draw.bms_id = 200;
	draw.robj_index = 4;
	draw.bounds_position = {1, 2, 3};
	draw.bounds_size = {4, 5, 6};
	CHECK(sources.append_light_draw(draw) == 0);
	draw.bms_id = 0;
	CHECK(sources.append_light_draw(draw) == 1);
	const auto light_revision = sources.light_revision();
	CHECK(sources.hide_instance(200));
	const auto lights = sources.light_draw_sources();
	CHECK(lights.size() == 2 && !lights[0].active && lights[1].active);
	CHECK(lights[0].atlas_row == 0 && lights[1].atlas_row == 1);
	CHECK(lights[0].robj_index == 4 && lights[0].bounds_position[1] == 2);
	CHECK(lights[0].bounds_size[2] == 6 && sources.light_revision() > light_revision);
	sources.invalidate_shadow_assets();
	CHECK(sources.effect_sources()[0].asset_id == 10); // epoch invalidation only releases shadow refs
	const auto before_clear = sources.shadow_revision();
	sources.clear();
	CHECK(sources.effect_sources().empty() && sources.light_draw_sources().empty());
	CHECK(sources.shadow_sources(resolve).empty() && sources.instance(200) == nullptr);
	CHECK(!sources.is_hidden(200) && sources.shadow_revision() > before_clear);
}

int main() {
	test_manual_shadow_lifecycle();
	test_placed_order_policy_and_epoch();
	test_unobservable_changes_and_live_draws();
	return failures ? 1 : 0;
}
