// R1 of the net parity proof ladder: our in-game body codec against every
// message body in the committed retail captures (fixtures/novaworld/run_*).
//
// Each file's stream is reassembled, every distinct (direction, tag, body) is
// looked up in the message catalog, and each catalog-Decoded body runs through
// the body codec registry (net/npwire/ingame_body_codec.h): the decoder must
// read the body to the byte, and where a struct encoder exists, re-encoding the
// decoded fields must reproduce the retail bytes exactly. PrinterOnly /
// Unhandled tags and the 0x100 session layer are reported, not proven.
//
// A known divergence is waived only by a row naming its D-NET entry and the
// exact number of distinct bodies it covers. The ratchet runs both ways: an
// unwaived failure fails, and a waived row whose count no longer matches fails
// too ("bank it"). `--dump` prints the waiver rows the current state needs.
//
// The S2C 0x0A / C2S 0x0C compact records need each wire type id's record
// class. The core leg resolves the 14 ids the corpus uses through an authored
// definition table; the `_retail` leg proves that table against the real
// items.def and reruns the sweep with the real catalog.

#include <net/npwire/ingame_body_codec.h>
#include <net/npwire/ingame_decode.h>
#include <net/npwire/ingame_message_catalog.h>

#include <formats/def/def.h>
#include <runtime/replication/item_replication_catalog.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "common/nwmsg_fixture.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova;
namespace rep = opennova::replication;

