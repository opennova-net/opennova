#pragma once

// The products a set of packed updates serves (net/update/update_server.h's UpdateCatalog):
// each login is bound to its command file's name, and the section of the latest package that
// carries that command file is the product's files, its command file and its blobs. The
// DB-less catalog behind `opennova-update-tool serve`; the service's (ADR 0061 phase 4) reads
// its releases instead.

#include <formats/update/sfs.h>
#include <net/update/update_server.h>

#include <map>
#include <string>
#include <vector>

namespace opennova::update_tool {

class PackageCatalog : public update::UpdateCatalog {
public:
	// `hosts`: the names SERVER.LST lists for every product.
	explicit PackageCatalog(std::vector<std::string> hosts) : hosts_(std::move(hosts)) {}

	// Serve `login` from the section whose command file is `command_file` in the packages added
	// (the last added that carries it). False when none carries it.
	bool bind(const std::string &login, const std::string &command_file,
			const std::vector<update::SfsPackage> &packages, std::string &error);

	bool has_product(std::string_view login) const override;
	std::vector<std::string> server_hosts(std::string_view login) const override;
	std::string command_file_name(std::string_view login) const override;
	bool read_file(std::string_view login, std::string_view name,
			std::vector<uint8_t> &out) const override;

private:
	struct Product {
		std::string command_file;
		std::map<std::string, std::vector<uint8_t>> files; // by upper-cased name
	};
	const Product *find(std::string_view login) const;

	std::vector<std::string> hosts_;
	std::map<std::string, Product> products_; // by upper-cased login
};

} // namespace opennova::update_tool
