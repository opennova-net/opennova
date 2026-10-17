// opennova-update-tool: the update service's tooling (ADR 0061).
//
//   build    a packed update (1SFS) from a product's baseline tree and the tree to update it to
//   inspect  a packed update's products, command files and rows
//   serve    packed updates over UPDATE.EXE's protocol, with no database (a local or self-hosted
//            update server; the service serves its releases the same way)
//
// docs/update/updater-re.md is the protocol and formats; package_builder.h what a package holds.

#include "package_builder.h"
#include "package_catalog.h"
#include "update_tcp_server.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/sfs.h>
#include <net/update/update_wire.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace opennova;

const char kUsage[] =
		"usage:\n"
		"  opennova-update-tool build --from <dir> --to <dir> --out <file.sfs>\n"
		"      --base <CMD> [--expansion <name>=<CMD>]... [--product-name <registry name>]\n"
		"      [--exe <file>] [--exclude <path|dir/>]... [--no-remove] [--prefix <update.exe>]\n"
		"  opennova-update-tool inspect <file.sfs>\n"
		"  opennova-update-tool serve --package <file.sfs>... --product <LOGIN>=<CMD>...\n"
		"      [--server-host <name>]... [--port <n>]... [--loopback]\n";

int usage_error(const char *message) {
	std::fprintf(stderr, "opennova-update-tool: %s\n%s", message, kUsage);
	return 2;
}

bool split_pair(const std::string &text, std::string &left, std::string &right) {
	const size_t eq = text.find('=');
	if (eq == std::string::npos || eq == 0 || eq + 1 == text.size())
		return false;
	left = text.substr(0, eq);
	right = text.substr(eq + 1);
	return true;
}

bool read_package(const std::string &path, update::SfsPackage &package, std::string &error) {
	std::vector<uint8_t> bytes;
	if (!io::read_file_bytes(path, bytes, error))
		return false;
	if (!update::sfs_read(bytes.data(), bytes.size(), package, error)) {
		error = path + ": " + error;
		return false;
	}
	return true;
}

int run_build(int argc, char **argv) {
	update_tool::BuildSpec spec;
	std::string out, prefix, base_cmd, exe;
	std::vector<std::pair<std::string, std::string>> expansions;
	for (int i = 0; i < argc; ++i) {
		const std::string arg = argv[i];
		auto value = [&]() -> const char * { return i + 1 < argc ? argv[++i] : nullptr; };
		const char *v = nullptr;
		if (arg == "--no-remove") {
			spec.removes = false;
		} else if ((v = value()) == nullptr) {
			return usage_error(("missing a value after " + arg).c_str());
		} else if (arg == "--from") {
			spec.from = io::os_path(v);
		} else if (arg == "--to") {
			spec.to = io::os_path(v);
		} else if (arg == "--out") {
			out = v;
		} else if (arg == "--base") {
			base_cmd = v;
		} else if (arg == "--expansion") {
			std::string name, cmd;
			if (!split_pair(v, name, cmd))
				return usage_error("--expansion takes <name>=<CMD>");
			expansions.emplace_back(name, cmd);
		} else if (arg == "--product-name") {
			spec.package_product = v;
		} else if (arg == "--exe") {
			exe = v;
		} else if (arg == "--exclude") {
			spec.excludes.push_back(v);
		} else if (arg == "--prefix") {
			prefix = v;
		} else {
			return usage_error(("unknown option " + arg).c_str());
		}
	}
	if (spec.from.empty() || spec.to.empty() || out.empty())
		return usage_error("build needs --from, --to and --out");
	if (base_cmd.empty() && expansions.empty())
		return usage_error("build needs --base or an --expansion");
	if (!base_cmd.empty())
		spec.products.push_back({ "", "", base_cmd, "", "", exe });
	for (const auto &[name, cmd] : expansions)
		spec.products.push_back({ "expansion/" + name, name, cmd, "", "", "" });

	update::SfsPackage package;
	std::vector<update_tool::ProductReport> report;
	std::string error;
	if (!update_tool::build_update_package(spec, package, report, error)) {
		std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
		return 1;
	}
	std::vector<uint8_t> bytes;
	if (!prefix.empty() && !io::read_file_bytes(prefix, bytes, error)) {
		std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
		return 1;
	}
	const std::vector<uint8_t> payload = update::sfs_write(package);
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	if (!io::write_file_atomic(out, bytes.data(), bytes.size(), error)) {
		std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
		return 1;
	}
	for (const update_tool::ProductReport &line : report)
		std::printf("%s: %zu loose, %zu copy, %zu remove, %zu bytes of blobs\n",
				line.command_file.c_str(), line.loose_rows, line.copy_rows, line.remove_rows,
				line.blob_bytes);
	std::printf("wrote %s (%zu bytes)\n", out.c_str(), bytes.size());
	return 0;
}

