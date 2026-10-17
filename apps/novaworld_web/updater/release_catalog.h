#pragma once

// The update listener's catalog (net/update/update_server.h's UpdateCatalog) over the
// published releases (ADR 0064; hosted/data_set_repository.h published_sections): each data
// set with an updater login serves the section of its published release that carries its
// command file (the base product's section, or the expansion's PACKAGEBREAK section). A
// refresh reads the rows and keeps a cache on disk under the cache directory: the package
// `<sha256>.sfs` (fetched from the store's updates/<sha256>.sfs when it is not there yet),
// and each served section unpacked to `<sha256>/<section index>/<UPPER NAME>`, one file per
// entry, so a cumulative package of hundreds of megabytes is read once and never held in
// memory; a file reply streams the entry from disk as its socket takes it (open_file). A
// package no published section names
// any more is deleted from the cache. The table the server reads is swapped under a mutex,
// so the refresh runs on a thread of its own beside the listener's pump. SERVER.LST names
// one host, so UPDATE.EXE never load-tests: ftp.novaworld.com for a data set served
// through the Switcher's hosts block (retail_updater), else the deployment's update host
// (ONNET_UPDATE_HOST). Service policy, not a port.

#include "hosted/data_set_repository.h"

#include <net/novaworld/db/database.h>
#include <net/update/update_server.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::novaworld_server {

class ObjectStore;

namespace update_service {

class ReleaseCatalog : public update::UpdateCatalog {
public:
	// `cache_dir` is made when missing; `update_host` is what SERVER.LST names for a data
	// set not served through retail's names.
	ReleaseCatalog(std::string cache_dir, std::string update_host);

	// The table rebuilt from the rows: every published section's package fetched and
	// unpacked when it is not cached yet, the products swapped in, the cache swept of the
	// packages no section names. A section whose package cannot be fetched or does not
	// carry its command file is logged and left out (its login is refused until a refresh
	// finds it); false with `error` when the rows could not be read (the table is kept).
	bool refresh(db::Database &db, ObjectStore &store, std::string &error);

	size_t product_count() const;
	// The logins served, in order, each with its command file and package sha.
	struct Served {
		std::string login;
		std::string command_file;
		std::string sha256;
	};
	std::vector<Served> served() const;

	bool has_product(std::string_view login) const override;
	std::vector<std::string> server_hosts(std::string_view login) const override;
	std::string command_file_name(std::string_view login) const override;
	bool read_file(std::string_view login, std::string_view name,
			std::vector<uint8_t> &out) const override;
	std::unique_ptr<update::UpdateFile> open_file(
			std::string_view login, std::string_view name) const override;

	const std::string &cache_dir() const { return cache_dir_; }

private:
	struct Product {
		std::string login;
		std::string command_file;
		std::string sha256;
		bool retail_updater = false;
		std::map<std::string, std::string> files; // by upper-cased name: its path in the cache
	};
	// A package's sections that carry a command file: what a published section is matched
	// against without reading the package again.
	struct SectionKey {
		std::string expansion;
		std::string command_file; // upper-cased
		size_t index = 0;
	};

	// The product for `section`: its package in the cache (fetched when missing), its
	// section unpacked (when missing), its files listed. False with `why`.
	bool load_section(ObjectStore &store, const hosted::PublishedSection &section, Product &product,
			std::string &why);
	// The package's section keys, from the memo or the package read once; `read` whether
	// the package's bytes were read into `bytes` (left empty otherwise).
	bool section_keys(const std::string &sha256, std::vector<SectionKey> &keys,
			std::vector<uint8_t> &bytes, bool &read, std::string &why);
	// Every cached package or directory whose sha is not in `live` removed.
	void sweep(const std::set<std::string> &live);

	const Product *find(std::string_view login) const; // under mu_
	// The cached file the product serves by `name`; empty when it has none.
	std::string path_of(std::string_view login, std::string_view name) const;

	std::string cache_dir_;
	std::string update_host_;
	std::map<std::string, std::vector<SectionKey>> sections_; // by sha; the refresh thread's
	mutable std::mutex mu_;
	std::map<std::string, Product> products_; // by upper-cased login
};

} // namespace update_service
} // namespace opennova::novaworld_server
