#include "ragdoll_set.h"

#include <base/io/tick_rate.h>

#include <algorithm>

namespace opennova::ragdoll {

namespace {

constexpr float kTickSeconds = static_cast<float>(1.0 / io::kTickHz);
// Ticks an entry with no ragdoll outlives its subject.
constexpr int kForgetTicks = 8;

} // namespace

void RagdollSet::set_enabled(bool enabled) {
	if (enabled_ == enabled)
		return;
	enabled_ = enabled;
	if (!enabled_)
		clear();
}

void RagdollSet::end(Entry &entry) {
	entry.ragdoll.reset();
	entry.blend_left = 0;
	entry.out.pose.clear();
	entry.out.weight = 0.0f;
}

Need RagdollSet::observe(const SubjectHead &head) {
	if (!enabled_)
		return Need::None;
	Entry &e = entries_[head.key];
	e.observed = true;
	e.missing = 0;
	if (e.seen && e.life != head.life) {
		end(e);
		e.was_dying = false;
		e.was_eligible = false;
	}
	e.life = head.life;
	e.node = head.node;

	Need need = Need::None;
	if (head.hidden) {
		end(e);
	} else if (e.ragdoll != nullptr) {
		if (head.dying) {
			need = Need::Run;
		} else {
			// Revived, or taken up by a medic: the clip takes the body back.
			e.ragdoll.reset();
			e.blend_left = kBlendOutTicks;
		}
	} else if (e.blend_left == 0 && e.seen && head.dying && head.ragdolls && !e.was_dying &&
			e.was_eligible && head.eligible) {
		need = Need::Start;
	}
	e.seen = true;
	e.was_dying = head.dying;
	e.was_eligible = head.eligible;
	return need;
}

void RagdollSet::make_room() {
	int awake = 0;
	int kept = 0;
	Entry *oldest_awake = nullptr;
	Entry *oldest = nullptr;
	for (auto &[key, e] : entries_) {
		if (e.ragdoll == nullptr)
			continue;
		++kept;
		if (oldest == nullptr || e.order < oldest->order)
			oldest = &e;
		if (e.ragdoll->asleep())
			continue;
		++awake;
		if (oldest_awake == nullptr || e.order < oldest_awake->order)
			oldest_awake = &e;
	}
	if (awake >= kMaxAwake && oldest_awake != nullptr)
		oldest_awake->ragdoll->sleep();
	if (kept >= kMaxKept && oldest != nullptr)
		end(*oldest);
}

bool RagdollSet::start(const SubjectHead &head, std::shared_ptr<const Rig> rig,
		const std::vector<anim::PoseBone> &pose, const std::vector<anim::PoseBone> *pose_prev,
		const Vec3 &anchor, const Collider &collider) {
	auto it = entries_.find(head.key);
	if (!enabled_ || it == entries_.end())
		return false;
	Entry &e = it->second;
	make_room();
	StartInput in;
	in.node = head.node;
	in.node_prev = head.node;
	// The body's own travel over the last ticks carries into the fall.
	if (e.node_ticks > 0) {
		const Vec3 per_tick =
				(head.node.origin - e.node_origin) * (1.0f / static_cast<float>(e.node_ticks));
		in.node_prev.origin = head.node.origin - per_tick;
	}
	in.pose = &pose;
	in.pose_prev = pose_prev;
	in.kick = head.kick;
	in.anchor = anchor;
	auto ragdoll = std::make_unique<Ragdoll>();
	if (!ragdoll->start(std::move(rig), in, collider))
		return false;
	e.ragdoll = std::move(ragdoll);
	e.anchor = anchor;
	e.blend_left = 0;
	e.order = next_order_++;
	e.ragdoll->pose(e.node, e.out.pose);
	e.out.weight = 1.0f;
	++started_;
	return true;
}

void RagdollSet::feed(uint32_t key, const Xform &node, const Vec3 &anchor) {
	auto it = entries_.find(key);
	if (it == entries_.end())
		return;
	it->second.node = node;
	it->second.anchor = anchor;
}

void RagdollSet::step(int ticks, const Collider &collider) {
	ticks = std::max(ticks, 0);
	// The awake budget holds through a step: a sleeper whose anchor drifted
	// wakes only while fewer than kMaxAwake step, else it sleeps on at the
	// anchor it now follows.
	int awake = 0;
	for (const auto &[key, e] : entries_)
		if (e.ragdoll != nullptr && !e.ragdoll->asleep())
			++awake;
	for (auto it = entries_.begin(); it != entries_.end();) {
		Entry &e = it->second;
		if (!e.observed)
			e.missing += ticks;
		if (e.ragdoll != nullptr) {
			if (e.observed) {
				for (int t = 0; t < ticks && !e.ragdoll->failed(); ++t) {
					bool rebase = false;
					if (e.ragdoll->drifted(e.anchor)) {
						if (awake < kMaxAwake) {
							e.ragdoll->wake();
							++awake;
						} else {
							rebase = true;
						}
					}
					const bool stepping = !e.ragdoll->asleep();
					e.ragdoll->step(collider, e.anchor, kTickSeconds);
					if (stepping && (e.ragdoll->asleep() || e.ragdoll->failed()))
						--awake;
					if (rebase)
						e.ragdoll->sleep();
				}
				if (e.ragdoll->failed()) {
					end(e);
				} else {
					e.ragdoll->pose(e.node, e.out.pose);
					e.out.weight = 1.0f;
				}
			} else if (!e.ragdoll->asleep()) {
				e.ragdoll->sleep();
				--awake;
			}
		} else if (e.blend_left > 0) {
			e.blend_left = std::max(0, e.blend_left - ticks);
			e.out.weight = static_cast<float>(e.blend_left) / static_cast<float>(kBlendOutTicks);
			if (e.blend_left == 0)
				end(e);
		}
		if (e.observed) {
			e.node_origin = e.node.origin;
			e.node_ticks = 0;
		}
		e.node_ticks += ticks;
		e.observed = false;
		const int keep = e.ragdoll != nullptr ? kGraceTicks : kForgetTicks;
		if (e.missing > keep) {
			it = entries_.erase(it);
		} else {
			++it;
		}
	}
}

const RagdollOutput *RagdollSet::output(uint32_t key) const {
	auto it = entries_.find(key);
	if (it == entries_.end())
		return nullptr;
	const RagdollOutput &out = it->second.out;
	return (out.weight > 0.0f && !out.pose.empty()) ? &out : nullptr;
}

const Ragdoll *RagdollSet::ragdoll(uint32_t key) const {
	auto it = entries_.find(key);
	return it == entries_.end() ? nullptr : it->second.ragdoll.get();
}

void RagdollSet::clear() {
	entries_.clear();
}

RagdollStats RagdollSet::stats() const {
	RagdollStats s;
	s.started = started_;
	for (const auto &[key, e] : entries_) {
		if (e.ragdoll != nullptr) {
			if (e.ragdoll->asleep()) {
				++s.sleeping;
			} else {
				++s.awake;
			}
		} else if (e.blend_left > 0) {
			++s.blending;
		}
	}
	return s;
}

} // namespace opennova::ragdoll
