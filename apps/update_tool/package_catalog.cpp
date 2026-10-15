#include "package_catalog.h"

#include <formats/update/update_ini.h>

namespace opennova::update_tool {

using update::ascii_upper;

bool PackageCatalog::bind(const std::string &login, const std::string &command_file,
		const std::vector<update::SfsPackage> &packages, std::string &error) {
	const std::string wanted = ascii_upper(command_file);
	for (auto package = packages.rbegin(); package != packages.rend(); ++package) {
		for (const update::SfsSection &section : package->sections) {
			bool carries = false;
			for (const update::SfsEntry &entry : section.entries)
				carries = carries || ascii_upper(entry.name) == wanted;
			if (!carries)
				continue;
			Product product;
			product.command_file = command_file;
			for (const update::SfsEntry &entry : section.entries)
				product.files[ascii_upper(entry.name)] = entry.data;
			products_[ascii_upper(login)] = std::move(product);
			return true;
		}
	}
	error = "no package carries " + command_file + " for " + login;
	return false;
}

const PackageCatalog::Product *PackageCatalog::find(std::string_view login) const {
	const auto it = products_.find(ascii_upper(login));
	return it == products_.end() ? nullptr : &it->second;
}

bool PackageCatalog::has_product(std::string_view login) const {
	return find(login) != nullptr;
}

std::vector<std::string> PackageCatalog::server_hosts(std::string_view login) const {
	return find(login) ? hosts_ : std::vector<std::string>();
}

std::string PackageCatalog::command_file_name(std::string_view login) const {
	const Product *product = find(login);
	return product ? product->command_file : std::string();
}

bool PackageCatalog::read_file(
		std::string_view login, std::string_view name, std::vector<uint8_t> &out) const {
	const Product *product = find(login);
	if (product == nullptr)
		return false;
	const auto it = product->files.find(ascii_upper(name));
	if (it == product->files.end())
		return false;
	out = it->second;
	return true;
}

} // namespace opennova::update_tool
