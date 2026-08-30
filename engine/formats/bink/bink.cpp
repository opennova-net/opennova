#include <formats/bink/bink.h>

#include <formats/bink/bink_tables.h>

#include <base/io/le.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace opennova::bink {
namespace {

// The original is RAD's binkw32.dll 1.5u (the DLL Jointops.exe imports; a
// separate image from Jointops.exe, imagebase 0x30000000 -- every address in
// this file is that image's unless it says otherwise):
// [orig: BinkVideo_DecodeFrame @ 0x3001F260, binkw32.dll] the per-frame
// entry BinkDoFrame reaches, and [orig: BinkVideo_DecodePlane @ 0x3001D2C0,
// binkw32.dll] the per-plane bundle/block decoder every routine in the
// anonymous namespace below is a structural translation of (the bundle
// readers, block decoders and residue walk are its inlined legs; the IDCT is
// its callee sub_3001F3E0). The BIKi bitstream layout is the decoder's
// contract with the shipped .bik files, so the file is the witness of the
// bit-level shapes below.
constexpr uint32_t kBikiMagic = 0x694b4942U;
constexpr uint32_t kHeaderSize = 44;
constexpr uint32_t kMaximumDimension = 16384;
constexpr uint32_t kMaximumFrames = 1000000;

uint32_t read_le32(const uint8_t *bytes) {
	return io::read_u32_le(bytes);
}

uint32_t align_up(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

unsigned bit_width(uint32_t value) {
	unsigned bits = 0;
	do {
		++bits;
		value >>= 1;
	} while (value != 0);
	return bits;
}

class BitReader {
public:
	explicit BitReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}

	bool read(unsigned count, uint32_t &value) {
		if (count > 32 || count > remaining()) {
			ok_ = false;
			value = 0;
			return false;
		}
		value = 0;
		for (unsigned i = 0; i < count; ++i) {
			const size_t bit = bit_offset_ + i;
			value |= ((bytes_[bit >> 3] >> (bit & 7)) & 1U) << i;
		}
		bit_offset_ += count;
		return true;
	}

	bool read_bit(bool &value) {
		uint32_t bit = 0;
		if (!read(1, bit)) {
			return false;
		}
		value = bit != 0;
		return true;
	}

	bool peek(unsigned count, uint32_t &value) {
		const size_t saved = bit_offset_;
		const bool result = read(count, value);
		bit_offset_ = saved;
		return result;
	}

	bool skip(size_t count) {
		if (count > remaining()) {
			ok_ = false;
			return false;
		}
		bit_offset_ += count;
		return true;
	}

	bool align32() {
		const size_t padding = (32 - (bit_offset_ & 31)) & 31;
		return skip(padding);
	}

	size_t remaining() const {
		const size_t total = bytes_.size() * 8;
		return bit_offset_ <= total ? total - bit_offset_ : 0;
	}

	bool ok() const { return ok_; }

private:
	const std::vector<uint8_t> &bytes_;
	size_t bit_offset_ = 0;
	bool ok_ = true;
};

struct Tree {
	uint8_t number = 0;
	std::array<uint8_t, 16> symbols{};
};

bool merge_tree_lists(BitReader &bits, uint8_t *destination,
		const uint8_t *first, unsigned count) {
	const uint8_t *second = first + count;
	unsigned first_left = count;
	unsigned second_left = count;
	while (first_left != 0 && second_left != 0) {
		bool choose_second = false;
		if (!bits.read_bit(choose_second)) {
			return false;
		}
		if (choose_second) {
			*destination++ = *second++;
			--second_left;
		} else {
			*destination++ = *first++;
			--first_left;
		}
	}
	while (first_left-- != 0) {
		*destination++ = *first++;
	}
	while (second_left-- != 0) {
		*destination++ = *second++;
	}
	return true;
}

bool read_tree(BitReader &bits, Tree &tree) {
	uint32_t number = 0;
	if (!bits.read(4, number)) {
		return false;
	}
	tree.number = static_cast<uint8_t>(number);
	if (number == 0) {
		for (uint8_t i = 0; i < 16; ++i) {
			tree.symbols[i] = i;
		}
		return true;
	}

	bool explicit_prefix = false;
	if (!bits.read_bit(explicit_prefix)) {
		return false;
	}
	if (explicit_prefix) {
		uint32_t last_explicit = 0;
		if (!bits.read(3, last_explicit)) {
			return false;
		}
		std::array<bool, 16> seen{};
		unsigned output = 0;
		for (; output <= last_explicit; ++output) {
			uint32_t symbol = 0;
			if (!bits.read(4, symbol) || seen[symbol]) {
				return false;
			}
			tree.symbols[output] = static_cast<uint8_t>(symbol);
			seen[symbol] = true;
		}
		for (uint8_t symbol = 0; symbol < 16; ++symbol) {
			if (!seen[symbol]) {
				tree.symbols[output++] = symbol;
			}
		}
		return output == 16;
	}

	uint32_t last_depth = 0;
	if (!bits.read(2, last_depth)) {
		return false;
	}
	std::array<uint8_t, 16> first{};
	std::array<uint8_t, 16> second{};
	for (uint8_t i = 0; i < 16; ++i) {
		first[i] = i;
	}
	uint8_t *input = first.data();
	uint8_t *output = second.data();
	for (unsigned depth = 0; depth <= last_depth; ++depth) {
		const unsigned count = 1U << depth;
		for (unsigned start = 0; start < 16; start += count * 2) {
			if (!merge_tree_lists(bits, output + start, input + start, count)) {
				return false;
			}
		}
		std::swap(input, output);
	}
	std::copy(input, input + 16, tree.symbols.begin());
	return true;
}

bool read_huffman(BitReader &bits, const Tree &tree, uint8_t &value) {
	for (unsigned length = 1; length <= 7; ++length) {
		if (bits.remaining() < length) {
			break;
		}
		uint32_t code = 0;
		if (!bits.peek(length, code)) {
			return false;
		}
		for (unsigned leaf = 0; leaf < 16; ++leaf) {
			if (detail::kTreeLengths[tree.number][leaf] == length &&
					detail::kTreeCodes[tree.number][leaf] == code) {
				if (!bits.skip(length)) {
					return false;
				}
				value = tree.symbols[leaf];
				return true;
			}
		}
	}
	return false;
}

enum class BundleKind : unsigned {
	block_types,
	sub_block_types,
	colors,
	patterns,
	x_motion,
	y_motion,
	intra_dc,
	inter_dc,
	runs,
	count,
};

constexpr unsigned bundle_index(BundleKind kind) {
	return static_cast<unsigned>(kind);
}

struct Bundle {
	unsigned length_bits = 0;
	size_t capacity = 0;
	Tree tree;
	std::vector<int32_t> values;
	size_t cursor = 0;
	bool ended = false;

