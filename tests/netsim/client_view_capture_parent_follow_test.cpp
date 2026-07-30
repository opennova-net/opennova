// The non-COOP "a vehicle follows me around" repro (one entity, varies per map).
//
// Witnessed trigger, straight from the retail<->retail AS capture
// (.scratch/golden/retail-vehicle-session.pcapng, "AS - Dormant Volcano Isle"
// ASH_I5A.BMS, g_GameType 0x10010): the S2C 0x0D pool-1 spawn (net-re §5.11) for
// "Drivable Dune Buggy" slot 0x1006 carries spawnFlags 0x1d77 — bit 0x0100 set —
// with parentHandle 0x0000 = pool-0 slot 0 = "Player #1 (Multiplayer)". Retail
// stores that handle at entity+368 [orig: NapiNPClientMsg_0x00D @0x432c40] and
// afterwards moves the vehicle ONLY through its per-frame §5.10b vehicle
// compacts, whose own parent_slot_handle (the carrier-local-coordinate parent)
// stays 0xFFFF for this vehicle throughout the capture. The organic parent is a
// back-reference, not a transform parent.
//
// The regression this pins: NetClientView latched the spawn-time parent and
// refresh_parented_pool_entities() rigid-recomposed the vehicle onto the
// PLAYER's row after every applied frame — gluing the buggy to the player for
// the whole session (the live symptom on non-COOP retail hosts; COOP world
// streams never set 0x0100, cf. §5.11's observed-bits note: 0x3EF7 only).
//
// Oracle: an independent raw decode of every S2C 0x0A frame — each tracked
// vehicle's anchor-relative compact position is the wire truth the view's row
// must match after apply. No committed derived oracle: env-gated on
// NW_GOLDEN_VEHICLE_SESSION (capture, DEFAULT_* fallback) + NW_ITEMS_DEF (the
// §5.10b class table); skips clean when either is absent.

#include <netsim/connection_fan.h>
#include <netsim/net_client_view.h>

#include <npwire/ingame_decode.h>
#include <npwire/ingame_message_id.h>
#include <npwire/wire_capture.h>

#include <def/def.h>
#include <scr/scr.h>

#include <pcapio/pcap_reader.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef DEFAULT_VEHICLE_SESSION_PCAP
#define DEFAULT_VEHICLE_SESSION_PCAP ""
#endif

namespace {

using namespace opennova;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// items.def wire_id -> §5.10b dispatch class, the same table nw_pp's --items
// builds: ai_function first (the player's `plyr` is the witnessed dispatch
// signal), move_function as the fallback. wire id = items.def id - 100000.
bool load_item_classes(const char *path,
                       std::unordered_map<uint16_t, EntityClass> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamsize n = f.tellg();
	if (n <= 0) return false;
	std::vector<uint8_t> raw(static_cast<size_t>(n));
	f.seekg(0);
	if (!f.read(reinterpret_cast<char *>(raw.data()), n)) return false;

	const uint8_t *plain = raw.data();
	size_t plain_size = raw.size();
	std::vector<uint8_t> decrypted;
	if (scr_is_scr(raw.data(), raw.size())) {
		decrypted.resize(raw.size());
		size_t out_size = decrypted.size();
		if (scr_decrypt_buf(raw.data(), raw.size(), decrypted.data(), &out_size,
		                    SCR_KEY_JO_DFX2) != 0)
			return false;
		decrypted.resize(out_size);
		plain = decrypted.data();
		plain_size = decrypted.size();
	}

	DefItemsFile items{};
	if (def_parse_items_memory(plain, plain_size, &items) != 0) return false;
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &it = items.entries[i];
		const int wire_id = it.id - 100000;
		if (wire_id < 0 || wire_id >= 0x10000) continue;
		EntityClass cls = class_from_tag(it.ai_function);
		if (cls == EntityClass::Unknown) cls = class_from_tag(it.move_function);
		if (cls != EntityClass::Unknown)
			out[static_cast<uint16_t>(wire_id)] = cls;
	}
	def_free_items(&items);
	return !out.empty();
}

struct Tracked {
	uint16_t parent = 0xFFFF;
	uint16_t type_id = 0;
	int samples = 0;
	double max_div_m = 0.0;
};

} // namespace

