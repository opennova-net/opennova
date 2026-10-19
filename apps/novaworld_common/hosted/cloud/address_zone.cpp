#include "address_zone.h"

#include "core/loggable.h"
#include "core/text_util.h"
#include "ec2_machine_driver.h"
#include "hosted/model/address_names.h"

#include <base/io/json.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace opennova::novaworld_server::hosted {

namespace {

std::string fqdn_of(const std::string &name, const std::string &zone) { return name + "." + zone + "."; }

} // namespace

// --- Route53Zone ---

Route53Zone::Route53Zone(Route53ZoneConfig config, proc::CommandRunner &runner) :
		config_(std::move(config)), runner_(runner) {}

std::string Route53Zone::check_config(const Route53ZoneConfig &c) {
	if (c.aws_cli.empty() || c.aws_cli[0] != '/') return "ONNET_HOSTED_AWS_CLI is no absolute path";
	if (!is_zone_name(c.zone)) return "ONNET_HOSTED_ADDRESS_ZONE is no zone name (play.<domain>)";
	if (c.zone_id.size() < 2 || c.zone_id[0] != 'Z' ||
			!std::all_of(c.zone_id.begin(), c.zone_id.end(), [](char ch) { return is_alnum(ch); }))
		return "ONNET_HOSTED_ADDRESS_ZONE_ID is no Route 53 zone id (Z...)";
	if (c.region.empty()) return "ONNET_HOSTED_EC2_REGION is not set";
	return {};
}

std::string Route53Zone::change_batch(const std::string &action, const std::string &fqdn, const std::string &ip) {
	io::JsonValue record = io::JsonValue::make_object();
	record.set("Name", io::json_string(fqdn));
	record.set("Type", io::json_string("A"));
	record.set("TTL", io::json_number(static_cast<double>(kAddressTtlS)));
	io::JsonValue value = io::JsonValue::make_object();
	value.set("Value", io::json_string(ip));
	io::JsonValue values = io::JsonValue::make_array();
	values.push(std::move(value));
	record.set("ResourceRecords", std::move(values));
	io::JsonValue one = io::JsonValue::make_object();
	one.set("Action", io::json_string(action));
	one.set("ResourceRecordSet", std::move(record));
	io::JsonValue changes = io::JsonValue::make_array();
	changes.push(std::move(one));
	io::JsonValue batch = io::JsonValue::make_object();
	batch.set("Changes", std::move(changes));
	return io::json_write(batch);
}

ZoneChange Route53Zone::change(const std::string &action, const std::string &name, const std::string &ip) {
	ZoneChange out;
	const std::vector<std::string> argv = { config_.aws_cli, "route53", "change-resource-record-sets",
		"--hosted-zone-id", config_.zone_id, "--change-batch", change_batch(action, fqdn_of(name, config_.zone), ip),
		"--region", config_.region, "--output", "json", "--no-cli-pager", "--cli-connect-timeout", "10",
		"--cli-read-timeout", "30" };
	const proc::CommandResult result = runner_.run(argv, config_.command_timeout_ms);
	if (!result.ok()) {
		// A delete of a record that is not there: nothing to delete.
		if (action == "DELETE" && result.outcome == proc::CommandResult::Outcome::Exited &&
				result.err.find("(InvalidChangeBatch)") != std::string::npos &&
				result.err.find("not found") != std::string::npos) {
			out.ok = true;
			return out;
		}
		out.error = Ec2MachineDriver::cli_failure("change-resource-record-sets", result);
		out.error.replace(0, std::string("aws ec2").size(), "aws route53");
		return out;
	}
	io::JsonValue v;
	std::string error;
	const io::JsonValue *info = io::json_parse(result.out, v, error) ? v.get("ChangeInfo") : nullptr;
	const std::string id = info != nullptr && info->is_object() ? info->get_string("Id", "") : std::string();
	if (id.empty()) {
		out.error = "aws route53 change-resource-record-sets: the reply names no change";
		return out;
	}
	out.ok = true;
	out.change_id = id;
	return out;
}

ZoneChange Route53Zone::upsert(const std::string &name, const std::string &ip) { return change("UPSERT", name, ip); }

ZoneChange Route53Zone::remove(const std::string &name, const std::string &ip) { return change("DELETE", name, ip); }

