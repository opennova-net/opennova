#include "data_sets.h"

#include "hosted_settings.h"
#include "loggable.h"

#include <base/io/json.h>

#include <utility>

namespace opennova::novaworld_server::hosted {

namespace {

// Joint Operations' host screen. Service policy, not a witnessed limit (the
// host screen's spin ranges are unread): 64 is the screen's MAX_PLAYERS cap,
// and 500 the kill limit's and the max score's "no limit".
constexpr HostLimits kJointOperationsLimits = {
	{ 2, 64 },  // max_players
	{ 1, 300 }, // time_limit (minutes)
	{ 1, 500 }, // kill_limit
	{ 1, 500 }, // max_score
	{ 0, 300 }, // respawn (seconds)
	{ 0, 300 }, // start_delay (seconds)
};

// Delta Force Xtreme 2's host screen: Joint Operations' ranges but for its
// MAX_PLAYERS cap, 100 to JO's 64 (the cap word dword_835F0C, which the host
// settings' read, slider map and fill take [orig: ReadSettings @0x553dd0,
// the slider map @0x5540b0, PopulateHostSettings @0x5544cd (dfx2.exe)]; a
// hosted server is a dedicated host). Its kill limit's and max score's "no
// limit" 500 is the same code as JO's; the other ranges are unread in both.
// docs/titles/dfx2-vs-jo-re.md DFX2-8.
constexpr HostLimits kDeltaForceXtreme2Limits = {
	{ 2, 100 }, // max_players
	{ 1, 300 }, // time_limit (minutes)
	{ 1, 500 }, // kill_limit
	{ 1, 500 }, // max_score
	{ 0, 300 }, // respawn (seconds)
	{ 0, 300 }, // start_delay (seconds)
};

// OpenNova's own data runs Joint Operations' engine rules: its limits are
// Joint Operations' (a row of its own, so it can part from them).
constexpr HostLimits kOpenNovaLimits = kJointOperationsLimits;

std::vector<DataSet> make_catalog() {
	std::vector<DataSet> out;
	{
		DataSet jo;
		jo.game = kDataSetJointOperations;
		jo.title = "jo";
		jo.display_name = "Joint Operations";
		// Escalation: the data's expansion/jox01/, mounted with /exp jox01.
		jo.expansions = { { "", "Joint Operations (base game)" }, { "jox01", "Escalation (jox01)" } };
		jo.limits = kJointOperationsLimits;
		out.push_back(std::move(jo));
	}
	{
		DataSet dfx2;
		dfx2.game = kDataSetDeltaForceXtreme2;
		dfx2.title = "dfx2";
		dfx2.display_name = "Delta Force Xtreme 2";
		dfx2.expansions = { { "", "Delta Force Xtreme 2 (base game)" } };
		dfx2.limits = kDeltaForceXtreme2Limits;
		out.push_back(std::move(dfx2));
	}
	{
		// OpenNova's own data speaks Joint Operations' identity on the wire; its
		// lobby is its own games row.
		DataSet on;
		on.game = kDataSetOpenNova;
		on.title = "jo";
		on.display_name = "OpenNova";
		on.expansions = { { "", "OpenNova (base game)" } };
		on.limits = kOpenNovaLimits;
		out.push_back(std::move(on));
	}
	return out;
}

std::string known_names() {
	std::string out;
	for (const DataSet &d : known_data_sets()) out += (out.empty() ? "" : ", ") + d.game;
	return out;
}

// `text` read as a JSON object into `out`; "" reads as an empty object.
bool parse_object(std::string_view text, const char *variable, io::JsonValue &out, std::string &error) {
	if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
		out = io::JsonValue::make_object();
		return true;
	}
	std::string parse_error;
	if (!io::json_parse(text, out, parse_error)) {
		error = std::string(variable) + " is no JSON (" + loggable(parse_error, 120) + ")";
		return false;
	}
	if (!out.is_object()) {
		error = std::string(variable) + " is no JSON object";
		return false;
	}
	return true;
}

// The config of data set `game` in `out`, made when absent; nullptr (with
// `error`) for a data set the catalog lacks.
DataSetConfig *config_for(std::vector<DataSetConfig> &out, const std::string &game, const char *variable,
		std::string &error) {
	if (!find_data_set(known_data_sets(), game)) {
		error = std::string(variable) + " names '" + loggable(game, 64) + "', which is no data set (" +
				known_names() + ")";
		return nullptr;
	}
	for (DataSetConfig &c : out) {
		if (c.game == game) return &c;
	}
	DataSetConfig c;
	c.game = game;
	out.push_back(std::move(c));
	return &out.back();
}

} // namespace

