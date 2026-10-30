#pragma once

// The deployment's downloads host (ADR 0070) written by the data program from the products'
// published releases, so what the site's download buttons fetch is what the update service
// serves:
//
//   downloads.publish  {}, cap 1. The release routes enqueue it in the transaction that publishes
//                      or retires a release (one dedupe key: a publish while one is queued adds
//                      none). Every product the host offers (the service's own, and an
//                      account's own game while it is public, ADR 0074) with a download (its
//                      file's stable name; an account's own game's player-<key>-game-windows.zip)
//                      and a published release is rendered into the downloads store
//                      (ONNET_DOWNLOADS_URL, the downloads bucket's top):
//                        a game        the runtime's published package (the app whose login is
//                                      ONRUNTIME) applied into an empty folder with its command
//                                      file beside it, then the game's UPDATE.WIZ (one with an
//                                      updater login, or an account's own; CMD its command
//                                      file, URL this deployment's update host, PRODUCT_LOGIN
//                                      its wizard login,
//                                      or its updater login when it has none; an account's own
//                                      game's GAME its title, our own key) and
//                                      expansion/<code>/UPDATE.WIZ for each of its expansions of
//                                      the service's own with a login (an account's own mod is
//                                      installed in game), zipped; none while the runtime has no
//                                      published release
//                        an updatable  its own published package applied into an empty folder
//                        app           with its command file beside it, zipped (the editor's)
//                        a download-   its build as uploaded (product_jobs.h
//                        only app      release_download_key)
//                      each put under its stable name with Cache-Control
//                      kDownloadCacheControl, then manifest.json (kManifestCacheControl, what
//                      the site reads):
//
//                        {"written": "<ISO 8601 UTC>",
//                         "products": [{"key", "kind", "name", "description", "owner",
//                                       "protocol", "file", "size", "sha256", "version",
//                                       "release", "published"}]}
//
//                      every product the host offers listed in list_products' order (games,
//                      expansions, apps; an account's own game once it has a published release:
//                      no private one, no account's own expansion), owner the username of the
//                      account whose game it is (null for the service's own), protocol a game's
//                      {code, name, version} (the protocol its title speaks, version its
//                      protocol version; null for an expansion and an app), file its download's
//                      name (null with none), size, sha256, version (its
//                      release's, else its section's VERSION), release (its id) and published
//                      (ISO 8601 UTC) null while it offers no rendered download. Every other
//                      object on the host is removed after. A publish or retire that landed
//                      while it ran (the products' published releases read again at its end)
//                      runs it again; a store failure runs it again; a package that does not
//                      apply or a wizard that does not write fails it, the host left as it was.
//                      With no downloads store or no update host it writes nothing.
//
// Service policy, not a port.

#include "core/object_store.h"
#include "jobs/worker_pool.h"

#include <net/novaworld/db/database.h>

#include <cstdint>
#include <string>

namespace opennova::novaworld_server::hosted {

inline constexpr const char *kJobDownloadsPublish = "downloads.publish";
// The downloads host's manifest, at its top, and what its objects are served with (CloudFront
// honours the origin's Cache-Control, infra/aws/downloads.tf).
inline constexpr const char *kDownloadsManifestName = "manifest.json";
inline constexpr const char *kDownloadCacheControl = "public, max-age=300";
inline constexpr const char *kManifestCacheControl = "public, max-age=60";

// The downloads host's render queued (in the caller's transaction); the job's id, 0 when one is
// queued or running.
int64_t enqueue_downloads_publish(db::Database &db);

// downloads.publish's handler: the packages and builds read from `store`, the host written into
// `downloads` (none: the job writes nothing), its work under `scratch_dir`, its wizards naming
// `update_host` (ONNET_UPDATE_HOST; "" writes nothing). Both stores outlive the workers.
void register_downloads_publish(jobs::JobRegistry &registry, ObjectStore &store,
		ObjectStore *downloads, std::string scratch_dir, std::string update_host);

} // namespace opennova::novaworld_server::hosted
