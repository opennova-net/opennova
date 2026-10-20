// The BIKi decoder's stages over bitstreams built here (formats/bink/bink.h), in place of decoding a
// shipped movie: the container header and its refusals; the bit reader's LSB-first order and its
// 32-bit plane alignment; the Huffman trees in each of their three spellings (number 0's identity,
// an explicit symbol prefix, the merge passes) and the codes of the retail code tables; every bundle's
// refill (the repeat forms, the block types' run codes 12..15, the DC deltas in groups of eight, a
// bundle refilled row by row and one filled once for the plane, a bundle ended by a count of 0);
// every block type 0..9 and every scaled subtype (3, 5, 6, 8, 9); the DCT's DC term exactly and an AC
// term by its separable shape; the residue's coefficient walk; the plane order Y, V, U. Each frame's
// expected pixels are worked out here from the construction alone (the codec's witnessed tables, the
// retail YUV law yuv_to_rgb pins in bink_test), and the two decoded frames are pinned by FNV-1a64.
// The retired bink_retail row decoded 120 frames of the JO:CA install's main.bik (800 x 450, 1856
// frames); no encoder exists for real movies, so the stages are held here one by one instead.
// [orig: BinkVideo_DecodeFrame @ 0x3001F260, binkw32.dll] [orig: BinkVideo_DecodePlane @ 0x3001D2C0,
// binkw32.dll] (the shapes below are formats/bink/bink.cpp's, its bitstream contract).
#include <formats/bink/bink.h>
#include <formats/bink/bink_tables.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace detail = opennova::bink::detail;
using opennova::bink::BinkMovie;
using opennova::bink::BinkStatus;

int failures = 0;

bool expect(bool condition, const std::string &message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message.c_str());
		++failures;
	}
	return condition;
}

uint64_t fnv1a64(const std::vector<uint8_t> &bytes) {
	uint64_t h = 1469598103934665603ull;
	for (uint8_t b : bytes) {
		h ^= b;
		h *= 1099511628211ull;
	}
	return h;
}

// --- the bit writer: LSB first, as the decoder's reader takes bits ---------------------------------

class BitWriter {
public:
	void write(uint32_t value, unsigned count) {
		for (unsigned bit = 0; bit < count; ++bit) {
			if ((bits_ & 7) == 0) bytes_.push_back(0);
			bytes_.back() |= static_cast<uint8_t>(((value >> bit) & 1U) << (bits_ & 7));
			++bits_;
		}
	}
	void align32() {
		while ((bits_ & 31) != 0) write(0, 1);
	}
	const std::vector<uint8_t> &bytes() const { return bytes_; }

private:
	std::vector<uint8_t> bytes_;
	size_t bits_ = 0;
};

unsigned bit_width(uint32_t value) {
	unsigned bits = 0;
	do {
		++bits;
		value >>= 1;
	} while (value != 0);
	return bits;
}

int32_t floor_shift(int64_t value, unsigned count) {
	return static_cast<int32_t>(value >= 0 ? value >> count : -((-value + (int64_t{1} << count) - 1) >> count));
}

// --- the Huffman trees -------------------------------------------------------------------------------

// A tree as the stream spells it: number 0 (the identity, no more bits), an explicit symbol prefix (the
// rest of the symbols follow in order), or the merge passes with depths 0 and 1 and every choice bit
// set, which swaps each pair and then each pair of pairs: 3 2 1 0 7 6 5 4 11 10 9 8 15 14 13 12 (worked
// by hand from the merge rule).
struct TreeSpec {
	uint8_t number = 0;
	enum Form { Identity, Prefix, Merged } form = Identity;
	std::vector<uint8_t> prefix;

	std::array<uint8_t, 16> symbols() const {
		std::array<uint8_t, 16> out{};
		if (form == Prefix) {
			size_t at = 0;
			for (uint8_t s : prefix) out[at++] = s;
			for (uint8_t s = 0; s < 16; ++s)
				if (std::find(prefix.begin(), prefix.end(), s) == prefix.end()) out[at++] = s;
		} else if (form == Merged) {
			for (uint8_t group = 0; group < 16; group += 4)
				for (uint8_t i = 0; i < 4; ++i) out[group + i] = static_cast<uint8_t>(group + 3 - i);
		} else {
			for (uint8_t s = 0; s < 16; ++s) out[s] = s;
		}
		return out;
	}

