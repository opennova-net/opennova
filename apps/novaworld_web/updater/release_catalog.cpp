#include "release_catalog.h"

#include "core/object_store.h"
#include "hosted/data_set_jobs.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/update/sfs.h>
#include <net/update/update_wire.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

namespace opennova::novaworld_server::update_service {

namespace fs = std::filesystem;

namespace {

constexpr const char *kPartialSuffix = ".partial";

bool is_sha256_hex(std::string_view text) {
	if (text.size() != 64)
		return false;
	for (const char c : text)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	return true;
}

// The cached files of one unpacked section, by upper-cased name.
bool list_section_files(
		const std::string &dir, std::map<std::string, std::string> &files, std::string &why) {
	std::error_code ec;
	for (fs::directory_iterator it(io::os_path(dir), ec), end; !ec && it != end; it.increment(ec)) {
		std::error_code type_ec;
		if (!it->is_regular_file(type_ec))
			continue;
		const std::string name = io::utf8_path(it->path().filename());
		files[strutil::to_upper(name)] = io::utf8_join(dir, name);
	}
	if (ec) {
		why = "cannot list " + dir + ": " + ec.message();
		return false;
	}
	return true;
}

} // namespace

ReleaseCatalog::ReleaseCatalog(std::string cache_dir, std::string update_host) :
		cache_dir_(std::move(cache_dir)), update_host_(std::move(update_host)) {}

bool ReleaseCatalog::refresh(db::Database &db, ObjectStore &store, std::string &error) {
	std::vector<hosted::PublishedSection> sections;
	try {
		sections = hosted::published_sections(db);
	} catch (const db::Error &e) {
		error = e.what();
		return false;
	}
	if (!io::ensure_directory(cache_dir_, error))
		return false;
	std::map<std::string, Product> next;
	std::set<std::string> live;
	for (const hosted::PublishedSection &section : sections) {
		if (!is_sha256_hex(section.sha256)) {
			std::fprintf(stderr, "[update] WARN %s not served: its release's sha is no SHA-256\n",
					section.updater_login.c_str());
			continue;
		}
		live.insert(section.sha256);
		Product product;
		std::string why;
		if (!load_section(store, section, product, why)) {
			std::fprintf(stderr, "[update] WARN %s not served: %s\n", section.updater_login.c_str(),
					why.c_str());
			continue;
		}
		next[strutil::to_upper(section.updater_login)] = std::move(product);
	}
	sweep(live);
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		changed = next.size() != products_.size();
		for (const auto &[login, product] : next) {
			const auto it = products_.find(login);
			changed = changed || it == products_.end() || it->second.sha256 != product.sha256 ||
					it->second.command_file != product.command_file ||
					it->second.retail_updater != product.retail_updater;
		}
		products_.swap(next);
	}
	if (changed) {
		std::printf("[update] serving %zu product(s)", product_count());
		for (const Served &s : served())
			std::printf(
					" %s=%s (%.12s)", s.login.c_str(), s.command_file.c_str(), s.sha256.c_str());
		std::printf("\n");
	}
	return true;
}

bool ReleaseCatalog::section_keys(const std::string &sha256, std::vector<SectionKey> &keys,
		std::vector<uint8_t> &bytes, bool &read, std::string &why) {
	read = false;
	const auto memo = sections_.find(sha256);
	if (memo != sections_.end()) {
		keys = memo->second;
		return true;
	}
	const std::string package_path = io::utf8_join(cache_dir_, sha256 + ".sfs");
	if (!io::read_file_bytes(package_path, bytes, why))
		return false;
	update::SfsPackage package;
	if (!update::sfs_read(bytes.data(), bytes.size(), package, why)) {
		why = "the package does not read: " + why;
		return false;
	}
	keys.clear();
	for (size_t i = 0; i < package.sections.size(); ++i)
		for (const update::SfsEntry &entry : package.sections[i].entries)
			if (strutil::ends_with_icase(entry.name, ".CMD"))
				keys.push_back({ package.sections[i].expansion, strutil::to_upper(entry.name), i });
	sections_[sha256] = keys;
	read = true;
	return true;
}