	void reset() {
		values.clear();
		cursor = 0;
		ended = false;
	}
};

struct Plane {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	uint32_t rows = 0;
	std::vector<uint8_t> pixels;

	void allocate(uint32_t new_width, uint32_t new_height) {
		width = new_width;
		height = new_height;
		stride = align_up(std::max(new_width, 1U), 8);
		rows = align_up(std::max(new_height, 1U), 8);
		pixels.assign(static_cast<size_t>(stride) * rows, 0);
	}
};

int32_t arithmetic_shift_right(int32_t value, unsigned count) {
	if (value >= 0) {
		return value >> count;
	}
	const int64_t magnitude = -static_cast<int64_t>(value);
	return static_cast<int32_t>(-((magnitude + (int64_t{1} << count) - 1) >> count));
}

int32_t wrap_add(int32_t first, int32_t second) {
	return static_cast<int32_t>(static_cast<uint32_t>(first) +
			static_cast<uint32_t>(second));
}

int32_t wrap_subtract(int32_t first, int32_t second) {
	return static_cast<int32_t>(static_cast<uint32_t>(first) -
			static_cast<uint32_t>(second));
}

// The 8-point integer DCT butterfly the plane decoder calls per block
// [orig: sub_3001F3E0 @ 0x3001F3E0, binkw32.dll -- the 0xB50 / 0xEC8 / 0x8A9
// (2896 / 3784 / 2217) multipliers at its imul sites, the -5352 pair being the
// negated 0x14E8 term].
int32_t multiply_shift_11(int32_t first, int32_t second) {
	const uint32_t product = static_cast<uint32_t>(first) *
			static_cast<uint32_t>(second);
	return arithmetic_shift_right(static_cast<int32_t>(product), 11);
}

void idct_transform(const int32_t *source, int source_step,
		int32_t *destination, int destination_step, bool round_row) {
	const int32_t a0 = wrap_add(source[0 * source_step], source[4 * source_step]);
	const int32_t a1 = wrap_subtract(source[0 * source_step], source[4 * source_step]);
	const int32_t a2 = wrap_add(source[2 * source_step], source[6 * source_step]);
	const int32_t a3 = multiply_shift_11(2896,
			wrap_subtract(source[2 * source_step], source[6 * source_step]));
	const int32_t a4 = wrap_add(source[5 * source_step], source[3 * source_step]);
	const int32_t a5 = wrap_subtract(source[5 * source_step], source[3 * source_step]);
	const int32_t a6 = wrap_add(source[1 * source_step], source[7 * source_step]);
	const int32_t a7 = wrap_subtract(source[1 * source_step], source[7 * source_step]);
	const int32_t b0 = wrap_add(a4, a6);
	const int32_t b1 = multiply_shift_11(3784, wrap_add(a5, a7));
	const int32_t b2 = wrap_add(wrap_subtract(multiply_shift_11(-5352, a5), b0), b1);
	const int32_t b3 = wrap_subtract(multiply_shift_11(2896, wrap_subtract(a6, a4)), b2);
	const int32_t b4 = wrap_subtract(wrap_add(multiply_shift_11(2217, a7), b3), b1);

	std::array<int32_t, 8> output = {
		wrap_add(wrap_add(a0, a2), b0),
		wrap_add(wrap_subtract(wrap_add(a1, a3), a2), b2),
		wrap_add(wrap_add(wrap_subtract(a1, a3), a2), b3),
		wrap_subtract(wrap_subtract(a0, a2), b4),
		wrap_add(wrap_subtract(a0, a2), b4),
		wrap_subtract(wrap_add(wrap_subtract(a1, a3), a2), b3),
		wrap_subtract(wrap_subtract(wrap_add(a1, a3), a2), b2),
		wrap_subtract(wrap_add(a0, a2), b0),
	};
	for (unsigned i = 0; i < output.size(); ++i) {
		if (round_row) {
			output[i] = arithmetic_shift_right(wrap_add(output[i], 0x7f), 8);
		}
		destination[i * destination_step] = output[i];
	}
}

void inverse_dct(std::array<int32_t, 64> &block) {
	std::array<int32_t, 64> temporary{};
	for (unsigned column = 0; column < 8; ++column) {
		bool dc_only = true;
		for (unsigned row = 1; row < 8; ++row) {
			dc_only = dc_only && block[row * 8 + column] == 0;
		}
		if (dc_only) {
			for (unsigned row = 0; row < 8; ++row) {
				temporary[row * 8 + column] = block[column];
			}
		} else {
			idct_transform(block.data() + column, 8,
					temporary.data() + column, 8, false);
		}
	}
	for (unsigned row = 0; row < 8; ++row) {
		idct_transform(temporary.data() + row * 8, 1,
				block.data() + row * 8, 1, true);
	}
}

uint8_t byte_wrap(int32_t value) {
	return static_cast<uint8_t>(static_cast<uint32_t>(value) & 0xffU);
}

uint8_t clamp_byte(int value) {
	return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

}  // namespace

struct BinkMovie::Impl {
	BinkSource source;
	BinkInfo info;
	std::vector<uint32_t> frame_offsets;
	uint64_t declared_size = 0;
	uint32_t next_frame = 0;
	BinkFrame output;
	std::array<Plane, 3> current;
	std::array<Plane, 3> previous;
	std::string error;

	bool fail(const char *message) {
		error = message;
		return false;
	}

	bool read_range(uint64_t offset, uint8_t *destination, size_t size) {
		if (offset > source.size || size > source.size - offset || !source.read_at) {
			return false;
		}
		return source.read_at(offset, destination, size);
	}