std::optional<bool> Route53Zone::in_sync(const std::string &change_id) {
	// A delete that found nothing made no change.
	if (change_id.empty()) return true;
	const std::vector<std::string> argv = { config_.aws_cli, "route53", "get-change", "--id", change_id, "--region",
		config_.region, "--output", "json", "--no-cli-pager", "--cli-connect-timeout", "10", "--cli-read-timeout",
		"30" };
	const proc::CommandResult result = runner_.run(argv, config_.command_timeout_ms);
	if (!result.ok()) {
		std::fprintf(stderr, "[fleet] WARN %s\n", Ec2MachineDriver::cli_failure("get-change", result).c_str());
		return std::nullopt;
	}
	io::JsonValue v;
	std::string error;
	const io::JsonValue *info = io::json_parse(result.out, v, error) ? v.get("ChangeInfo") : nullptr;
	if (info == nullptr || !info->is_object()) return std::nullopt;
	const std::string status = info->get_string("Status", "");
	if (status == "INSYNC") return true;
	if (status == "PENDING") return false;
	return std::nullopt;
}

// --- FileZone ---

FileZone::FileZone(std::string zone, std::string path) : zone_(std::move(zone)), path_(std::move(path)) {}

std::string FileZone::render(
		const std::string &zone, const std::map<std::string, std::string> &records, uint32_t serial) {
	std::ostringstream out;
	out << "; " << zone << ": the static addresses' names (apps/novaworld_common/hosted/cloud/address_zone.h FileZone)\n";
	out << "$ORIGIN " << zone << ".\n";
	out << "$TTL " << kAddressTtlS << "\n";
	out << "@\tIN\tSOA\tns." << zone << ". hostmaster." << zone << ". ( " << serial << " 3600 600 86400 "
		<< kAddressTtlS << " )\n";
	out << "@\tIN\tNS\tns." << zone << ".\n";
	for (const auto &[name, ip] : records) out << name << "\t" << kAddressTtlS << "\tIN\tA\t" << ip << "\n";
	return out.str();
}

std::map<std::string, std::string> FileZone::parse(const std::string &text) {
	std::map<std::string, std::string> out;
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		std::istringstream words(line);
		std::vector<std::string> w;
		for (std::string word; words >> word;) w.push_back(word);
		if (w.size() == 5 && w[2] == "IN" && w[3] == "A") out[w[0]] = w[4];
	}
	return out;
}

ZoneChange FileZone::rewrite(const std::function<void(std::map<std::string, std::string> &)> &edit) {
	std::lock_guard<std::mutex> lock(mu_);
	ZoneChange out;
	std::string text;
	{
		std::ifstream in(path_, std::ios::binary);
		if (in) {
			std::ostringstream all;
			all << in.rdbuf();
			text = all.str();
		}
	}
	std::map<std::string, std::string> records = parse(text);
	// The SOA's serial: the last one's, one on.
	uint32_t serial = 1;
	if (const size_t open = text.find("( "); open != std::string::npos) {
		const size_t end = text.find(' ', open + 2);
		if (const auto last = strutil::parse_llong(text.substr(open + 2, end - open - 2)); last && *last > 0)
			serial = static_cast<uint32_t>(*last + 1);
	}
	edit(records);
	std::error_code ec;
	const std::filesystem::path path(path_);
	if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
	const std::filesystem::path next = path_ + ".next";
	{
		std::ofstream file(next, std::ios::binary | std::ios::trunc);
		file << render(zone_, records, serial);
		if (!file) {
			out.error = "the zone file " + loggable(path_, 200) + " could not be written";
			return out;
		}
	}
	std::filesystem::rename(next, path, ec);
	if (ec) {
		out.error = "the zone file " + loggable(path_, 200) + ": " + ec.message();
		return out;
	}
	out.ok = true;
	out.change_id = "file-" + std::to_string(serial);
	return out;
}

ZoneChange FileZone::upsert(const std::string &name, const std::string &ip) {
	return rewrite([&](std::map<std::string, std::string> &records) { records[name] = ip; });
}

ZoneChange FileZone::remove(const std::string &name, const std::string &ip) {
	return rewrite([&](std::map<std::string, std::string> &records) {
		const auto it = records.find(name);
		if (it != records.end() && it->second == ip) records.erase(it);
	});
}

std::optional<bool> FileZone::in_sync(const std::string &) { return true; }

} // namespace opennova::novaworld_server::hosted
