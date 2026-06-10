#include "util/engine_caches.h"

#include <atomic>

namespace opennova {

namespace {
std::atomic<uint64_t> g_cache_epoch{1};
} // namespace

uint64_t cache_epoch() {
	return g_cache_epoch.load(std::memory_order_acquire);
}

void bump_cache_epoch() {
	g_cache_epoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace opennova