	void write(BitWriter &w) const {
		if (form == Identity) {
			w.write(0, 4);
			return;
		}
		w.write(number, 4);
		if (form == Prefix) {
			w.write(1, 1);
			w.write(static_cast<uint32_t>(prefix.size() - 1), 3);
			for (uint8_t s : prefix) w.write(s, 4);
			return;
		}
		w.write(0, 1);
		w.write(1, 2);  // the last depth: 1
		for (int i = 0; i < 16; ++i) w.write(1, 1);  // 8 merges of depth 0, 4 of depth 1, two bits each
	}

	// The symbol's code: its leaf's place in the tree's symbol order, that leaf's code in the retail
	// tables of the tree's number.
	void emit(BitWriter &w, uint8_t symbol) const {
		const std::array<uint8_t, 16> order = symbols();
		const size_t leaf = static_cast<size_t>(std::find(order.begin(), order.end(), symbol) - order.begin());
		const uint8_t number = form == Identity ? 0 : this->number;
		w.write(detail::kTreeCodes[number][leaf], detail::kTreeLengths[number][leaf]);
	}
};

TreeSpec identity() { return TreeSpec{}; }
TreeSpec prefixed(uint8_t number, std::vector<uint8_t> prefix) {
	TreeSpec t;
	t.number = number;
	t.form = TreeSpec::Prefix;
	t.prefix = std::move(prefix);
	return t;
}
TreeSpec merged(uint8_t number) {
	TreeSpec t;
	t.number = number;
	t.form = TreeSpec::Merged;
	return t;
}

// The bundles in the decoder's order.
enum Bundle { BlockTypes, SubTypes, Colors, Patterns, XMotion, YMotion, IntraDc, InterDc, Runs, kBundles };

struct Trees {
	std::array<TreeSpec, kBundles> bundle;
	std::array<TreeSpec, 16> high;

	void write(BitWriter &w) const {
		for (int i = 0; i < kBundles; ++i) {
			if (i == Colors)
				for (const TreeSpec &t : high) t.write(w);
			if (i != IntraDc && i != InterDc) bundle[i].write(w);
		}
	}
};

Trees the_trees() {
	Trees t;
	t.bundle[BlockTypes] = prefixed(1, {6, 0, 1});
	t.bundle[SubTypes] = identity();
	t.bundle[Colors] = prefixed(7, {5, 9, 0});
	t.bundle[Patterns] = prefixed(2, {0});
	t.bundle[XMotion] = identity();
	t.bundle[YMotion] = prefixed(4, {3});
	t.bundle[Runs] = merged(10);
	t.high[0] = merged(3);
	t.high[9] = prefixed(13, {8});
	return t;
}

// --- the planes as built, and what they decode to ----------------------------------------------------

// A plane's pixels as expected: -1 where the construction leaves a value to the decoder (an AC DCT).
struct PlaneModel {
	uint32_t width = 0, height = 0;
	std::vector<int16_t> px;
	void allocate(uint32_t w, uint32_t h) {
		width = w;
		height = h;
		px.assign(static_cast<size_t>(w) * h, 0);
	}
	int16_t at(int x, int y) const { return px[static_cast<size_t>(y) * width + x]; }
	int16_t &at(int x, int y) { return px[static_cast<size_t>(y) * width + x]; }
};

struct Run {
	unsigned length;
	bool repeat;
	std::vector<uint8_t> colors;  // one when `repeat`, else `length`
};

enum class Kind { Copy, Scaled, ScaledSkip, Motion, RunBlock, Residue, IntraDct, Fill, InterDct, Pattern, Raw };

struct Block {
	Kind kind = Kind::Copy;
	int subtype = 0;  // a Scaled block's inner kind: 3 run, 5 DCT, 6 fill, 8 pattern, 9 raw
	uint8_t color = 0;
	std::array<uint8_t, 64> raw{};
	uint8_t c0 = 0, c1 = 0;
	std::array<uint8_t, 8> pattern{};
	unsigned scan = 0;
	std::vector<Run> runs;
	uint8_t final_color = 0;  // the 64th pixel of runs ending at 63
	int dx = 0, dy = 0;
	int dc = 0;
	unsigned q = 0;
	bool ac = false;  // a DCT with one AC coefficient, 15 at the first coded place after the DC
};