bool ReleaseCatalog::load_section(ObjectStore &store, const hosted::PublishedSection &section,
		Product &product, std::string &why) {
	const std::string package_path = io::utf8_join(cache_dir_, section.sha256 + ".sfs");
	std::error_code ec;
	if (!fs::is_regular_file(io::os_path(package_path), ec)) {
		const std::string partial = package_path + kPartialSuffix;
		if (!store.get_file(hosted::release_package_key(section.sha256), partial, why)) {
			why = "the package could not be fetched: " + why;
			return false;
		}
		fs::rename(io::os_path(partial), io::os_path(package_path), ec);
		if (ec) {
			why = "cannot put the package in place: " + ec.message();
			return false;
		}
		std::printf("[update] fetched %s (%s)\n",
				hosted::release_package_key(section.sha256).c_str(), section.updater_login.c_str());
	}
	std::vector<SectionKey> keys;
	std::vector<uint8_t> bytes;
	bool read = false;
	if (!section_keys(section.sha256, keys, bytes, read, why))
		return false;
	const SectionKey *key = nullptr;
	for (const SectionKey &k : keys)
		if (strutil::iequals(k.expansion, section.expansion) &&
				k.command_file == strutil::to_upper(section.command_file))
			key = &k;
	if (key == nullptr) {
		why = "no section of the package carries " + section.command_file +
				(section.expansion.empty() ? std::string() : " for " + section.expansion);
		return false;
	}
	const std::string dir =
			io::utf8_join(io::utf8_join(cache_dir_, section.sha256), std::to_string(key->index));
	if (!fs::is_directory(io::os_path(dir), ec)) {
		// The memo answered without the bytes: the package is read for its entries.
		if (!read && !io::read_file_bytes(package_path, bytes, why))
			return false;
		update::SfsPackage package;
		if (!update::sfs_read(bytes.data(), bytes.size(), package, why) ||
				key->index >= package.sections.size()) {
			why = "the package does not read: " + why;
			return false;
		}
		const std::string partial = dir + kPartialSuffix;
		fs::remove_all(io::os_path(partial), ec);
		if (!io::ensure_directory(partial, why))
			return false;
		for (const update::SfsEntry &entry : package.sections[key->index].entries) {
			if (entry.name.empty() || entry.name.find_first_of("\\/") != std::string::npos ||
					entry.name == "." || entry.name == "..") {
				why = "the package carries an entry name no file takes";
				return false;
			}
			if (!io::write_file_atomic(io::utf8_join(partial, strutil::to_upper(entry.name)),
						entry.data.data(), entry.data.size(), why))
				return false;
		}
		fs::rename(io::os_path(partial), io::os_path(dir), ec);
		if (ec) {
			why = "cannot put the section in place: " + ec.message();
			return false;
		}
		std::printf("[update] unpacked section %zu of %.12s for %s (%zu file(s))\n", key->index,
				section.sha256.c_str(), section.updater_login.c_str(),
				package.sections[key->index].entries.size());
	}
	product.login = section.updater_login;
	product.command_file = section.command_file;
	product.sha256 = section.sha256;
	product.retail_updater = section.retail_updater;
	return list_section_files(dir, product.files, why);
}

void ReleaseCatalog::sweep(const std::set<std::string> &live) {
	std::error_code ec;
	std::vector<fs::path> gone;
	for (fs::directory_iterator it(io::os_path(cache_dir_), ec), end; !ec && it != end;
			it.increment(ec)) {
		std::string name = io::utf8_path(it->path().filename());
		if (name.size() > 8 && name.compare(name.size() - 8, 8, kPartialSuffix) == 0) {
			gone.push_back(it->path()); // a fetch or an unpack cut short
			continue;
		}
		if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sfs") == 0)
			name.resize(name.size() - 4);
		if (!is_sha256_hex(name) || live.count(name) == 0)
			gone.push_back(it->path());
	}
	for (const fs::path &path : gone) {
		std::error_code rm;
		fs::remove_all(path, rm);
		std::string name = io::utf8_path(path.filename());
		if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sfs") == 0) {
			name.resize(name.size() - 4);
			sections_.erase(name);
			std::printf("[update] dropped %s.sfs from the cache: no published release names it\n",
					name.c_str());
		}
	}
}

const ReleaseCatalog::Product *ReleaseCatalog::find(std::string_view login) const {
	const auto it = products_.find(strutil::to_upper(login));
	return it == products_.end() ? nullptr : &it->second;
}

size_t ReleaseCatalog::product_count() const {
	std::lock_guard<std::mutex> lock(mu_);
	return products_.size();
}

std::vector<ReleaseCatalog::Served> ReleaseCatalog::served() const {
	std::lock_guard<std::mutex> lock(mu_);
	std::vector<Served> out;
	for (const auto &[key, product] : products_)
		out.push_back({ product.login, product.command_file, product.sha256 });
	return out;
}

bool ReleaseCatalog::has_product(std::string_view login) const {
	std::lock_guard<std::mutex> lock(mu_);
	return find(login) != nullptr;
}

std::vector<std::string> ReleaseCatalog::server_hosts(std::string_view login) const {
	std::lock_guard<std::mutex> lock(mu_);
	const Product *product = find(login);
	if (product == nullptr)
		return {};
	return { product->retail_updater ? std::string(update::kRetailGatewayHosts[0]) : update_host_ };
}

std::string ReleaseCatalog::command_file_name(std::string_view login) const {
	std::lock_guard<std::mutex> lock(mu_);
	const Product *product = find(login);
	return product ? product->command_file : std::string();
}

std::string ReleaseCatalog::path_of(std::string_view login, std::string_view name) const {
	std::lock_guard<std::mutex> lock(mu_);
	const Product *product = find(login);
	if (product == nullptr)
		return {};
	const auto it = product->files.find(strutil::to_upper(name));
	return it == product->files.end() ? std::string() : it->second;
}

std::unique_ptr<update::UpdateFile> ReleaseCatalog::open_file(
		std::string_view login, std::string_view name) const {
	const std::string path = path_of(login, name);
	if (path.empty())
		return nullptr;
	std::unique_ptr<update::UpdateFile> file = update::UpdateFile::from_disk(path);
	if (!file)
		std::fprintf(stderr, "[update] WARN %s's %s does not open\n", std::string(login).c_str(),
				std::string(name).c_str());
	return file;
}

bool ReleaseCatalog::read_file(
		std::string_view login, std::string_view name, std::vector<uint8_t> &out) const {
	const std::string path = path_of(login, name);
	if (path.empty())
		return false;
	std::string error;
	if (!io::read_file_bytes(path, out, error)) {
		std::fprintf(stderr, "[update] WARN %s's %s does not read: %s\n",
				std::string(login).c_str(), std::string(name).c_str(), error.c_str());
		return false;
	}
	return true;
}

} // namespace opennova::novaworld_server::update_service
