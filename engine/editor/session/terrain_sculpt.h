#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <editor/documents/terrain_heights_document.h>
#include <editor/session/terrain_follow.h>
#include <formats/cpt/trngen/terrain_region_bake.h>

namespace opennova::editor {

class SessionCore;

// The terrain sculpt's baker (ADR 0046 S25, the heights lane): the session's own thread that bakes the heights a
// stroke changed with the TrnGen port's region bake (formats/cpt/trngen/terrain_region_bake.h), so that every tile
// the stroke did not reach keeps its bytes and the remade ones are the bytes a whole bake of the sculpted input makes.
// A bake starts once a heights document holds regions whose bake is due and no gesture is open in it (its stroke
// ended), and not while a redo is kept past its last step (the redo would bring the step the bake belongs to back); a
// new stroke on the file stops a bake running for it (its regions join the next one). Its result lands as part of the
// stroke's own step (HeightsBakeLanding: one Ctrl+Z takes the stroke and its bake) when the document is still in the
// state the bake started from; where that step cannot take it (saved since, or Save's settle over a kept redo) it lands
// as a step of its own carrying the stroke's link; otherwise it is dropped and its regions stay due. A bake that fails
// (the bake refusing rough ground, D-TERRAIN-22) is not tried again until the heights change, its reason a finding of
// the document's (terrain_heights.bake_refused). The first bake of a file makes its bake state: a cold in-memory bake
// of the input its tiles were made from (a .cpt's atlas: D-TERRAIN-23), made in the background once the file is
// sculpted (Sculpt picked over its terrain, or its first stroke) and kept for the file's next bakes. Save and Play wait
// (settle). Single-threaded on the session's side: poll, interrupt and settle are the session thread's; only the bake
// runs on the worker.
class TerrainSculpt {
public:
	explicit TerrainSculpt(SessionCore &core);
	~TerrainSculpt();
	TerrainSculpt(const TerrainSculpt &) = delete;
	TerrainSculpt &operator=(const TerrainSculpt &) = delete;

	// Each poll: a bake that finished lands (or is dropped), and a heights document due a bake with no stroke open
	// starts one when none runs; a follow a drag held back is applied once the drag ends.
	void poll();
	// A stroke's first dab on `path`: a bake running for it stops.
	void interrupt(const std::string &path);
	// A stroke's first dab on `document`: its ground's window opens where none is (the open missions as they stand,
	// so a landing leaves an entity edited since where it was put).
	void stroke_began(const TerrainHeightsDocument &document);
	// Save and Play: every bake due for the heights document at `path` made and landed now, the caller waiting, as its
	// stroke's step or as one of its own; `followed` takes the missions the landing moved. False, with why, where a bake
	// fails (its regions stay due).
	bool settle(const std::string &path, std::string &error, std::vector<std::string> *followed = nullptr);
	// Whether a bake runs or is due for `path`, and its words for the pane's bake line ("" none).
	std::string bake_words(const std::string &path) const;
	// The bakes made since the session started (a test's count), and the bakes started (each lands, fails or is
	// dropped).
	size_t bakes_landed() const { return landed_; }
	size_t bakes_started() const { return started_; }
	// Flatten's target for a stroke (the live height under its first sample, raw16), kept for its gesture.
	void remember_flatten(uint64_t gesture, int32_t raw);
	bool flatten_target(uint64_t gesture, int32_t &raw) const;

private:
	struct Job;
	struct Seed;
	// The job a document's state asks for now; null where nothing is due, it cannot bake (a blocked or read-only
	// document, one with no planes), its last bake failed on these heights, or (unless `force`) a redo is kept.
	std::unique_ptr<Job> make_job(const TerrainHeightsDocument &document, bool force);
	// The seed kept for a document's file whose input it was made from (by its tiles, else by its texels' hash: a
	// match adopts the document's input, so the next is by its tiles) under `locks`; null none.
	const Seed *seed_for_(const TerrainHeightsDocument &document, const std::array<trngen::CornerLockFlags, 4> &locks);
	// A seed of `path` from `input` (null: one made already, or one that failed on it), and the next seed to make: an
	// open heights document's being sculpted with no state, else, Sculpt picked over the active view's terrain, its
	// heights file's from the file. `cpt`: a heightmap image's import's .cpt (S25 A2b), whose header the seed's whole
	// bake takes and which it is checked against; "" for a .cpt's own seed.
	std::unique_ptr<Job> seed_job_(const std::string &path, const RasterPlane &input,
			const std::array<trngen::CornerLockFlags, 4> &locks, const std::string &cpt = std::string());
	std::unique_ptr<Job> next_seed_();
	// Whether `document` is being sculpted: regions due, or Sculpt picked over its terrain in the active view; and
	// none of an import's outputs (a stroke routes to its source).
	bool sculpted_(const TerrainHeightsDocument &document) const;
	// The quadrant locks the bake of the .cpt `cpt` takes (terrain_bake_locks), kept per the project's files' state.
	std::array<trngen::CornerLockFlags, 4> locks_for_(const std::string &cpt);
	void start(std::unique_ptr<Job> job);
	// The job's bake, on whichever thread runs it.
	static void run(Job &job);
	// A finished job landed in its document, or dropped; true when it landed. `force`: as a step of its own over a
	// kept redo (Save's settle).
	bool land(Job &job, bool force, std::vector<std::string> *followed = nullptr);
	// A bake that failed on a document's heights: kept, said once, the document's finding.
	void failed_(const Job &job, TerrainHeightsDocument *document);
	void refresh_imports_();
	void work();