Block copy() { return Block{}; }
Block fill(uint8_t c) {
	Block b;
	b.kind = Kind::Fill;
	b.color = c;
	return b;
}
Block raw(int seed) {
	Block b;
	b.kind = Kind::Raw;
	for (int i = 0; i < 64; ++i) b.raw[i] = static_cast<uint8_t>(20 + (i * 7 + seed * 13) % 200);
	return b;
}
Block pattern(uint8_t c0, uint8_t c1, int seed) {
	Block b;
	b.kind = Kind::Pattern;
	b.c0 = c0;
	b.c1 = c1;
	for (int i = 0; i < 8; ++i) b.pattern[i] = static_cast<uint8_t>((i * 37 + seed * 91) & 0xff);
	return b;
}
Block run_block(unsigned scan, std::vector<Run> runs, uint8_t final_color = 0) {
	Block b;
	b.kind = Kind::RunBlock;
	b.scan = scan;
	b.runs = std::move(runs);
	b.final_color = final_color;
	return b;
}
Run repeat_run(unsigned length, uint8_t c) { return Run{length, true, {c}}; }
Run spread_run(unsigned length, uint8_t first) {
	Run r{length, false, {}};
	for (unsigned i = 0; i < length; ++i) r.colors.push_back(static_cast<uint8_t>(first + i * 3));
	return r;
}
Block motion(int dx, int dy) {
	Block b;
	b.kind = Kind::Motion;
	b.dx = dx;
	b.dy = dy;
	return b;
}
Block residue(int dx, int dy) {
	Block b = motion(dx, dy);
	b.kind = Kind::Residue;
	return b;
}
Block intra(int dc, unsigned q, bool ac = false) {
	Block b;
	b.kind = Kind::IntraDct;
	b.dc = dc;
	b.q = q;
	b.ac = ac;
	return b;
}
Block inter(int dx, int dy, int dc, unsigned q) {
	Block b = motion(dx, dy);
	b.kind = Kind::InterDct;
	b.dc = dc;
	b.q = q;
	return b;
}
Block scaled(Block inner) {
	inner.subtype = inner.kind == Kind::RunBlock ? 3 : inner.kind == Kind::IntraDct ? 5 :
	                inner.kind == Kind::Fill ? 6 : inner.kind == Kind::Pattern ? 8 : 9;
	inner.kind = Kind::Scaled;
	return inner;
}
Block scaled_skip() {
	Block b;
	b.kind = Kind::ScaledSkip;
	return b;
}

struct PlaneSpec {
	uint32_t width = 0, height = 0;
	std::vector<std::vector<Block>> rows;
	std::array<bool, kBundles> per_row{};  // refilled row by row, else once for the plane
};

using Inline = std::vector<std::pair<uint32_t, unsigned>>;  // a row's in-block bits, in order

// The DC-only DCT's one value over its block: the DC dequantized (dc * quant >> 11), then the inverse
// transform, which carries a lone DC through both passes, and its row pass's rounding ((v + 127) >> 8).
int dc_only_pixel(int dc, uint32_t quant) {
	const int32_t v = dc == 0 ? 0 : floor_shift(static_cast<int64_t>(dc) * quant, 11);
	return floor_shift(static_cast<int64_t>(v) + 0x7f, 8);
}

class PlaneEncoder {
public:
	PlaneEncoder(const PlaneSpec &spec, const Trees &trees, const PlaneModel &reference, PlaneModel &out)
	    : spec_(spec), trees_(trees), ref_(reference), out_(out) {
		vals_.assign(kBundles, std::vector<std::vector<int32_t>>(spec.rows.size()));
		inline_.assign(spec.rows.size(), Inline());
	}

	void encode(BitWriter &w) {
		for (size_t r = 0; r < spec_.rows.size(); ++r) {
			unsigned bx = 0;
			for (const Block &b : spec_.rows[r]) bx += block(b, r, bx);
		}
		trees_.write(w);
		std::array<size_t, kBundles> offered{};
		std::array<bool, kBundles> ended{};
		std::array<size_t, kBundles> consumed{};
		for (size_t r = 0; r < spec_.rows.size(); ++r) {
			for (int k = 0; k < kBundles; ++k) {
				if (!ended[k] && offered[k] == consumed[k]) {
					std::vector<int32_t> batch;
					for (size_t at = r; at < spec_.rows.size(); ++at) {
						batch.insert(batch.end(), vals_[k][at].begin(), vals_[k][at].end());
						if (spec_.per_row[k] && !batch.empty()) break;
					}
					w.write(static_cast<uint32_t>(batch.size()), length_bits(k));
					if (batch.empty()) ended[k] = true;
					else write_batch(w, k, batch);
					offered[k] += batch.size();
				}
				consumed[k] += vals_[k][r].size();
			}
			for (const auto &bits : inline_[r]) w.write(bits.first, bits.second);
		}
		w.align32();
	}

private:
	unsigned length_bits(int k) const {
		const uint32_t block_width = (spec_.width + 7) >> 3;
		const uint32_t aligned = (std::max(spec_.width, 8U) + 7) & ~7U;
		switch (k) {
		case SubTypes: return bit_width((aligned >> 4) + 511);
		case Colors: return bit_width(block_width * 64 + 511);
		case Patterns: return bit_width(block_width * 8 + 511);
		case Runs: return bit_width(block_width * 48 + 511);
		default: return bit_width((aligned >> 3) + 511);
		}
	}

