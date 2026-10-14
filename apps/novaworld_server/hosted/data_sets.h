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
// them it offers and where each one's data is:
//
//   ONNET_HOSTED_DATA_SETS             a JSON object keyed by data set: its
//                                      default rotation ("missions"), and for
//                                      the local provider its game folder
//                                      ("resource_dir", "loose_root")
//   ONNET_HOSTED_EC2_LAUNCH_TEMPLATES  a JSON object, data set -> the launch
//                                      template whose data volume holds it
//                                      (the deploy's, from the Terraform
//                                      outputs)
//
// A data set is offered when the provider has its data: the local provider a
// resource_dir, the ec2 provider a launch template. Only offered ones may be
// started (check_settings), and the site lists only those.

#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server::hosted {

// The data sets' slugs: the games rows (backend/seed/0001_games.sql, and the
// OpenNova row of migration 0013) whose lobbies they list in.
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
	std::string resource_dir;          // local: --resource-dir
	bool loose_root = false;           // local: --loose-root (a folder with no archives)
	std::string launch_template;       // ec2: the launch template (lt-...)
};

// What a deployment says about one data set (ONNET_HOSTED_DATA_SETS and
// ONNET_HOSTED_EC2_LAUNCH_TEMPLATES, merged by data set).
struct DataSetConfig {
	std::string game;
	std::vector<std::string> missions;
	std::string resource_dir;
	bool loose_root = false;
	std::string launch_template;
};

// Every data set the service knows, in the order the site offers them.
const std::vector<DataSet> &known_data_sets();
// The data set `game` names among `sets`; nullptr for none.
const DataSet *find_data_set(const std::vector<DataSet> &sets, std::string_view game);
// The expansion `code` of `data_set`; nullptr when it allows none by that code.
const Expansion *find_expansion(const DataSet &data_set, std::string_view code);

// The two variables read into configs, merged by data set: false with `error`
// when either is no JSON object of the shape above, names a data set the
// catalog lacks, or holds a mission that is no .bms file name. An empty text is
// an empty object.
bool parse_data_set_configs(std::string_view data_sets_json, std::string_view launch_templates_json,
		std::vector<DataSetConfig> &out, std::string &error);

// The data sets `provider` ("local", "ec2") offers: each known one, in the
// catalog's order, that `configs` gives the provider's data (a resource_dir, a
// launch template), with the config's values. Any other provider offers none.
std::vector<DataSet> offered_data_sets(const std::string &provider, const std::vector<DataSetConfig> &configs);

} // namespace opennova::novaworld_server::hosted