const std::vector<DataSet> &known_data_sets() {
	static const std::vector<DataSet> catalog = make_catalog();
	return catalog;
}

const DataSet *find_data_set(const std::vector<DataSet> &sets, std::string_view game) {
	for (const DataSet &d : sets) {
		if (d.game == game) return &d;
	}
	return nullptr;
}

const Expansion *find_expansion(const DataSet &data_set, std::string_view code) {
	for (const Expansion &e : data_set.expansions) {
		if (e.code == code) return &e;
	}
	return nullptr;
}

bool parse_data_set_configs(std::string_view data_sets_json, std::vector<DataSetConfig> &out, std::string &error) {
	out.clear();
	static constexpr const char *kDataSets = "ONNET_HOSTED_DATA_SETS";
	io::JsonValue sets;
	if (!parse_object(data_sets_json, kDataSets, sets, error)) return false;
	for (const io::JsonMember &m : sets.object) {
		const std::string where = std::string(kDataSets) + "." + loggable(m.key, 64);
		if (!m.value.is_object()) {
			error = where + " is no JSON object";
			return false;
		}
		DataSetConfig *c = config_for(out, m.key, kDataSets, error);
		if (!c) return false;
		for (const io::JsonMember &field : m.value.object) {
			const io::JsonValue &v = field.value;
			if (field.key == "missions") {
				if (!v.is_array()) {
					error = where + ".missions is no list";
					return false;
				}
				c->missions.clear();
				for (const io::JsonValue &mission : v.array) {
					if (!mission.is_string() || !is_mission_file_name(mission.string)) {
						error = where +
								".missions holds an entry that is no .bms file name of letters, digits, _ and -";
						return false;
					}
					c->missions.push_back(mission.string);
				}
			} else if (field.key == "resource_dir") {
				if (!v.is_string()) {
					error = where + ".resource_dir is no string";
					return false;
				}
				c->resource_dir = v.string;
			} else if (field.key == "loose_root") {
				if (!v.is_bool()) {
					error = where + ".loose_root is no true or false";
					return false;
				}
				c->loose_root = v.boolean;
			} else {
				error = where + " has '" + loggable(field.key, 32) +
						"', which is none of missions, resource_dir and loose_root";
				return false;
			}
		}
	}
	return true;
}

std::vector<DataSet> offered_data_sets(
		const std::string &provider, const std::vector<DataSetConfig> &configs) {
	std::vector<DataSet> out;
	for (const DataSet &known : known_data_sets()) {
		const DataSetConfig *config = nullptr;
		for (const DataSetConfig &c : configs) {
			if (c.game == known.game) config = &c;
		}
		if (!config) continue;
		bool has_data = false;
		if (provider == "local") has_data = !config->resource_dir.empty();
		if (provider == "ec2") has_data = true; // every cloud machine mounts every named data set
		if (!has_data) continue;
		DataSet offered = known;
		offered.missions = config->missions;
		offered.resource_dir = config->resource_dir;
		offered.loose_root = config->loose_root;
		out.push_back(std::move(offered));
	}
	return out;
}

} // namespace opennova::novaworld_server::hosted