	void write_batch(BitWriter &w, int k, const std::vector<int32_t> &v) {
		const bool same = v.size() > 1 && std::all_of(v.begin(), v.end(), [&](int32_t x) { return x == v[0]; });
		const TreeSpec &tree = trees_.bundle[k];
		switch (k) {
		case BlockTypes:
		case SubTypes:
			w.write(same, 1);
			if (same) {
				w.write(static_cast<uint32_t>(v[0]), 4);
				return;
			}
			for (size_t i = 0; i < v.size();) {
				// A run of the last value: the longest of 32, 12, 8, 4 it holds (codes 15, 14, 13, 12).
				if (i > 0) {
					size_t run = 0;
					while (i + run < v.size() && v[i + run] == v[i - 1]) ++run;
					static constexpr unsigned kRuns[4] = {4, 8, 12, 32};
					int code = -1;
					for (int c = 3; c >= 0; --c)
						if (run >= kRuns[c]) {
							code = c;
							break;
						}
					if (code >= 0) {
						tree.emit(w, static_cast<uint8_t>(12 + code));
						i += kRuns[code];
						continue;
					}
				}
				tree.emit(w, static_cast<uint8_t>(v[i]));
				++i;
			}
			return;
		case Colors:
			w.write(same, 1);
			for (size_t i = 0; i < (same ? 1 : v.size()); ++i) {
				const uint8_t c = static_cast<uint8_t>(v[i]);
				trees_.high[last_high_].emit(w, static_cast<uint8_t>(c >> 4));
				tree.emit(w, static_cast<uint8_t>(c & 15));
				last_high_ = c >> 4;
			}
			return;
		case Patterns:
			for (int32_t p : v) {
				tree.emit(w, static_cast<uint8_t>(p & 15));
				tree.emit(w, static_cast<uint8_t>((p >> 4) & 15));
			}
			return;
		case XMotion:
		case YMotion:
			w.write(same, 1);
			for (size_t i = 0; i < (same ? 1 : v.size()); ++i) {
				const uint32_t magnitude = static_cast<uint32_t>(std::abs(v[i]));
				if (same) w.write(magnitude, 4);
				else tree.emit(w, static_cast<uint8_t>(magnitude));
				if (magnitude != 0) w.write(v[i] < 0, 1);
			}
			return;
		case IntraDc:
		case InterDc: {
			const bool sign = k == InterDc;
			w.write(static_cast<uint32_t>(std::abs(v[0])), sign ? 10 : 11);
			if (sign && v[0] != 0) w.write(v[0] < 0, 1);
			for (size_t base = 1; base < v.size(); base += 8) {
				const size_t group = std::min<size_t>(8, v.size() - base);
				uint32_t widest = 0;
				for (size_t i = 0; i < group; ++i)
					widest = std::max<uint32_t>(widest, static_cast<uint32_t>(std::abs(v[base + i] - v[base + i - 1])));
				const unsigned bits = widest == 0 ? 0 : bit_width(widest);
				w.write(bits, 4);
				for (size_t i = 0; i < group && bits != 0; ++i) {
					const int32_t delta = v[base + i] - v[base + i - 1];
					w.write(static_cast<uint32_t>(std::abs(delta)), bits);
					if (delta != 0) w.write(delta < 0, 1);
				}
			}
			return;
		}
		case Runs:
			w.write(same, 1);
			if (same) {
				w.write(static_cast<uint32_t>(v[0]), 4);
				return;
			}
			for (int32_t x : v) tree.emit(w, static_cast<uint8_t>(x));
			return;
		}
	}

	void push(int k, size_t r, int32_t v) { vals_[k][r].push_back(v); }