namespace {

// --- the record classes the corpus needs ------------------------------------

// The items.def rows behind the 14 wire type ids in the corpus (wire id =
// definition id - 100000). Network dispatch resolves ai_function first
// [orig: EntityDef network callback lookup; item_replication_catalog.cpp].
struct AuthoredItem {
	int32_t definition_id;
	const char *ai_function;
	const char *move_function;
	const char *name; // items.def `begin` label, for the reader
};

const AuthoredItem kCorpusItems[] = {
	{100074, "cveh", "cbike", "Drivable Motorcycle"},
	{100164, "cveh", "ctank", "Drivable M1A1 Tank"},
	{100165, "cveh", "ctank", "Drivable T80 Tank"},
	{101213, "cveh", "catv", "Drivable BTR-80, Indonesian"},
	{101214, "chel", "chel", "Flyable Mil-26, Indonesian helo"},
	{101215, "chel", "chel", "Flyable Nb0-105, Indonesian helo"},
	{101301, "cveh", "catv", "Drivable ATV"},
	{101302, "cveh", "cveh", "Drivable SUV with 50cal"},
	{101304, "cveh", "cveh", "Drivable Attack Vehicle with MK19"},
	{101305, "cveh", "cveh", "Drivable SUV with Mk19"},
	{101308, "chel", "chel", "Flyable Super Puma, Indonesian helo"},
	{102099, "chel", "chel", "Flyable Nb0-105 with miniguns, Indonesian helo"},
	{102100, "chel", "chel", "Flyable Nb0-105 with rocketpods, Indonesian helo"},
	{105305, "plyr", "org2", "Player #1, Multiplayer"},
};

rep::ItemReplicationCatalog authored_catalog() {
	std::vector<rep::ItemReplicationDefinition> defs;
	for (const AuthoredItem &item : kCorpusItems) {
		rep::ItemReplicationDefinition d;
		d.definition_id = item.definition_id;
		d.ai_function = item.ai_function;
		d.move_function = item.move_function;
		defs.push_back(d);
	}
	return rep::ItemReplicationCatalog::from_definitions(defs);
}

// --- the corpus -------------------------------------------------------------

struct BodyKey {
	char dir;
	uint16_t full_tag;
	std::vector<uint8_t> body;
	bool operator<(const BodyKey &o) const {
		return std::tie(dir, full_tag, body) < std::tie(o.dir, o.full_tag, o.body);
	}
};

struct Corpus {
	std::map<BodyKey, size_t> bodies; // distinct body -> occurrences
	std::set<uint32_t> game_types;    // S2C 0x08 fields[3], per run
	size_t messages = 0;
	size_t fragmented = 0;
};

bool ends_with(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool load_corpus(Corpus &corpus) {
	const std::string root = FIXTURE_DIR;
	for (const char *run : nwmsg::kRuns) {
		const std::string run_dir = root + "/" + run;
		std::map<std::string, nwmsg::ManifestSection> manifest;
		std::string err;
		if (!nwmsg::load_manifest(run_dir + "/manifest.txt", manifest, err)) {
			std::fprintf(stderr, "manifest: %s\n", err.c_str());
			return false;
		}
		for (const auto &entry : manifest) {
			const std::string &name = entry.first;
			// `X_packets` repeats `X`'s stream (nwmsg_replay pins that).
			if (ends_with(name, "_packets.nwmsg")) {
				const std::string plain =
						name.substr(0, name.size() - std::strlen("_packets.nwmsg")) + ".nwmsg";
				if (manifest.count(plain) != 0) continue;
			}
			std::vector<nwmsg::FixtureBundle> bundles;
			std::vector<nwmsg::LogicalMessage> messages;
			if (!nwmsg::load_nwmsg(run_dir + "/" + name, bundles, err) ||
			    !nwmsg::reassembled_messages(bundles, messages, err)) {
				std::fprintf(stderr, "%s/%s: %s\n", run, name.c_str(), err.c_str());
				return false;
			}
			size_t frags = 0;
			std::set<uint16_t> tags;
			for (const nwmsg::LogicalMessage &m : messages) {
				++corpus.bodies[BodyKey{m.dir, m.full_tag, m.body}];
				if (m.fragmented) ++frags;
				tags.insert(m.full_tag);
				if (m.dir == 'S' && m.full_tag == s2c::SESSION_CONFIG) {
					SessionConfig cfg;
					if (decode_session_config(m.body.data(), m.body.size(), cfg))
						corpus.game_types.insert(static_cast<uint32_t>(cfg.fields[3]));
				}
			}
			corpus.messages += messages.size();
			corpus.fragmented += frags;
			std::printf("file %s/%s msgs=%zu reassembled=%zu tags=%zu\n", run, name.c_str(),
			            messages.size(), frags, tags.size());
		}
	}
	return true;
}

// --- the sweep --------------------------------------------------------------

enum class Failure : uint8_t { DecodeGap, EncodeDivergence };

const char *failure_name(Failure f) {
	return f == Failure::DecodeGap ? "DecodeGap" : "EncodeDivergence";
}

struct TagStats {
	const MsgCatalogEntry *entry = nullptr;
	const IngameBodyCodec *codec = nullptr;
	size_t occurrences = 0;
	size_t bodies = 0;
	size_t bytes = 0;
	size_t decoded_exact = 0;
	size_t reencoded = 0;
	size_t identical = 0;
	size_t identical_bytes = 0;
	std::map<Failure, size_t> failures; // distinct bodies per failure kind
	std::vector<uint8_t> first_failure;  // for the report
	std::vector<uint8_t> first_ours;     // our re-encode of it (EncodeDivergence)
};

using TagKey = std::pair<char, uint16_t>;

struct Sweep {
	std::map<TagKey, TagStats> tags;
	size_t session_layer_bodies = 0;
	size_t uncatalogued = 0;
};

const MsgCatalogEntry *catalog_entry(char dir, uint8_t tag) {
	size_t n = 0;
	const MsgCatalogEntry *table = ingame_message_catalog(&n);
	for (size_t i = 0; i < n; ++i)
		if (table[i].dir == dir && table[i].tag == tag) return &table[i];
	return nullptr;
}

Sweep sweep(const Corpus &corpus, const IngameBodyCodecContext &ctx) {
	Sweep s;
	for (const auto &kv : corpus.bodies) {
		const BodyKey &key = kv.first;
		if (key.full_tag >= 0x100) {
			++s.session_layer_bodies;
			continue;
		}
		const uint8_t tag = static_cast<uint8_t>(key.full_tag);
		TagStats &t = s.tags[{key.dir, key.full_tag}];
		t.entry = catalog_entry(key.dir, tag);
		t.occurrences += kv.second;
		++t.bodies;
		t.bytes += key.body.size();
		if (t.entry == nullptr) {
			++s.uncatalogued;
			continue;
		}
		if (t.entry->coverage != MsgCoverage::Decoded) continue;
		t.codec = lookup_ingame_body_codec(key.dir, tag);
		if (t.codec == nullptr) continue; // reported as a missing registry entry
		const IngameBodyCodecResult r = t.codec->run(key.body.data(), key.body.size(), ctx);
		if (r.decode != BodyDecode::Exact) {
			if (t.failures[Failure::DecodeGap]++ == 0) t.first_failure = key.body;
			continue;
		}
		++t.decoded_exact;
		if (!r.reencoded) continue;
		++t.reencoded;
		if (r.bytes == key.body) {
			++t.identical;
			t.identical_bytes += key.body.size();
		} else if (t.failures[Failure::EncodeDivergence]++ == 0) {
			t.first_failure = key.body;
			t.first_ours = r.bytes;
		}
	}
	return s;
}

// --- waivers ----------------------------------------------------------------

// A known divergence, by the D-NET entry that tracks it and the exact number of
// distinct corpus bodies it covers.
struct ParityWaiver {
	char dir;
	uint8_t tag;
	Failure kind;
	const char *cite; // "D-NET-<n>: why"
	size_t bodies;
};

const ParityWaiver kWaivers[] = {
	// (none)
	{0, 0, Failure::DecodeGap, nullptr, 0},
};

bool cites_dnet(const char *cite) {
	if (cite == nullptr || std::strncmp(cite, "D-NET-", 6) != 0) return false;
	size_t i = 6;
	while (cite[i] >= '0' && cite[i] <= '9') ++i;
	return i > 6;
}

std::string hex_head(const std::vector<uint8_t> &b, size_t max = 24) {
	std::string out;
	char buf[4];
	for (size_t i = 0; i < b.size() && i < max; ++i) {
		std::snprintf(buf, sizeof buf, "%02x", b[i]);
		out += buf;
	}
	if (b.size() > max) out += "..";
	return out;
}

// Where our re-encode first leaves the retail bytes.
std::string first_difference(const std::vector<uint8_t> &retail, const std::vector<uint8_t> &ours) {
	size_t i = 0;
	while (i < retail.size() && i < ours.size() && retail[i] == ours[i]) ++i;
	const auto window = [i](const std::vector<uint8_t> &b) {
		const size_t from = i >= 4 ? i - 4 : 0;
		return hex_head(std::vector<uint8_t>(b.begin() + std::min(from, b.size()),
		                                     b.begin() + std::min(from + 12, b.size())), 12);
	};
	return "at +" + std::to_string(i) + " (len retail " + std::to_string(retail.size()) +
	       " ours " + std::to_string(ours.size()) + ") retail[" + window(retail) + "] ours[" +
	       window(ours) + "]";
}

const char *coverage_name(MsgCoverage c) {
	switch (c) {
	case MsgCoverage::Decoded: return "Decoded";
	case MsgCoverage::PrinterOnly: return "PrinterOnly";
	case MsgCoverage::Unhandled: return "Unhandled";
	}
	return "?";
}

// Report the sweep and apply the waivers; returns the number of violations.
int judge(const Sweep &s, bool dump, const char *leg) {
	int violations = 0;
	std::map<std::tuple<char, uint8_t, Failure>, const ParityWaiver *> waivers;
	for (const ParityWaiver &w : kWaivers) {
		if (w.cite == nullptr) continue;
		if (!cites_dnet(w.cite) || w.bodies == 0) {
			std::printf("FAIL [%s] waiver %c 0x%02X %s must cite D-NET-<n> and a body count\n",
			            leg, w.dir, w.tag, failure_name(w.kind));
			++violations;
		}
		waivers[{w.dir, w.tag, w.kind}] = &w;
	}
	std::set<std::tuple<char, uint8_t, Failure>> waivers_used;

	size_t identical_tags = 0, identical_bodies = 0, identical_bytes = 0;
	size_t encodable_bodies = 0, encodable_bytes = 0;
	size_t decode_only_tags = 0, reported_tags = 0, divergences = 0;
	size_t corpus_bytes = 0;
	std::printf("[%s] dir tag  name                       class       bodies   bytes decode   identical  verdict\n", leg);
	for (const auto &kv : s.tags) {
		const TagStats &t = kv.second;
		const char dir = kv.first.first;
		const uint8_t tag = static_cast<uint8_t>(kv.first.second);
		corpus_bytes += t.bytes;
		std::string verdict;
		if (t.entry == nullptr) {
			verdict = "UNCATALOGUED";
			++violations;
		} else if (t.entry->coverage != MsgCoverage::Decoded) {
			verdict = "reported";
			++reported_tags;
		} else if (t.codec == nullptr) {
			verdict = "NO REGISTRY ENTRY";
			++violations;
		} else {
			if (t.codec->encoder != nullptr) {
				encodable_bodies += t.bodies;
				encodable_bytes += t.bytes;
			} else {
				++decode_only_tags;
			}
			for (const auto &f : t.failures) {
				if (f.second == 0) continue;
				divergences += f.second;
				const auto key = std::make_tuple(dir, tag, f.first);
				const auto w = waivers.find(key);
				if (dump)
					std::printf("DUMP {'%c', 0x%02X, Failure::%s, \"D-NET-?: \", %zu},\n", dir, tag,
					            failure_name(f.first), f.second);
				if (w == waivers.end()) {
					verdict += std::string(" FAIL ") + failure_name(f.first) + " x" +
					           std::to_string(f.second) + " first=" + hex_head(t.first_failure);
					if (f.first == Failure::EncodeDivergence)
						verdict += " " + first_difference(t.first_failure, t.first_ours);
					++violations;
				} else if (w->second->bodies != f.second) {
					verdict += std::string(" FAIL waiver count ") +
					           std::to_string(w->second->bodies) + " != " +
					           std::to_string(f.second) + " (bank it)";
					++violations;
					waivers_used.insert(key);
				} else {
					verdict += std::string(" waived ") + w->second->cite;
					waivers_used.insert(key);
				}
			}
			if (verdict.empty())
				verdict = t.codec->encoder != nullptr ? "IDENTICAL" : "decode-exact (decode-only)";
			if (t.codec->encoder != nullptr && t.identical == t.bodies) {
				++identical_tags;
				identical_bodies += t.identical;
				identical_bytes += t.identical_bytes;
			} else {
				identical_bodies += t.identical;
				identical_bytes += t.identical_bytes;
			}
		}
		std::printf("[%s]  %c  0x%02X %-26s %-11s %6zu %7zu %3zu/%-3zu %3zu/%-3zu %s\n", leg,
		            dir, tag, t.entry ? t.entry->name : "?",
		            t.entry ? coverage_name(t.entry->coverage) : "-", t.bodies, t.bytes,
		            t.decoded_exact, t.bodies, t.identical, t.reencoded, verdict.c_str());
	}
	for (const auto &w : waivers) {
		if (waivers_used.count(w.first) != 0) continue;
		std::printf("FAIL [%s] waiver %c 0x%02X %s (%s) no longer fires: bank it, delete the row\n",
		            leg, std::get<0>(w.first), std::get<1>(w.first),
		            failure_name(std::get<2>(w.first)), w.second->cite);
		++violations;
	}
	std::printf("NWMSG-PARITY[%s] identical_tags=%zu identical_bodies=%zu/%zu "
	            "identical_bytes=%zu/%zu decode_only_tags=%zu reported_tags=%zu "
	            "session_layer_bodies=%zu divergences=%zu corpus_bytes=%zu\n",
	            leg, identical_tags, identical_bodies, encodable_bodies, identical_bytes,
	            encodable_bytes, decode_only_tags, reported_tags, s.session_layer_bodies,
	            divergences, corpus_bytes);
	return violations;
}

// The registry stays a subset of the catalog's Decoded set, and every Decoded
// tag the corpus carries has an entry (judge() reports a missing one).
int registry_matches_catalog() {
	size_t n = 0;
	const IngameBodyCodec *codecs = ingame_body_codecs(&n);
	int bad = 0;
	for (size_t i = 0; i < n; ++i) {
		const MsgCatalogEntry *e = catalog_entry(codecs[i].dir, codecs[i].tag);
		if (e == nullptr || e->coverage != MsgCoverage::Decoded) {
			std::printf("FAIL registry entry %c 0x%02X is not a catalog Decoded tag\n",
			            codecs[i].dir, codecs[i].tag);
			++bad;
		}
		for (size_t j = 0; j < i; ++j)
			if (codecs[j].dir == codecs[i].dir && codecs[j].tag == codecs[i].tag) {
				std::printf("FAIL registry entry %c 0x%02X is duplicated\n", codecs[i].dir,
				            codecs[i].tag);
				++bad;
			}
	}
	return bad;
}

int retail_leg(const Corpus &corpus, const rep::ItemReplicationCatalog &authored, bool dump) {
	const std::string items_path = retail::asset_file("items.def");
	if (items_path.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS carrying items.def");
	def::DefItemsFile items{};
	if (def::def_parse_items(items_path.c_str(), &items) != 0) {
		std::printf("FAIL cannot parse %s\n", items_path.c_str());
		return 1;
	}
	const rep::ItemReplicationCatalog real = rep::ItemReplicationCatalog::from_items_def(items);
	def::def_free_items(&items);
	int bad = 0;
	// Each authored row resolves to the class the real items.def gives it.
	for (const AuthoredItem &item : kCorpusItems) {
		const uint16_t wire = static_cast<uint16_t>(item.definition_id - 100000);
		const auto want = real.resolve_wire_entity_class(wire);
		const auto got = authored.resolve_wire_entity_class(wire);
		if (!want || !got || *want != *got) {
			std::printf("FAIL authored row %d (%s) disagrees with items.def\n",
			            item.definition_id, item.name);
			++bad;
		}
	}
	IngameBodyCodecContext ctx = IngameBodyCodecContext::for_game_type(*corpus.game_types.begin());
	ctx.class_of = [&real](uint16_t id) {
		const auto c = real.resolve_wire_entity_class(id);
		return c ? *c : EntityClass::Unknown;
	};
	bad += judge(sweep(corpus, ctx), dump, "retail");
	return bad;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	bool dump = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--dump") == 0) dump = true;

	Corpus corpus;
	TEST_EXPECT(load_corpus(corpus));
	std::printf("corpus: %zu messages (%zu reassembled), %zu distinct bodies\n",
	            corpus.messages, corpus.fragmented, corpus.bodies.size());

	// All three runs are one game type (AAS), so their bodies share one context.
	TEST_EXPECT(corpus.game_types.size() == 1);
	TEST_EXPECT(*corpus.game_types.begin() == 0x10010u);

	int violations = registry_matches_catalog();

	const rep::ItemReplicationCatalog authored = authored_catalog();
	TEST_EXPECT(authored.valid());
	IngameBodyCodecContext ctx = IngameBodyCodecContext::for_game_type(*corpus.game_types.begin());
	std::set<uint16_t> unknown_ids;
	ctx.class_of = [&authored, &unknown_ids](uint16_t id) {
		const auto c = authored.resolve_wire_entity_class(id);
		if (!c) unknown_ids.insert(id);
		return c ? *c : EntityClass::Unknown;
	};
	violations += judge(sweep(corpus, ctx), dump, "core");
	for (uint16_t id : unknown_ids) {
		std::printf("FAIL wire type 0x%04X (%d) is not in the authored table\n", id, id + 100000);
		++violations;
	}

	violations += retail_leg(corpus, authored, dump);
	if (violations != 0) {
		std::printf("nwmsg_codec_parity: %d violation(s)\n", violations);
		return 1;
	}
	return 0;
}