int run_inspect(int argc, char **argv) {
	if (argc != 1)
		return usage_error("inspect takes one package");
	update::SfsPackage package;
	std::string error;
	if (!read_package(argv[0], package, error)) {
		std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
		return 1;
	}
	std::printf("product: %s\n", package.product.c_str());
	for (const update::SfsSection &section : package.sections) {
		std::printf("section %s: %zu file(s)\n",
				section.expansion.empty() ? "(base)" : section.expansion.c_str(),
				section.entries.size());
		for (const update::SfsEntry &entry : section.entries) {
			update::NlzHeader header;
			if (update::nlz_read_header(entry.data.data(), entry.data.size(), header)) {
				std::printf("  %-15s %10zu bytes (1ZLN, %u inflated)\n", entry.name.c_str(),
						entry.data.size(), header.raw_size);
				continue;
			}
			std::printf("  %-15s %10zu bytes\n", entry.name.c_str(), entry.data.size());
			const std::optional<update::CommandFile> cmd =
					update::read_command_file(std::string(entry.data.begin(), entry.data.end()));
			if (!cmd)
				continue;
			std::printf("    PRODUCT=%s VERSION=%s EXE=%s\n", cmd->product.c_str(),
					cmd->version.c_str(), cmd->exe.c_str());
			for (const update::CommandRow &row : cmd->rows)
				std::printf("    %s=%s %d %d %s\n", row.key.c_str(), row.name.c_str(), row.time,
						row.size, row.crc ? std::to_string(*row.crc).c_str() : "-");
		}
	}
	return 0;
}

std::atomic<bool> g_stop{ false };

void on_signal(int) {
	g_stop = true;
}

int run_serve(int argc, char **argv) {
	std::vector<std::string> packages_paths, hosts;
	std::vector<std::pair<std::string, std::string>> products;
	std::vector<uint16_t> ports;
	bool loopback = false;
	for (int i = 0; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--loopback") {
			loopback = true;
			continue;
		}
		if (i + 1 >= argc)
			return usage_error(("missing a value after " + arg).c_str());
		const std::string v = argv[++i];
		if (arg == "--package") {
			packages_paths.push_back(v);
		} else if (arg == "--product") {
			std::string login, cmd;
			if (!split_pair(v, login, cmd))
				return usage_error("--product takes <LOGIN>=<CMD>");
			products.emplace_back(login, cmd);
		} else if (arg == "--server-host") {
			hosts.push_back(v);
		} else if (arg == "--port") {
			const int port = std::atoi(v.c_str());
			if (port <= 0 || port > 65535)
				return usage_error("--port takes 1..65535");
			ports.push_back(uint16_t(port));
		} else {
			return usage_error(("unknown option " + arg).c_str());
		}
	}
	if (packages_paths.empty() || products.empty())
		return usage_error("serve needs --package and --product");
	if (hosts.empty())
		hosts.push_back(std::string(update::kRetailGatewayHosts[0]));
	if (ports.empty())
		ports.assign(std::begin(update::kGatewayPorts), std::end(update::kGatewayPorts));

	std::vector<update::SfsPackage> packages(packages_paths.size());
	std::string error;
	for (size_t i = 0; i < packages_paths.size(); ++i)
		if (!read_package(packages_paths[i], packages[i], error)) {
			std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
			return 1;
		}
	update_tool::PackageCatalog catalog(hosts);
	for (const auto &[login, cmd] : products)
		if (!catalog.bind(login, cmd, packages, error)) {
			std::fprintf(stderr, "opennova-update-tool: %s\n", error.c_str());
			return 1;
		}

	if (net::startup() != 0) {
		std::fprintf(stderr, "opennova-update-tool: networking failed to start\n");
		return 1;
	}
	update::UpdateServer server(catalog);
	net::UpdateTcpServer listener(server, [](const std::string &line) {
		std::printf("%s\n", line.c_str());
		std::fflush(stdout);
	});
	if (!listener.listen(ports, loopback)) {
		std::fprintf(stderr, "opennova-update-tool: cannot listen on the ports\n");
		net::shutdown();
		return 1;
	}
	std::printf("serving %zu product(s) on", products.size());
	for (uint16_t port : listener.ports())
		std::printf(" %u", unsigned(port));
	std::printf("%s; SERVER.LST lists %s\n", loopback ? " (loopback)" : "", hosts[0].c_str());
	std::fflush(stdout);
	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);
	while (!g_stop) {
		if (!listener.pump())
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	listener.close();
	net::shutdown();
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2)
		return usage_error("a verb is required");
	const std::string verb = argv[1];
	if (verb == "build")
		return run_build(argc - 2, argv + 2);
	if (verb == "inspect")
		return run_inspect(argc - 2, argv + 2);
	if (verb == "serve")
		return run_serve(argc - 2, argv + 2);
	return usage_error(("unknown verb " + verb).c_str());
}