	bool initialize();
	bool decode_packet(const std::vector<uint8_t> &packet);
	bool decode_plane(BitReader &bits, unsigned plane_index);
	void convert_to_rgba();
};

double BinkInfo::frames_per_second() const {
	return fps_denominator == 0 ? 0.0 :
			static_cast<double>(fps_numerator) / fps_denominator;
}

bool BinkMovie::Impl::initialize() {
	std::array<uint8_t, kHeaderSize> header{};
	if (!read_range(0, header.data(), header.size())) {
		return fail("could not read the Bink header");
	}
	if (read_le32(header.data()) != kBikiMagic) {
		return fail("only BIKi video is supported");
	}
	declared_size = static_cast<uint64_t>(read_le32(header.data() + 4)) + 8;
	info.frame_count = read_le32(header.data() + 8);
	info.largest_frame = read_le32(header.data() + 12);
	info.width = read_le32(header.data() + 20);
	info.height = read_le32(header.data() + 24);
	info.fps_numerator = read_le32(header.data() + 28);
	info.fps_denominator = read_le32(header.data() + 32);
	info.file_flags = read_le32(header.data() + 36);
	info.audio_track_count = read_le32(header.data() + 40);

	if (declared_size < kHeaderSize || declared_size > source.size) {
		return fail("the declared Bink file size is out of bounds");
	}
	if (info.frame_count == 0 || info.frame_count > kMaximumFrames) {
		return fail("the Bink frame count is invalid");
	}
	if (info.width == 0 || info.height == 0 ||
			info.width > kMaximumDimension || info.height > kMaximumDimension) {
		return fail("the Bink dimensions are invalid");
	}
	if (info.fps_numerator == 0 || info.fps_denominator == 0) {
		return fail("the Bink frame rate is invalid");
	}
	if (info.largest_frame > declared_size) {
		return fail("the largest Bink frame exceeds the file size");
	}
	if (info.file_flags != 0) {
		return fail("alpha, grayscale, and extended Bink flags are not supported");
	}
	if (info.audio_track_count != 0) {
		return fail("Bink audio tracks are not supported yet");
	}

	const uint64_t table_bytes = static_cast<uint64_t>(info.frame_count) * 4;
	if (table_bytes > declared_size - kHeaderSize ||
			table_bytes > std::numeric_limits<size_t>::max()) {
		return fail("the Bink frame index is out of bounds");
	}
	std::vector<uint8_t> table(static_cast<size_t>(table_bytes));
	if (!read_range(kHeaderSize, table.data(), table.size())) {
		return fail("could not read the Bink frame index");
	}
	frame_offsets.resize(static_cast<size_t>(info.frame_count) + 1);
	for (uint32_t i = 0; i < info.frame_count; ++i) {
		frame_offsets[i] = read_le32(table.data() + static_cast<size_t>(i) * 4) & ~1U;
	}
	frame_offsets[info.frame_count] = static_cast<uint32_t>(declared_size);
	const uint64_t minimum_data_offset = kHeaderSize + table_bytes;
	for (uint32_t i = 0; i < info.frame_count; ++i) {
		if (frame_offsets[i] < minimum_data_offset ||
				frame_offsets[i + 1] <= frame_offsets[i] ||
				frame_offsets[i + 1] > declared_size) {
			return fail("the Bink frame index is invalid");
		}
		if (frame_offsets[i + 1] - frame_offsets[i] > info.largest_frame) {
			return fail("a Bink packet exceeds the declared largest frame");
		}
	}

	current[0].allocate(info.width, info.height);
	previous[0].allocate(info.width, info.height);
	const uint32_t chroma_width = info.width >> 1;
	const uint32_t chroma_height = info.height >> 1;
	if (chroma_width == 0 || chroma_height == 0) {
		return fail("the Bink dimensions are too small for YUV 4:2:0 video");
	}
	for (unsigned plane = 1; plane < 3; ++plane) {
		current[plane].allocate(chroma_width, chroma_height);
		previous[plane].allocate(chroma_width, chroma_height);
	}
	output.width = info.width;
	output.height = info.height;
	output.stride = info.width * 4;
	output.rgba.resize(static_cast<size_t>(output.stride) * output.height);
	return true;
}

namespace {

bool take_value(Bundle &bundle, int32_t &value) {
	if (bundle.cursor >= bundle.values.size()) {
		return false;
	}
	value = bundle.values[bundle.cursor++];
	return true;
}

bool begin_refill(BitReader &bits, Bundle &bundle, uint32_t &count) {
	if (bundle.ended || bundle.cursor < bundle.values.size()) {
		count = 0;
		return true;
	}
	bundle.values.clear();
	bundle.cursor = 0;
	if (!bits.read(bundle.length_bits, count)) {
		return false;
	}
	if (count == 0) {
		bundle.ended = true;
		return true;
	}
	if (count > bundle.capacity) {
		return false;
	}
	bundle.values.reserve(count);
	return true;
}

// The prologue refill_runs and refill_block_types share: the begin_refill
// gate, then the repeat bit that fills the whole bundle from one 4-bit value.
// `finished` set = the caller returns the result as-is; clear = decode
// `count` Huffman values.
bool refill_repeat_prologue(BitReader &bits, Bundle &bundle, uint32_t &count,
		bool &finished) {
	finished = true;
	if (!begin_refill(bits, bundle, count) || count == 0) {
		return bits.ok();
	}
	bool repeat = false;
	if (!bits.read_bit(repeat)) {
		return false;
	}
	if (repeat) {
		uint32_t value = 0;
		if (!bits.read(4, value)) {
			return false;
		}
		bundle.values.assign(count, static_cast<int32_t>(value));
		return true;
	}
	finished = false;
	return true;
}

bool refill_runs(BitReader &bits, Bundle &bundle) {
	uint32_t count = 0;
	bool finished = false;
	const bool prologue = refill_repeat_prologue(bits, bundle, count, finished);
	if (finished) {
		return prologue;
	}
	for (uint32_t i = 0; i < count; ++i) {
		uint8_t value = 0;
		if (!read_huffman(bits, bundle.tree, value)) {
			return false;
		}
		bundle.values.push_back(value);
	}
	return true;
}

bool refill_patterns(BitReader &bits, Bundle &bundle) {
	uint32_t count = 0;
	if (!begin_refill(bits, bundle, count) || count == 0) {
		return bits.ok();
	}
	for (uint32_t i = 0; i < count; ++i) {
		uint8_t low = 0;
		uint8_t high = 0;
		if (!read_huffman(bits, bundle.tree, low) ||
				!read_huffman(bits, bundle.tree, high)) {
			return false;
		}
		bundle.values.push_back(static_cast<int32_t>(low | (high << 4)));
	}
	return true;
}

bool refill_motion(BitReader &bits, Bundle &bundle) {
	uint32_t count = 0;
	if (!begin_refill(bits, bundle, count) || count == 0) {
		return bits.ok();
	}
	bool repeat = false;
	if (!bits.read_bit(repeat)) {
		return false;
	}
	auto read_one = [&]() -> std::pair<bool, int32_t> {
		uint8_t magnitude = 0;
		if (!read_huffman(bits, bundle.tree, magnitude)) {
			return {false, 0};
		}
		bool negative = false;
		if (magnitude != 0 && !bits.read_bit(negative)) {
			return {false, 0};
		}
		return {true, negative ? -static_cast<int32_t>(magnitude) : magnitude};
	};
	if (repeat) {
		uint32_t magnitude = 0;
		if (!bits.read(4, magnitude)) {
			return false;
		}
		bool negative = false;
		if (magnitude != 0 && !bits.read_bit(negative)) {
			return false;
		}
		bundle.values.assign(count, negative ? -static_cast<int32_t>(magnitude) :
				static_cast<int32_t>(magnitude));
		return true;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const auto decoded = read_one();
		if (!decoded.first) {
			return false;
		}
		bundle.values.push_back(decoded.second);
	}
	return true;
}

bool refill_block_types(BitReader &bits, Bundle &bundle) {
	uint32_t count = 0;
	bool finished = false;
	const bool prologue = refill_repeat_prologue(bits, bundle, count, finished);
	if (finished) {
		return prologue;
	}
	constexpr std::array<unsigned, 4> repeats = {4, 8, 12, 32};
	uint8_t last = 0;
	while (bundle.values.size() < count) {
		uint8_t value = 0;
		if (!read_huffman(bits, bundle.tree, value)) {
			return false;
		}
		if (value < 12) {
			last = value;
			bundle.values.push_back(value);
		} else {
			const unsigned run = repeats[value - 12];
			if (run > count - bundle.values.size()) {
				return false;
			}
			bundle.values.insert(bundle.values.end(), run, last);
		}
	}
	return true;
}

bool refill_dc(BitReader &bits, Bundle &bundle, bool has_sign) {
	uint32_t count = 0;
	if (!begin_refill(bits, bundle, count) || count == 0) {
		return bits.ok();
	}
	uint32_t first = 0;
	if (!bits.read(has_sign ? 10 : 11, first)) {
		return false;
	}
	int32_t value = static_cast<int32_t>(first);
	if (has_sign && value != 0) {
		bool negative = false;
		if (!bits.read_bit(negative)) {
			return false;
		}
		if (negative) {
			value = -value;
		}
	}
	bundle.values.push_back(value);
	for (uint32_t base = 1; base < count; base += 8) {
		const uint32_t group = std::min<uint32_t>(8, count - base);
		uint32_t magnitude_bits = 0;
		if (!bits.read(4, magnitude_bits)) {
			return false;
		}
		for (uint32_t i = 0; i < group; ++i) {
			int32_t delta = 0;
			if (magnitude_bits != 0) {
				uint32_t magnitude = 0;
				if (!bits.read(magnitude_bits, magnitude)) {
					return false;
				}
				if (magnitude != 0) {
					bool negative = false;
					if (!bits.read_bit(negative)) {
						return false;
					}
					delta = negative ? -static_cast<int32_t>(magnitude) :
							static_cast<int32_t>(magnitude);
				}
			}
			value += delta;
			if (value < std::numeric_limits<int16_t>::min() ||
					value > std::numeric_limits<int16_t>::max()) {
				return false;
			}
			bundle.values.push_back(value);
		}
	}
	return true;
}

bool refill_colors(BitReader &bits, Bundle &bundle,
		std::array<Tree, 16> &high_trees, uint8_t &last_high) {
	uint32_t count = 0;
	if (!begin_refill(bits, bundle, count) || count == 0) {
		return bits.ok();
	}
	bool repeat = false;
	if (!bits.read_bit(repeat)) {
		return false;
	}
	auto read_color = [&]() -> std::pair<bool, uint8_t> {
		uint8_t high = 0;
		uint8_t low = 0;
		if (!read_huffman(bits, high_trees[last_high], high) ||
				!read_huffman(bits, bundle.tree, low)) {
			return {false, 0};
		}
		last_high = high;
		return {true, static_cast<uint8_t>((high << 4) | low)};
	};
	if (repeat) {
		const auto color = read_color();
		if (!color.first) {
			return false;
		}
		bundle.values.assign(count, color.second);
		return true;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const auto color = read_color();
		if (!color.first) {
			return false;
		}
		bundle.values.push_back(color.second);
	}
	return true;
}

}  // namespace

namespace {

bool read_signed_coefficient(BitReader &bits, unsigned magnitude_bit,
		int32_t &coefficient) {
	if (magnitude_bit == 0) {
		bool negative = false;
		if (!bits.read_bit(negative)) {
			return false;
		}
		coefficient = negative ? -1 : 1;
		return true;
	}
	uint32_t low_bits = 0;
	if (!bits.read(magnitude_bit, low_bits)) {
		return false;
	}
	uint32_t magnitude = low_bits | (1U << magnitude_bit);
	bool negative = false;
	if (!bits.read_bit(negative)) {
		return false;
	}
	coefficient = negative ? -static_cast<int32_t>(magnitude) :
			static_cast<int32_t>(magnitude);
	return true;
}

bool read_dct_block(BitReader &bits, std::array<int32_t, 64> &block,
		bool intra) {
	std::array<int, 128> coordinates{};
	std::array<int, 128> modes{};
	int list_start = 64;
	int list_end = 64;
	auto append = [&](int coordinate, int mode) {
		if (list_end >= static_cast<int>(coordinates.size())) {
			return false;
		}
		coordinates[list_end] = coordinate;
		modes[list_end] = mode;
		++list_end;
		return true;
	};
	if (!append(4, 0) || !append(24, 0) || !append(44, 0) ||
			!append(1, 3) || !append(2, 3) || !append(3, 3)) {
		return false;
	}

	uint32_t level_count = 0;
	if (!bits.read(4, level_count)) {
		return false;
	}
	for (int magnitude_bit = static_cast<int>(level_count) - 1;
			magnitude_bit >= 0; --magnitude_bit) {
		int position = list_start;
		while (position < list_end) {
			if ((coordinates[position] | modes[position]) == 0) {
				++position;
				continue;
			}
			bool active = false;
			if (!bits.read_bit(active)) {
				return false;
			}
			if (!active) {
				++position;
				continue;
			}

			int coordinate = coordinates[position];
			const int mode = modes[position];
			if (mode == 0) {
				coordinates[position] = coordinate + 4;
				modes[position] = 1;
			}
			if (mode == 0 || mode == 2) {
				if (mode == 2) {
					coordinates[position] = 0;
					modes[position] = 0;
					++position;
				}
				for (unsigned i = 0; i < 4; ++i, ++coordinate) {
					if (coordinate < 0 || coordinate >= 64) {
						return false;
					}
					bool defer = false;
					if (!bits.read_bit(defer)) {
						return false;
					}
					if (defer) {
						if (list_start == 0) {
							return false;
						}
						--list_start;
						coordinates[list_start] = coordinate;
						modes[list_start] = 3;
					} else {
						int32_t coefficient = 0;
						if (!read_signed_coefficient(bits,
								static_cast<unsigned>(magnitude_bit), coefficient)) {
							return false;
						}
						block[detail::kCoefficientScan[coordinate]] = coefficient;
					}
				}
				continue;
			}
			if (mode == 1) {
				modes[position] = 2;
				for (unsigned i = 0; i < 3; ++i) {
					coordinate += 4;
					if (!append(coordinate, 2)) {
						return false;
					}
				}
				continue;
			}
			if (mode == 3) {
				if (coordinate < 0 || coordinate >= 64) {
					return false;
				}
				int32_t coefficient = 0;
				if (!read_signed_coefficient(bits,
						static_cast<unsigned>(magnitude_bit), coefficient)) {
					return false;
				}
				block[detail::kCoefficientScan[coordinate]] = coefficient;
				coordinates[position] = 0;
				modes[position] = 0;
				++position;
				continue;
			}
			return false;
		}
	}

	uint32_t quantizer = 0;
	if (!bits.read(4, quantizer)) {
		return false;
	}
	const uint32_t *quant = intra ? detail::kIntraQuant[quantizer] :
			detail::kInterQuant[quantizer];
	for (unsigned i = 0; i < block.size(); ++i) {
		if (block[i] != 0) {
			const uint32_t product = static_cast<uint32_t>(block[i]) * quant[i];
			block[i] = arithmetic_shift_right(static_cast<int32_t>(product), 11);
		}
	}
	return true;
}

bool read_residue(BitReader &bits, std::array<int16_t, 64> &block,
		int events_remaining) {
	std::array<int, 128> coordinates{};
	std::array<int, 128> modes{};
	std::array<uint8_t, 64> nonzero{};
	int nonzero_count = 0;
	int list_start = 64;
	int list_end = 64;
	auto append = [&](int coordinate, int mode) {
		if (list_end >= static_cast<int>(coordinates.size())) {
			return false;
		}
		coordinates[list_end] = coordinate;
		modes[list_end] = mode;
		++list_end;
		return true;
	};
	if (!append(4, 0) || !append(24, 0) || !append(44, 0) || !append(0, 2)) {
		return false;
	}
	uint32_t first_mask_bit = 0;
	if (!bits.read(3, first_mask_bit)) {
		return false;
	}
	for (int mask = 1 << first_mask_bit; mask != 0; mask >>= 1) {
		for (int i = 0; i < nonzero_count; ++i) {
			bool refine = false;
			if (!bits.read_bit(refine)) {
				return false;
			}
			if (!refine) {
				continue;
			}
			const unsigned coordinate = nonzero[i];
			block[coordinate] = static_cast<int16_t>(block[coordinate] < 0 ?
					block[coordinate] - mask : block[coordinate] + mask);
			if (--events_remaining < 0) {
				return true;
			}
		}
		int position = list_start;
		while (position < list_end) {
			if ((coordinates[position] | modes[position]) == 0) {
				++position;
				continue;
			}
			bool active = false;
			if (!bits.read_bit(active)) {
				return false;
			}
			if (!active) {
				++position;
				continue;
			}

			int coefficient_coordinate = coordinates[position];
			const int mode = modes[position];
			if (mode == 0) {
				coordinates[position] = coefficient_coordinate + 4;
				modes[position] = 1;
			}
			if (mode == 0 || mode == 2) {
				if (mode == 2) {
					coordinates[position] = 0;
					modes[position] = 0;
					++position;
				}
				for (unsigned i = 0; i < 4; ++i, ++coefficient_coordinate) {
					if (coefficient_coordinate < 0 || coefficient_coordinate >= 64) {
						return false;
					}
					bool defer = false;
					if (!bits.read_bit(defer)) {
						return false;
					}
					if (defer) {
						if (list_start == 0) {
							return false;
						}
						--list_start;
						coordinates[list_start] = coefficient_coordinate;
						modes[list_start] = 3;
					} else {
						if (nonzero_count >= static_cast<int>(nonzero.size())) {
							return false;
						}
						const uint8_t output_coordinate =
								detail::kCoefficientScan[coefficient_coordinate];
						nonzero[nonzero_count++] = output_coordinate;
						bool negative = false;
						if (!bits.read_bit(negative)) {
							return false;
						}
						block[output_coordinate] = static_cast<int16_t>(negative ? -mask : mask);
						if (--events_remaining < 0) {
							return true;
						}
					}
				}
				continue;
			}
			if (mode == 1) {
				modes[position] = 2;
				for (unsigned i = 0; i < 3; ++i) {
					coefficient_coordinate += 4;
					if (!append(coefficient_coordinate, 2)) {
						return false;
					}
				}
				continue;
			}
			if (mode == 3) {
				if (coefficient_coordinate < 0 || coefficient_coordinate >= 64 ||
						nonzero_count >= static_cast<int>(nonzero.size())) {
					return false;
				}
				const uint8_t output_coordinate =
						detail::kCoefficientScan[coefficient_coordinate];
				nonzero[nonzero_count++] = output_coordinate;
				bool negative = false;
				if (!bits.read_bit(negative)) {
					return false;
				}
				block[output_coordinate] = static_cast<int16_t>(negative ? -mask : mask);
				coordinates[position] = 0;
				modes[position] = 0;
				++position;
				if (--events_remaining < 0) {
					return true;
				}
				continue;
			}
			return false;
		}
	}
	return true;
}

void write_dct(uint8_t *destination, uint32_t stride,
		std::array<int32_t, 64> &block, bool add) {
	inverse_dct(block);
	for (unsigned y = 0; y < 8; ++y) {
		for (unsigned x = 0; x < 8; ++x) {
			const unsigned index = y * 8 + x;
			destination[y * stride + x] = add ?
					byte_wrap(static_cast<int32_t>(destination[y * stride + x]) + block[index]) :
					byte_wrap(block[index]);
		}
	}
}

void fill_block(uint8_t *destination, uint32_t stride,
		unsigned width, unsigned height, uint8_t value) {
	for (unsigned y = 0; y < height; ++y) {
		std::memset(destination + y * stride, value, width);
	}
}

void copy_block(uint8_t *destination, uint32_t destination_stride,
		const uint8_t *source, uint32_t source_stride) {
	for (unsigned y = 0; y < 8; ++y) {
		std::memcpy(destination + y * destination_stride,
				source + y * source_stride, 8);
	}
}

bool copy_motion_block(uint8_t *destination, const Plane &reference,
		uint32_t destination_stride, uint32_t block_x, uint32_t block_y,
		int32_t x_offset, int32_t y_offset) {
	const int64_t source_x = static_cast<int64_t>(block_x) * 8 + x_offset;
	const int64_t source_y = static_cast<int64_t>(block_y) * 8 + y_offset;
	if (source_x < 0 || source_y < 0 || source_x + 8 > reference.stride ||
			source_y + 8 > reference.rows) {
		return false;
	}
	copy_block(destination, destination_stride,
			reference.pixels.data() + source_y * reference.stride + source_x,
			reference.stride);
	return true;
}

bool decode_run_block(BitReader &bits, Bundle &colors, Bundle &runs,
		uint8_t *destination, uint32_t stride) {
	uint32_t scan_number = 0;
	if (!bits.read(4, scan_number)) {
		return false;
	}
	const uint8_t *scan = detail::kPatternScans[scan_number];
	unsigned filled = 0;
	while (filled < 63) {
		int32_t encoded_run = 0;
		if (!take_value(runs, encoded_run)) {
			return false;
		}
		const unsigned run = static_cast<unsigned>(encoded_run) + 1;
		if (run > 64 - filled) {
			return false;
		}
		bool repeat_color = false;
		if (!bits.read_bit(repeat_color)) {
			return false;
		}
		int32_t color = 0;
		if (repeat_color && !take_value(colors, color)) {
			return false;
		}
		for (unsigned i = 0; i < run; ++i) {
			if (!repeat_color && !take_value(colors, color)) {
				return false;
			}
			const uint8_t coordinate = scan[filled++];
			destination[(coordinate >> 3) * stride + (coordinate & 7)] =
					static_cast<uint8_t>(color);
		}
	}
	if (filled == 63) {
		int32_t color = 0;
		if (!take_value(colors, color)) {
			return false;
		}
		const uint8_t coordinate = scan[filled];
		destination[(coordinate >> 3) * stride + (coordinate & 7)] =
				static_cast<uint8_t>(color);
	}
	return true;
}

bool decode_pattern_block(Bundle &colors, Bundle &patterns,
		uint8_t *destination, uint32_t stride) {
	int32_t first = 0;
	int32_t second = 0;
	if (!take_value(colors, first) || !take_value(colors, second)) {
		return false;
	}
	for (unsigned y = 0; y < 8; ++y) {
		int32_t pattern = 0;
		if (!take_value(patterns, pattern)) {
			return false;
		}
		for (unsigned x = 0; x < 8; ++x) {
			destination[y * stride + x] = static_cast<uint8_t>(
					(pattern & (1 << x)) != 0 ? second : first);
		}
	}
	return true;
}

bool decode_raw_block(Bundle &colors, uint8_t *destination, uint32_t stride) {
	for (unsigned y = 0; y < 8; ++y) {
		for (unsigned x = 0; x < 8; ++x) {
			int32_t color = 0;
			if (!take_value(colors, color)) {
				return false;
			}
			destination[y * stride + x] = static_cast<uint8_t>(color);
		}
	}
	return true;
}

void scale_block(const std::array<uint8_t, 64> &source,
		uint8_t *destination, uint32_t stride) {
	for (unsigned y = 0; y < 8; ++y) {
		for (unsigned x = 0; x < 8; ++x) {
			const uint8_t value = source[y * 8 + x];
			destination[(y * 2) * stride + x * 2] = value;
			destination[(y * 2) * stride + x * 2 + 1] = value;
			destination[(y * 2 + 1) * stride + x * 2] = value;
			destination[(y * 2 + 1) * stride + x * 2 + 1] = value;
		}
	}
}

}  // namespace

// [orig: BinkVideo_DecodePlane @ 0x3001D2C0, binkw32.dll -- the per-plane
// bundle refill + block walk; the bundle callbacks it installs are its
// sub_3001C300 / sub_3001C5E0 pointers, the tree reads its sub_3001C030 /
// sub_3001C0A0 callees.]
bool BinkMovie::Impl::decode_plane(BitReader &bits, unsigned plane_index) {
	Plane &plane = current[plane_index];
	const Plane &reference = previous[plane_index];
	const uint32_t block_width = (plane.width + 7) >> 3;
	const uint32_t block_height = (plane.height + 7) >> 3;
	const size_t block_count = static_cast<size_t>(block_width) * block_height;
	std::array<Bundle, bundle_index(BundleKind::count)> bundles;
	for (Bundle &bundle : bundles) {
		bundle.capacity = block_count * 64;
		bundle.reset();
	}
	const uint32_t aligned_width = align_up(std::max(plane.width, 8U), 8);
	const unsigned block_length = bit_width((aligned_width >> 3) + 511);
	bundles[bundle_index(BundleKind::block_types)].length_bits = block_length;
	bundles[bundle_index(BundleKind::sub_block_types)].length_bits =
			bit_width((aligned_width >> 4) + 511);
	bundles[bundle_index(BundleKind::colors)].length_bits =
			bit_width(block_width * 64 + 511);
	bundles[bundle_index(BundleKind::patterns)].length_bits =
			bit_width(block_width * 8 + 511);
	bundles[bundle_index(BundleKind::x_motion)].length_bits = block_length;
	bundles[bundle_index(BundleKind::y_motion)].length_bits = block_length;
	bundles[bundle_index(BundleKind::intra_dc)].length_bits = block_length;
	bundles[bundle_index(BundleKind::inter_dc)].length_bits = block_length;
	bundles[bundle_index(BundleKind::runs)].length_bits =
			bit_width(block_width * 48 + 511);

	std::array<Tree, 16> high_color_trees;
	uint8_t last_high_color = 0;
	for (unsigned i = 0; i < bundles.size(); ++i) {
		if (i == bundle_index(BundleKind::colors)) {
			for (Tree &tree : high_color_trees) {
				if (!read_tree(bits, tree)) {
					return false;
				}
			}
		}
		if (i != bundle_index(BundleKind::intra_dc) &&
				i != bundle_index(BundleKind::inter_dc) &&
				!read_tree(bits, bundles[i].tree)) {
			return false;
		}
	}

	auto &block_types = bundles[bundle_index(BundleKind::block_types)];
	auto &sub_block_types = bundles[bundle_index(BundleKind::sub_block_types)];
	auto &colors = bundles[bundle_index(BundleKind::colors)];
	auto &patterns = bundles[bundle_index(BundleKind::patterns)];
	auto &x_motion = bundles[bundle_index(BundleKind::x_motion)];
	auto &y_motion = bundles[bundle_index(BundleKind::y_motion)];
	auto &intra_dc = bundles[bundle_index(BundleKind::intra_dc)];
	auto &inter_dc = bundles[bundle_index(BundleKind::inter_dc)];
	auto &runs = bundles[bundle_index(BundleKind::runs)];

	for (uint32_t block_y = 0; block_y < block_height; ++block_y) {
		if (!refill_block_types(bits, block_types) ||
				!refill_block_types(bits, sub_block_types) ||
				!refill_colors(bits, colors, high_color_trees, last_high_color) ||
				!refill_patterns(bits, patterns) ||
				!refill_motion(bits, x_motion) ||
				!refill_motion(bits, y_motion) ||
				!refill_dc(bits, intra_dc, false) ||
				!refill_dc(bits, inter_dc, true) ||
				!refill_runs(bits, runs)) {
			return false;
		}

		for (uint32_t block_x = 0; block_x < block_width; ++block_x) {
			int32_t block_type = 0;
			if (!take_value(block_types, block_type)) {
				return false;
			}
			uint8_t *destination = plane.pixels.data() +
					static_cast<size_t>(block_y) * 8 * plane.stride + block_x * 8;
			if (block_type == 1 && ((block_x & 1) != 0 || (block_y & 1) != 0)) {
				if (block_x + 1 >= block_width) {
					return false;
				}
				++block_x;
				continue;
			}

			switch (block_type) {
			case 0:
				copy_block(destination, plane.stride,
						reference.pixels.data() +
								static_cast<size_t>(block_y) * 8 * reference.stride + block_x * 8,
						reference.stride);
				break;
			case 1: {
				if (block_x + 1 >= block_width || block_y + 1 >= block_height) {
					return false;
				}
				int32_t subtype = 0;
				if (!take_value(sub_block_types, subtype)) {
					return false;
				}
				std::array<uint8_t, 64> unscaled{};
				if (subtype == 3) {
					if (!decode_run_block(bits, colors, runs, unscaled.data(), 8)) {
						return false;
					}
				} else if (subtype == 5) {
					std::array<int32_t, 64> coefficients{};
					int32_t dc = 0;
					if (!take_value(intra_dc, dc)) {
						return false;
					}
					coefficients[0] = dc;
					if (!read_dct_block(bits, coefficients, true)) {
						return false;
					}
					write_dct(unscaled.data(), 8, coefficients, false);
				} else if (subtype == 6) {
					int32_t color = 0;
					if (!take_value(colors, color)) {
						return false;
					}
					fill_block(destination, plane.stride, 16, 16,
							static_cast<uint8_t>(color));
				} else if (subtype == 8) {
					if (!decode_pattern_block(colors, patterns, unscaled.data(), 8)) {
						return false;
					}
				} else if (subtype == 9) {
					if (!decode_raw_block(colors, unscaled.data(), 8)) {
						return false;
					}
				} else {
					return false;
				}
				if (subtype != 6) {
					scale_block(unscaled, destination, plane.stride);
				}
				++block_x;
				break;
			}
			case 2: {
				int32_t x = 0;
				int32_t y = 0;
				if (!take_value(x_motion, x) || !take_value(y_motion, y) ||
						!copy_motion_block(destination, reference, plane.stride,
								block_x, block_y, x, y)) {
					return false;
				}
				break;
			}
			case 3:
				if (!decode_run_block(bits, colors, runs, destination, plane.stride)) {
					return false;
				}
				break;
			case 4: {
				int32_t x = 0;
				int32_t y = 0;
				if (!take_value(x_motion, x) || !take_value(y_motion, y) ||
						!copy_motion_block(destination, reference, plane.stride,
								block_x, block_y, x, y)) {
					return false;
				}
				uint32_t event_count = 0;
				if (!bits.read(7, event_count)) {
					return false;
				}
				std::array<int16_t, 64> residue{};
				if (!read_residue(bits, residue, static_cast<int>(event_count))) {
					return false;
				}
				for (unsigned y_index = 0; y_index < 8; ++y_index) {
					for (unsigned x_index = 0; x_index < 8; ++x_index) {
						const unsigned index = y_index * 8 + x_index;
						destination[y_index * plane.stride + x_index] = byte_wrap(
								static_cast<int32_t>(destination[y_index * plane.stride + x_index]) +
								residue[index]);
					}
				}
				break;
			}
			case 5: {
				std::array<int32_t, 64> coefficients{};
				int32_t dc = 0;
				if (!take_value(intra_dc, dc)) {
					return false;
				}
				coefficients[0] = dc;
				if (!read_dct_block(bits, coefficients, true)) {
					return false;
				}
				write_dct(destination, plane.stride, coefficients, false);
				break;
			}
			case 6: {
				int32_t color = 0;
				if (!take_value(colors, color)) {
					return false;
				}
				fill_block(destination, plane.stride, 8, 8,
						static_cast<uint8_t>(color));
				break;
			}
			case 7: {
				int32_t x = 0;
				int32_t y = 0;
				if (!take_value(x_motion, x) || !take_value(y_motion, y) ||
						!copy_motion_block(destination, reference, plane.stride,
								block_x, block_y, x, y)) {
					return false;
				}
				std::array<int32_t, 64> coefficients{};
				int32_t dc = 0;
				if (!take_value(inter_dc, dc)) {
					return false;
				}
				coefficients[0] = dc;
				if (!read_dct_block(bits, coefficients, false)) {
					return false;
				}
				write_dct(destination, plane.stride, coefficients, true);
				break;
			}
			case 8:
				if (!decode_pattern_block(colors, patterns, destination, plane.stride)) {
					return false;
				}
				break;
			case 9:
				if (!decode_raw_block(colors, destination, plane.stride)) {
					return false;
				}
				break;
			default:
				return false;
			}
		}
	}
	return bits.align32();
}

// One video packet: the 32-bit prefix, then the Y, V, U planes in that
// order [orig: BinkVideo_DecodeFrame @ 0x3001F260, binkw32.dll -- the three
// BinkVideo_DecodePlane calls].
bool BinkMovie::Impl::decode_packet(const std::vector<uint8_t> &packet) {
	BitReader bits(packet);
	if (!bits.skip(32)) {
		return fail("the BIKi packet is missing its video prefix");
	}
	// BIKi stores Y, V, U. The public frame is converted from conventional
	// Y, U, V plane indices below.
	if (!decode_plane(bits, 0) || !decode_plane(bits, 2) || !decode_plane(bits, 1)) {
		return fail("the BIKi video packet is malformed");
	}
	convert_to_rgba();
	return true;
}

void BinkMovie::Impl::convert_to_rgba() {
	// The colour conversion is binkw32's own, not the decoder's caller: the
	// game hands BinkCopyToBufferRect a BINKSURFACE32 target (the D3D
	// X8R8G8B8 texture the slot creates) and the DLL converts. Byte order is
	// the one device fold -- retail packs (R<<16)|(G<<8)|B into the X8R8G8B8
	// dword; the frame here is top-down RGBA8 with an opaque alpha.
	// [orig: BinkVideoSlot_RenderFrameToTexture @0x567540 (Jointops.exe) --
	//  BinkCopyToBufferRect(..., flags | 0x80000000 BINKCOPYALL) @0x5675b2,
	//  the slot's surface code from the D3DFMT -> BINKSURFACE table
	//  @0x5679be..0x567a2b, D3DFMT_X8R8G8B8 (22) -> BINKSURFACE32 (3) being
	//  the first CheckDeviceFormat hit @0x567a61]
	const Plane &luma = current[0];
	const Plane &chroma_u = current[1];
	const Plane &chroma_v = current[2];
	for (uint32_t y = 0; y < info.height; ++y) {
		uint8_t *destination = output.rgba.data() + static_cast<size_t>(y) * output.stride;
		const uint8_t *luma_row = luma.pixels.data() + static_cast<size_t>(y) * luma.stride;
		const uint8_t *u_row = chroma_u.pixels.data() + static_cast<size_t>(y >> 1) * chroma_u.stride;
		const uint8_t *v_row = chroma_v.pixels.data() + static_cast<size_t>(y >> 1) * chroma_v.stride;
		for (uint32_t x = 0; x < info.width; ++x) {
			yuv_to_rgb(luma_row[x], u_row[x >> 1], v_row[x >> 1],
					destination[x * 4 + 0], destination[x * 4 + 1], destination[x * 4 + 2]);
			destination[x * 4 + 3] = 255;
		}
	}
}

// The retail YUV -> RGB law, witnessed in binkw32.dll 1.5u [orig: YUV_init
// @ 0x30019F00, binkw32.dll -- BinkCopyToBufferRect @ 0x30013220 calls it with
// the surface code (the call @ 0x30013365) before the blit]:
//   * the luma row dword_30059178[Y] = trunc(clamp(Y - 16, 0, 219) * 38154
//     / 32768) (the `imul 950Ah; cdq; and edx,7FFFh; add; sar 15` truncating
//     divide) -- the MMX blits fold the same law as (Y - 16 sat) << 2, pmulhw
//     19077 (= 38154 / 2, qword_30055040) with the 16 in qword_30055038;
//   * the four chroma rows, each a linear ramp in the same truncating
//     fixed point: R += (V - 128) * 52299 / 32768 (dword_300632A8),
//     G += -(V - 128) * 26639 / 32768 (dword_30062AA8)
//        + -(U - 128) * 12837 / 32768 (dword_30062EA8),
//     B += (U - 128) * 66101 / 32768 (dword_300626A8);
//   * every channel clamps to 0..255 through the per-luma clamp rows
//     dword_30059998[Y] the BINKSURFACE32 blit indexes (YUV_blit_32bpp ->
//     the dword_30064700 table, sub_30029270 @ 0x30029270 scalar; the MMX
//     twins clamp with the 0x7F00 paddsw/psubusw pair).
// Not BT.601's 298/409/100/208/516 set with round-half-up: the retail ramps
// are 38154/52299/26639/12837/66101 over 32768 and truncate, so a mid-grey
// (128, 128, 128) lands on 130, not 131.
// The ramp constants, named for the tables they fill (binkw32.dll data):
constexpr int32_t kYuvFixedOne = 32768;   // the `sar 15` fixed-point denominator
constexpr int32_t kYuvLumaBias = 16;      // qword_30055038
constexpr int32_t kYuvLumaRange = 219;    // the clamp before the luma ramp
constexpr int32_t kYuvLumaScale = 38154;  // dword_30059178 ramp (pmulhw 19077 x 2)
constexpr int32_t kYuvChromaBias = 128;
constexpr int32_t kYuvVToR = 52299;       // dword_300632A8
constexpr int32_t kYuvVToG = 26639;       // dword_30062AA8 (subtracted)
constexpr int32_t kYuvUToG = 12837;       // dword_30062EA8 (subtracted)
constexpr int32_t kYuvUToB = 66101;       // dword_300626A8

void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
	const int32_t luma =
			std::clamp(static_cast<int32_t>(y) - kYuvLumaBias, 0, kYuvLumaRange) *
			kYuvLumaScale / kYuvFixedOne;
	const int32_t du = static_cast<int32_t>(u) - kYuvChromaBias;
	const int32_t dv = static_cast<int32_t>(v) - kYuvChromaBias;
	r = clamp_byte(luma + dv * kYuvVToR / kYuvFixedOne);
	g = clamp_byte(luma + (-dv * kYuvVToG) / kYuvFixedOne + (-du * kYuvUToG) / kYuvFixedOne);
	b = clamp_byte(luma + du * kYuvUToB / kYuvFixedOne);
}