	// An 8x8 block's expected pixels from its own values (the reference for the motion kinds), and its
	// bundle values and in-block bits; `cells` its 64 pixels.
	void inner(const Block &b, size_t r, std::array<int16_t, 64> &cells, int x0, int y0) {
		Inline &bits = inline_[r];
		switch (b.kind == Kind::Scaled ? (b.subtype == 3 ? Kind::RunBlock : b.subtype == 5 ? Kind::IntraDct :
		                                  b.subtype == 8 ? Kind::Pattern : Kind::Raw) : b.kind) {
		case Kind::Copy:
			for (int i = 0; i < 64; ++i) cells[i] = ref_.at(x0 + i % 8, y0 + i / 8);
			break;
		case Kind::Motion:
		case Kind::Residue:
		case Kind::InterDct:
			push(XMotion, r, b.dx);
			push(YMotion, r, b.dy);
			for (int i = 0; i < 64; ++i) cells[i] = ref_.at(x0 + b.dx + i % 8, y0 + b.dy + i / 8);
			if (b.kind == Kind::Residue) {
				// One event: the walk's list (4,0) (24,0) (44,0) idle, (0,2) active, its first two places
				// placed at mask 1 (+1 at scan 0, -1 at scan 1), the second ending the walk.
				bits.push_back({1, 7});
				bits.push_back({0, 3});
				for (uint32_t bit : {0u, 0u, 0u, 1u, 0u, 0u, 0u, 1u}) bits.push_back({bit, 1});
				const int at0 = detail::kCoefficientScan[0], at1 = detail::kCoefficientScan[1];
				if (cells[at0] >= 0) cells[at0] = static_cast<int16_t>((cells[at0] + 1) & 0xff);
				if (cells[at1] >= 0) cells[at1] = static_cast<int16_t>((cells[at1] - 1) & 0xff);
			}
			if (b.kind == Kind::InterDct) {
				push(InterDc, r, b.dc);
				bits.push_back({0, 4});
				bits.push_back({b.q, 4});
				const int add = dc_only_pixel(b.dc, detail::kInterQuant[b.q][0]);
				for (int16_t &c : cells)
					if (c >= 0) c = static_cast<int16_t>((c + add) & 0xff);
			}
			break;
		case Kind::RunBlock: {
			bits.push_back({b.scan, 4});
			unsigned filled = 0;
			for (const Run &run : b.runs) {
				push(Runs, r, static_cast<int32_t>(run.length - 1));
				bits.push_back({run.repeat ? 1u : 0u, 1});
				for (uint8_t c : run.colors) push(Colors, r, c);
				for (unsigned i = 0; i < run.length; ++i)
					cells[detail::kPatternScans[b.scan][filled++]] = run.repeat ? run.colors[0] : run.colors[i];
			}
			if (filled == 63) {
				push(Colors, r, b.final_color);
				cells[detail::kPatternScans[b.scan][63]] = b.final_color;
			}
			break;
		}
		case Kind::IntraDct:
			push(IntraDc, r, b.dc);
			if (b.ac) {
				// Four magnitude planes: at bit 3 the list (4,0) (24,0) (44,0) idle, (1,3) active (low bits
				// 7 and sign +: the coefficient 15), (2,3) (3,3) idle; the three lower planes all idle (the
				// placed entry cleared, five left to answer).
				bits.push_back({4, 4});
				for (uint32_t bit : {0u, 0u, 0u, 1u}) bits.push_back({bit, 1});
				bits.push_back({7, 3});
				for (int i = 0; i < 3; ++i) bits.push_back({0, 1});
				for (int i = 0; i < 15; ++i) bits.push_back({0, 1});
				std::fill(cells.begin(), cells.end(), int16_t(-1));
			} else {
				bits.push_back({0, 4});
				std::fill(cells.begin(), cells.end(),
				          static_cast<int16_t>(dc_only_pixel(b.dc, detail::kIntraQuant[b.q][0]) & 0xff));
			}
			bits.push_back({b.q, 4});
			break;
		case Kind::Fill:
			push(Colors, r, b.color);
			std::fill(cells.begin(), cells.end(), b.color);
			break;
		case Kind::Pattern:
			push(Colors, r, b.c0);
			push(Colors, r, b.c1);
			for (int y = 0; y < 8; ++y) {
				push(Patterns, r, b.pattern[y]);
				for (int x = 0; x < 8; ++x) cells[y * 8 + x] = (b.pattern[y] >> x) & 1 ? b.c1 : b.c0;
			}
			break;
		case Kind::Raw:
			for (int i = 0; i < 64; ++i) {
				push(Colors, r, b.raw[i]);
				cells[i] = b.raw[i];
			}
			break;
		default:
			break;
		}
	}