int main() {
	std::string cap_path;
	if (const char *env = std::getenv("NW_GOLDEN_VEHICLE_SESSION"); env && *env)
		cap_path = env;
	else
		cap_path = DEFAULT_VEHICLE_SESSION_PCAP;

	const char *items_path = std::getenv("NW_ITEMS_DEF");
	if (items_path == nullptr || *items_path == '\0') {
		std::printf("[skip] NW_ITEMS_DEF not set (items.def class table needed "
		            "to size §5.10b records)\n");
		return 0;
	}

	std::vector<net::PcapDatagram> pkts;
	if (cap_path.empty() || !net::read_pcap_udp_file(cap_path, pkts)) {
		std::printf("[skip] vehicle-session capture not found (set "
		            "NW_GOLDEN_VEHICLE_SESSION) — '%s'\n", cap_path.c_str());
		return 0;
	}

	std::unordered_map<uint16_t, EntityClass> item_classes;
	if (!load_item_classes(items_path, item_classes)) {
		std::fprintf(stderr, "FAIL: NW_ITEMS_DEF set but unusable: %s\n",
		             items_path);
		return 1;
	}

	std::vector<CaptureDatagram> caps;
	caps.reserve(pkts.size());
	for (const net::PcapDatagram &p : pkts) {
		CaptureDatagram c;
		c.frame_index = p.frame_index;
		c.src_port = p.srcport;
		c.dst_port = p.dstport;
		c.payload = p.payload;
		caps.push_back(std::move(c));
	}
	const std::vector<InGameMessage> msgs = decode_capture_to_messages(caps);

	ns::NetClientView view;
	view.set_item_class_resolver([&item_classes](uint16_t t) {
		const auto it = item_classes.find(t);
		return it != item_classes.end() ? it->second : EntityClass::Unknown;
	});

	// The oracle's classifier mirrors the view's precedence: items table first,
	// then the 0x0D pool-blanket Vehicle learn.
	std::unordered_map<uint16_t, EntityClass> learned;
	auto classify = [&](uint16_t t) -> EntityClass {
		const auto it = item_classes.find(t);
		if (it != item_classes.end() && it->second != EntityClass::Unknown)
			return it->second;
		const auto l = learned.find(t);
		return l != learned.end() ? l->second : EntityClass::Unknown;
	};

	std::map<uint16_t, Tracked> tracked;
	int s2c_frames = 0;

	for (const InGameMessage &m : msgs) {
		if (m.settings_update || m.dir != 'S') continue;
		const uint8_t tag = static_cast<uint8_t>(m.tag & 0xFF);

		if (tag == 0x0D) {
			PoolSpawnBatch batch;
			if (decode_pool_spawn_batch(m.payload.data(), m.payload.size(),
			                            batch)) {
				for (const PoolSpawnRecord &rec : batch.records) {
					learned[rec.item_type_id] = EntityClass::Vehicle;
					if (rec.parent_handle != 0xFFFFu &&
					    ((rec.parent_handle >> 12) & 0xFu) == 0u) {
						Tracked &t = tracked[rec.slot_id];
						t.parent = rec.parent_handle;
						t.type_id = rec.item_type_id;
						std::printf("[follow] pool-1 spawn %04x type %04x "
						            "parented to ORGANIC %04x (flags 0x%04x)\n",
						            rec.slot_id, rec.item_type_id,
						            rec.parent_handle, rec.spawn_flags);
					}
				}
			}
		}

		view.apply(tag, m.payload);

		if (tag != opennova::s2c::PER_FRAME_UPDATE || tracked.empty()) continue;
		++s2c_frames;
		FrameUpdate fu;
		decode_frame_update(m.payload.data(), m.payload.size(), classify, fu,
		                    (view.game_type() & 0x20000u) != 0u);
		for (const FrameUpdateRecord &rec : fu.records) {
			const auto it = tracked.find(rec.handle);
			if (it == tracked.end() || rec.cls != EntityClass::Vehicle) continue;
			// Carrier-local form (§5.10b parent_slot_handle set) is not an
			// absolute wire sample; every 0x1006 compact in this capture is
			// anchor-relative.
			if (rec.vehicle.parent_slot_handle != 0xFFFFu) continue;
			const double wx =
					static_cast<double>(fu.anchor_x +
					                    network_decompress_fixedpoint(
					                            rec.vehicle.pos_x_compressed)) /
					65536.0;
			const double wy =
					static_cast<double>(fu.anchor_y +
					                    network_decompress_fixedpoint(
					                            rec.vehicle.pos_y_compressed)) /
					65536.0;
			const double wz =
					static_cast<double>(fu.anchor_z +
					                    network_decompress_fixedpoint(
					                            rec.vehicle.pos_z_compressed)) /
					65536.0;
			const ns::ClientEntityState *es = nullptr;
			for (const ns::ClientEntityState &e : view.state().entities) {
				if (e.handle == rec.handle) {
					es = &e;
					break;
				}
			}
			if (es == nullptr) continue;
			const double dx = static_cast<double>(es->x) / 65536.0 - wx;
			const double dy = static_cast<double>(es->y) / 65536.0 - wy;
			const double dz = static_cast<double>(es->z) / 65536.0 - wz;
			const double div = std::sqrt(dx * dx + dy * dy + dz * dz);
			Tracked &t = it->second;
			++t.samples;
			if (div > t.max_div_m) t.max_div_m = div;
		}
	}

	std::printf("[follow] frames_applied=%u entities=%zu game_type=0x%x "
	            "oracle_frames=%d\n",
	            view.frames_applied(), view.state().entities.size(),
	            view.game_type(), s2c_frames);

	bool ok = true;
	ok &= expect((view.game_type() & 0x20000u) == 0u,
	             "capture is a non-objective (non-COOP) session");
	if (tracked.empty()) {
		std::printf("[skip] capture carries no organic-parented pool-1 spawns "
		            "— nothing to pin\n");
		return 0;
	}
	for (const auto &kv : tracked) {
		const Tracked &t = kv.second;
		std::printf("[follow] vehicle %04x (type %04x, parent %04x): "
		            "%d compact samples, max divergence %.2f m\n",
		            kv.first, t.type_id, t.parent, t.samples, t.max_div_m);
		ok &= expect(t.samples > 0,
		             "tracked vehicle appears in 0x0A compacts");
		ok &= expect(t.max_div_m < 2.0,
		             "vehicle stays on its own wire compact positions "
		             "(not glued to an organic parent)");
	}
	return ok ? 0 : 1;
}
