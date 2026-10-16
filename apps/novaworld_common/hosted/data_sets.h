#pragma once

// The data sets a hosted server can run (the hosted titles program): a game
// folder a machine mounts, the title opennova-serve speaks over it (its
// `/game <code>`, a gameprofile row) and the lobby it lists in (the `games`
// row whose slug names the data set, which hosted_servers.game stores). It is
// always our opennova-serve; the data set picks what it mounts and who it
// says it is.
//
// The service knows a fixed catalog (known_data_sets(): what each data set
// is, its expansions and its host-screen limits); a deployment says which of
// them it offers in ONNET_HOSTED_DATA_SETS: a JSON object keyed by data set,
// each with its default rotation ("missions") and whether its folder is loose
// files with no game archives ("loose_root": its servers then run with
// --loose-root).
//
// Every data set the variable names is offered, since every machine mounts each
// one at /opt/game/<data set> (machine_driver.h): a cloud machine its volume (the
// launch templates attach one per data set, infra/aws/fleet.tf, labelled by
// Ec2MachineDriver::volume_label), a local stack's machine container the data
// container's mount (docker_machine_driver.h). Only offered ones may be started
// (check_settings), and the site lists only those.

#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// The data sets' slugs: the games rows (backend/seed/0001_games.sql) whose
// lobbies they list in.
inline constexpr const char *kDataSetJointOperations = "jop_2_consumer";
inline constexpr const char *kDataSetDeltaForceXtreme2 = "dfx2_consumer";
inline constexpr const char *kDataSetOpenNova = "opennova_0_consumer";

// A whole-number setting's range, both ends included.
struct SettingRange {
	int min = 0;
	int max = 0;
};

// The ranges a hosted server's whole-number settings take, the host screen's
// for the data set's title.
struct HostLimits {
	SettingRange max_players;
	SettingRange time_limit;
	SettingRange kill_limit;
	SettingRange max_score;
	SettingRange respawn;
	SettingRange start_delay;
};

// An expansion a data set may mount: its /exp code ("" for the base game
// alone) and the name the site shows.
struct Expansion {
	std::string code;
	std::string name;
};

struct DataSet {
	std::string game;         // the games row's slug (hosted_servers.game)
	std::string title;        // the gameprofile code opennova-serve's /game takes
	std::string display_name; // what the site shows
	// The expansions it allows, the base game ("") first. Joiners need the same.
	std::vector<Expansion> expansions;
	HostLimits limits;

	// The deployment's (an offered data set's; empty in the catalog):
	std::vector<std::string> missions; // the default rotation, and the site's mission list
	bool loose_root = false; // --loose-root: a folder of loose files, no archives
};

// What a deployment says about one data set (ONNET_HOSTED_DATA_SETS).
struct DataSetConfig {
	std::string game;
	std::vector<std::string> missions;
	bool loose_root = false;
};

// Every data set the service knows, in the order the site offers them.
const std::vector<DataSet> &known_data_sets();
// The data set `game` names among `sets`; nullptr for none.
const DataSet *find_data_set(const std::vector<DataSet> &sets, std::string_view game);
// The expansion `code` of `data_set`; nullptr when it allows none by that code.
const Expansion *find_expansion(const DataSet &data_set, std::string_view code);

// ONNET_HOSTED_DATA_SETS read into configs: false with `error` when it is no
// JSON object of the shape above, names a data set the catalog lacks, or holds
// a mission that is no .bms file name. An empty text is an empty object.
bool parse_data_set_configs(std::string_view data_sets_json, std::vector<DataSetConfig> &out, std::string &error);

// The data sets a deployment offers: each known one, in the catalog's order,
// that `configs` names, with the config's values.
std::vector<DataSet> offered_data_sets(const std::vector<DataSetConfig> &configs);

} // namespace opennova::novaworld_server::hosted