	// One block spec at block column `bx` of row `r`: its type and values, its pixels; the columns it
	// covers.
	unsigned block(const Block &b, size_t r, unsigned bx) {
		const int x0 = static_cast<int>(bx) * 8, y0 = static_cast<int>(r) * 8;
		static constexpr int kTypes[] = {0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9};
		push(BlockTypes, r, kTypes[static_cast<int>(b.kind)]);
		if (b.kind == Kind::ScaledSkip) return 2;
		std::array<int16_t, 64> cells{};
		if (b.kind == Kind::Scaled) {
			push(SubTypes, r, b.subtype);
			if (b.subtype == 6) {
				push(Colors, r, b.color);
				std::fill(cells.begin(), cells.end(), b.color);
			} else {
				inner(b, r, cells, x0, y0);
			}
			for (int i = 0; i < 64; ++i)
				for (int dy = 0; dy < 2; ++dy)
					for (int dx = 0; dx < 2; ++dx) out_.at(x0 + (i % 8) * 2 + dx, y0 + (i / 8) * 2 + dy) = cells[i];
			return 2;
		}
		inner(b, r, cells, x0, y0);
		for (int i = 0; i < 64; ++i) out_.at(x0 + i % 8, y0 + i / 8) = cells[i];
		return 1;
	}

	const PlaneSpec &spec_;
	const Trees &trees_;
	const PlaneModel &ref_;
	PlaneModel &out_;
	std::vector<std::vector<std::vector<int32_t>>> vals_;
	std::vector<Inline> inline_;
	uint8_t last_high_ = 0;
};

// --- the movie ----------------------------------------------------------------------------------------

constexpr uint32_t kWidth = 64, kHeight = 32;

struct Frame {
	PlaneSpec y, u, v;
};

PlaneSpec chroma(std::vector<std::vector<Block>> rows) {
	PlaneSpec p;
	p.width = kWidth / 2;
	p.height = kHeight / 2;
	p.rows = std::move(rows);
	return p;
}

std::vector<Frame> the_frames() {
	std::vector<Frame> frames(2);
	// Frame 0: every intra kind; the block types and colors refilled row by row.
	Frame &f0 = frames[0];
	f0.y.width = kWidth;
	f0.y.height = kHeight;
	f0.y.per_row[BlockTypes] = true;
	f0.y.per_row[Colors] = true;
	f0.y.rows = {
	        {scaled(raw(1)), scaled(pattern(40, 200, 1)),
	         scaled(run_block(5, {repeat_run(16, 60), spread_run(16, 61), repeat_run(16, 90), repeat_run(16, 120)})),
	         scaled(fill(180))},
	        {scaled_skip(), scaled_skip(), scaled_skip(), scaled_skip()},
	        {scaled(intra(700, 1)), intra(1024, 0), intra(1024, 0, true), fill(33), raw(2), pattern(50, 150, 2),
	         run_block(12, {spread_run(16, 30), repeat_run(16, 77), repeat_run(16, 88), spread_run(15, 100)}, 222)},
	        {scaled_skip(), run_block(0, {repeat_run(16, 70), repeat_run(16, 80), repeat_run(16, 90), repeat_run(16, 100)}),
	         fill(200), fill(201), fill(202), raw(3), fill(33)},
	};
	// The chroma: fills, the block types in one repeat, V's colors apart, U's one repeated colour.
	std::vector<std::vector<Block>> v_rows(2), u_rows(2);
	for (int r = 0; r < 2; ++r)
		for (int c = 0; c < 4; ++c) {
			v_rows[r].push_back(fill(static_cast<uint8_t>(140 + (r * 4 + c) * 10)));
			u_rows[r].push_back(fill(110));
		}
	f0.v = chroma(v_rows);
	f0.u = chroma(u_rows);

	// Frame 1: every inter kind over frame 0, the block types in one batch with run codes, the motion
	// refilled row by row.
	Frame &f1 = frames[1];
	f1.y.width = kWidth;
	f1.y.height = kHeight;
	f1.y.per_row[XMotion] = true;
	f1.y.per_row[YMotion] = true;
	f1.y.rows = {
	        {copy(), copy(), copy(), copy(), copy(), motion(3, 2), motion(-4, 1), residue(-8, 0)},
	        {inter(0, 0, -64, 0), inter(1, -1, 40, 2), copy(), copy(), copy(), copy(), copy(), copy()},
	        {scaled(raw(4)), scaled(fill(77)), raw(5), pattern(60, 160, 3), fill(44), motion(0, -5)},
	        {scaled_skip(), scaled_skip(),
	         run_block(15, {spread_run(16, 20), repeat_run(16, 40), spread_run(16, 120), repeat_run(16, 210)}),
	         intra(512, 3), copy(), copy()},
	};
	// The chroma: motion alone, x 0 in one repeat, y +3 then -3 in a repeat a row.
	std::vector<std::vector<Block>> m_rows(2);
	for (int r = 0; r < 2; ++r)
		for (int c = 0; c < 4; ++c) m_rows[r].push_back(motion(0, r == 0 ? 3 : -3));
	f1.v = chroma(m_rows);
	f1.u = chroma(m_rows);
	f1.v.per_row[YMotion] = true;
	f1.u.per_row[YMotion] = true;
	return frames;
}