	SessionCore &core_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::thread worker_;
	bool stopping_ = false;
	std::unique_ptr<Job> queued_;   // handed to the worker
	std::unique_ptr<Job> running_;  // the worker's
	std::unique_ptr<Job> finished_; // back from the worker, to land
	std::atomic<bool> cancel_{false};
	std::string running_path_;
	bool running_seed_ = false; // the job running is a seed (no stroke stops it; closing the session does)
	size_t landed_ = 0, started_ = 0;
	// The seeds made: each file's bake state from the input its tiles were made from (a .cpt's atlas, an image's
	// heights as read), its texels' hash and the locks it was baked under, the last few files' within a byte budget;
	// an image's with the whole .cpt its bake made (what its first landing lays under the strokes' regions).
	struct Seed {
		std::string path;
		uint64_t content = 0;
		std::array<trngen::CornerLockFlags, 4> locks{};
		RasterPlane input;
		std::shared_ptr<const trngen::TerrainBakeState> state;
		std::shared_ptr<const CptFile> cpt;
		size_t bytes = 0;
	};
	std::vector<Seed> seeds_;
	// The heights files seeded from their file (by path and stamp), not read again.
	std::vector<std::pair<std::string, uint64_t>> tried_;
	std::vector<std::pair<uint64_t, int32_t>> flatten_; // the last few strokes' targets
	// The bakes that failed: a document's heights (by its identity, load and revision), a seed's input (by its path and
	// texels' hash).
	struct Failure {
		std::string path;
		uint64_t identity = 0, load = 0, revision = 0, content = 0;
		bool seed = false;
		std::string why;
	};
	std::vector<Failure> failures_;
	// Each heights document's ground window: the open missions' states when its first stroke since its last full
	// landing began (FollowMark), by mission path.
	std::map<std::string, std::map<std::string, FollowMark>> windows_;
	std::vector<FollowDeferred> deferred_;
	struct LocksMemo {
		std::string cpt;
		uint64_t generation = 0;
		std::array<trngen::CornerLockFlags, 4> locks{};
	};
	std::vector<LocksMemo> locks_memo_;
	const void *imports_scan_ = nullptr; // the scan the image documents' imports were last read against
};

} // namespace opennova::editor