std::unique_ptr<BinkMovie> BinkMovie::open(BinkSource source, std::string *error) {
	auto impl = std::make_unique<Impl>();
	impl->source = std::move(source);
	if (!impl->initialize()) {
		if (error != nullptr) {
			*error = impl->error;
		}
		return nullptr;
	}
	if (error != nullptr) {
		error->clear();
	}
	return std::unique_ptr<BinkMovie>(new BinkMovie(std::move(impl)));
}

BinkMovie::BinkMovie(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
BinkMovie::~BinkMovie() = default;
BinkMovie::BinkMovie(BinkMovie &&) noexcept = default;
BinkMovie &BinkMovie::operator=(BinkMovie &&) noexcept = default;

const BinkInfo &BinkMovie::info() const {
	return impl_->info;
}

BinkStatus BinkMovie::decode_next() {
	if (impl_->next_frame >= impl_->info.frame_count) {
		return BinkStatus::end_of_stream;
	}
	const uint32_t start = impl_->frame_offsets[impl_->next_frame];
	const uint32_t end = impl_->frame_offsets[impl_->next_frame + 1];
	std::vector<uint8_t> packet(end - start);
	if (!impl_->read_range(start, packet.data(), packet.size())) {
		impl_->error = "could not read the next Bink packet";
		return BinkStatus::io_error;
	}
	std::swap(impl_->current, impl_->previous);
	impl_->error.clear();
	if (!impl_->decode_packet(packet)) {
		std::swap(impl_->current, impl_->previous);
		return BinkStatus::invalid_data;
	}
	++impl_->next_frame;
	return BinkStatus::frame_ready;
}

const BinkFrame &BinkMovie::frame() const {
	return impl_->output;
}

bool BinkMovie::rewind() {
	impl_->next_frame = 0;
	impl_->error.clear();
	for (Plane &plane : impl_->current) {
		std::fill(plane.pixels.begin(), plane.pixels.end(), 0);
	}
	for (Plane &plane : impl_->previous) {
		std::fill(plane.pixels.begin(), plane.pixels.end(), 0);
	}
	std::fill(impl_->output.rgba.begin(), impl_->output.rgba.end(), 0);
	return true;
}

uint32_t BinkMovie::frame_index() const {
	return impl_->next_frame;
}

const std::string &BinkMovie::last_error() const {
	return impl_->error;
}

}  // namespace opennova::bink