struct Built {
	std::vector<uint8_t> file;
	std::vector<std::array<PlaneModel, 3>> expected;  // Y, U, V per frame
};

void put32(std::vector<uint8_t> &out, uint32_t v) {
	for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void set32(std::vector<uint8_t> &out, size_t at, uint32_t v) {
	for (int i = 0; i < 4; ++i) out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

Built build_movie(size_t frame_count) {
	const Trees trees = the_trees();
	const std::vector<Frame> frames = the_frames();
	Built built;
	std::array<PlaneModel, 3> previous;
	previous[0].allocate(kWidth, kHeight);
	previous[1].allocate(kWidth / 2, kHeight / 2);
	previous[2].allocate(kWidth / 2, kHeight / 2);
	std::vector<std::vector<uint8_t>> packets;
	for (size_t f = 0; f < frame_count; ++f) {
		std::array<PlaneModel, 3> current = previous;
		BitWriter w;
		w.write(0, 32);  // the video packet's prefix
		PlaneEncoder(frames[f].y, trees, previous[0], current[0]).encode(w);
		PlaneEncoder(frames[f].v, trees, previous[2], current[2]).encode(w);  // V before U
		PlaneEncoder(frames[f].u, trees, previous[1], current[1]).encode(w);
		packets.push_back(w.bytes());
		built.expected.push_back(current);
		previous = current;
	}
	const uint32_t table = 44 + 4 * static_cast<uint32_t>(packets.size());
	uint32_t size = table, largest = 0;
	for (const auto &p : packets) {
		size += static_cast<uint32_t>(p.size());
		largest = std::max<uint32_t>(largest, static_cast<uint32_t>(p.size()));
	}
	std::vector<uint8_t> &file = built.file;
	put32(file, 0x694b4942U);  // BIKi
	put32(file, size - 8);
	put32(file, static_cast<uint32_t>(packets.size()));
	put32(file, largest);
	put32(file, static_cast<uint32_t>(packets.size()));
	put32(file, kWidth);
	put32(file, kHeight);
	put32(file, 25);
	put32(file, 1);
	put32(file, 0);  // flags
	put32(file, 0);  // audio tracks
	uint32_t offset = table;
	for (size_t i = 0; i < packets.size(); ++i) {
		put32(file, offset | (i == 0 ? 1U : 0U));  // the key frame's flag bit, masked off by the reader
		offset += static_cast<uint32_t>(packets[i].size());
	}
	for (const auto &p : packets) file.insert(file.end(), p.begin(), p.end());
	return built;
}

std::unique_ptr<BinkMovie> open_bytes(const std::vector<uint8_t> &bytes, std::string &error) {
	auto held = std::make_shared<std::vector<uint8_t>>(bytes);
	opennova::bink::BinkSource source;
	source.size = held->size();
	source.read_at = [held](uint64_t offset, uint8_t *destination, size_t size) {
		if (offset > held->size() || size > held->size() - offset) return false;
		std::memcpy(destination, held->data() + offset, size);
		return true;
	};
	return BinkMovie::open(std::move(source), &error);
}

// --- the stages ---------------------------------------------------------------------------------------

// The header and its refusals, each field out of bounds alone.
void test_header() {
	const Built built = build_movie(1);
	std::string error;
	auto movie = open_bytes(built.file, error);
	if (!expect(movie != nullptr, "the built movie opens: " + error)) return;
	const auto &info = movie->info();
	expect(info.width == kWidth && info.height == kHeight && info.frame_count == 1 && info.fps_numerator == 25 &&
	               info.fps_denominator == 1 && info.frames_per_second() == 25.0,
	       "the header's fields read back");

	struct Refusal {
		size_t at;
		uint32_t value;
		const char *message;
	};
	const uint32_t size = static_cast<uint32_t>(built.file.size());
	const Refusal refusals[] = {
	        {4, size, "declared Bink file size is out of bounds"},
	        {8, 0, "frame count is invalid"},
	        {20, 0, "dimensions are invalid"},
	        {24, 16385, "dimensions are invalid"},
	        {32, 0, "frame rate is invalid"},
	        {12, size + 1, "largest Bink frame exceeds the file size"},
	        {36, 4, "alpha, grayscale"},
	        {40, 1, "audio tracks are not supported"},
	        {44, 20 | 1U, "frame index is invalid"},
	        {12, 8, "packet exceeds the declared largest frame"},
	        {20, 1, "too small for YUV 4:2:0"},
	};
	for (const Refusal &r : refusals) {
		std::vector<uint8_t> bytes = built.file;
		set32(bytes, r.at, r.value);
		error.clear();
		const auto refused = open_bytes(bytes, error);
		expect(refused == nullptr && error.find(r.message) != std::string::npos,
		       std::string("a header refused: ") + r.message + " (got \"" + error + "\")");
	}
	std::vector<uint8_t> bytes = built.file;
	bytes[3] = 'j';
	expect(open_bytes(bytes, error) == nullptr && error.find("BIKi") != std::string::npos, "a non-BIKi magic refused");
}

// A packet cut short (the declared size ending it 16 bytes early, inside its last plane): invalid data,
// the frame index where it was.
void test_truncated_packet() {
	const Built built = build_movie(1);
	std::vector<uint8_t> cut = built.file;
	set32(cut, 4, static_cast<uint32_t>(cut.size()) - 8 - 16);
	std::string error;
	auto movie = open_bytes(cut, error);
	if (!expect(movie != nullptr, "the cut movie opens: " + error)) return;
	expect(movie->decode_next() == BinkStatus::invalid_data && movie->frame_index() == 0 &&
	               movie->last_error().find("malformed") != std::string::npos,
	       "a packet cut short is refused and the frame index stays");
}

// The two frames decoded: each pixel the construction fixes equals the retail YUV law over the expected
// planes; the AC block's rows alike (an AC term on the first row of coefficients only); the frames
// pinned.
void test_frames() {
	const Built built = build_movie(2);
	std::string error;
	auto movie = open_bytes(built.file, error);
	if (!expect(movie != nullptr, "the built movie opens: " + error)) return;
	static constexpr uint64_t kPinned[2] = {0xd63a81f7b45a0bc3ull, 0x0f016786b961c7b0ull};
	for (size_t f = 0; f < built.expected.size(); ++f) {
		if (!expect(movie->decode_next() == BinkStatus::frame_ready, "frame decodes: " + movie->last_error())) return;
		const auto &frame = movie->frame();
		const auto &e = built.expected[f];
		size_t checked = 0, wrong = 0;
		for (uint32_t y = 0; y < kHeight; ++y)
			for (uint32_t x = 0; x < kWidth; ++x) {
				const int16_t luma = e[0].at(x, y), u = e[1].at(x / 2, y / 2), v = e[2].at(x / 2, y / 2);
				if (luma < 0 || u < 0 || v < 0) continue;
				uint8_t r = 0, g = 0, b = 0;
				opennova::bink::yuv_to_rgb(static_cast<uint8_t>(luma), static_cast<uint8_t>(u), static_cast<uint8_t>(v),
				                           r, g, b);
				const uint8_t *px = frame.rgba.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
				++checked;
				if (px[0] != r || px[1] != g || px[2] != b || px[3] != 255) {
					if (wrong++ < 4)
						std::fprintf(stderr, "frame %zu (%u, %u): decoded %u %u %u, expected Y %d U %d V %d -> %u %u %u\n", f,
						             x, y, px[0], px[1], px[2], luma, u, v, r, g, b);
				}
			}
		expect(wrong == 0, "frame " + std::to_string(f) + ": every constructed pixel decodes as built (" +
		                           std::to_string(wrong) + " of " + std::to_string(checked) + " apart)");
		if (f == 0) {
			// The AC block (block 3 of row 2): its eight rows alike, its columns not all alike.
			const auto row_of = [&](uint32_t y) {
				std::vector<uint8_t> out;
				for (uint32_t x = 24; x < 32; ++x) out.push_back(frame.rgba[(y * kWidth + x) * 4]);
				return out;
			};
			bool rows_alike = true;
			for (uint32_t y = 17; y < 24; ++y) rows_alike = rows_alike && row_of(y) == row_of(16);
			const std::vector<uint8_t> first = row_of(16);
			expect(rows_alike && *std::min_element(first.begin(), first.end()) != *std::max_element(first.begin(), first.end()),
			       "an AC term of the first coefficient row varies across the block, not down it");
		}
		const uint64_t hash = fnv1a64(frame.rgba);
		std::printf("frame %zu: %zu pixels as built, rgba fnv1a64 0x%016llx\n", f, checked, (unsigned long long)hash);
		expect(hash == kPinned[f], "frame " + std::to_string(f) + " matches its pinned FNV-1a64 vector");
	}
	expect(movie->decode_next() == BinkStatus::end_of_stream, "the two-frame stream ends");
}

} // namespace

int main() {
	test_header();
	test_truncated_packet();
	test_frames();
	if (failures == 0) std::printf("bink_stages: header, bit reader, trees, bundles, every block kind and the DCT as built\n");
	return failures == 0 ? 0 : 1;
}
