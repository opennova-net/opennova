// The chat display buffer of a host with no client (inmatch/server_console.h): the line's
// word wrap against the 4-unit chat box a host with no hud.def keeps, over a font whose
// narrow glyph and space let the walk break; the shift, the zeroed new slots, slot 1's timer
// and its 186-tick stagger, the two-space continuations, the 119-byte copy and the colour,
// CHAT GET's walk (oldest first, its strlen reading on past a full slot's text), the per-tick
// timers, and the bound at 40 slots (D-NET-372).
// [orig: Chat_AddMessageChannel1 @0x4985D0; HUD_WordWrapText @0x580980; HUD_GetChatBoxCoord
//  @0x5BBE90; Input_DecrementCooldownTimers @0x498440; CAdminServer_HandleChatCommand
//  @0x404B0F..0x404BB3]
#include <runtime/inmatch/server_console.h>

#include <formats/fnt/fnt.h>

#include <cstdio>
#include <string>

using namespace opennova;

namespace {

int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

// A one-page font whose glyphs are 8 units wide, 'i' 2 and the space 3 (spacing 1: a glyph's
// advance is its texel width).
struct TestFont {
	fnt::fnt_font_t font{};
	TestFont() {
		fnt::fnt_init_blank(&font, 1, 1);
		for (uint32_t i = 0; i < fnt::FNT_GLYPH_COUNT; ++i) set(static_cast<char>(32 + i), 8);
		set('i', 2);
		set(' ', 3);
	}
	~TestFont() { fnt::fnt_free(&font); }
	void set(char c, int width) {
		fnt::fnt_glyph_t &g = font.glyphs[static_cast<uint8_t>(c) - 32];
		g.page = 0;
		g.uv.u0 = 0.0f;
		g.uv.v0 = 0.0f;
		g.uv.u1 = static_cast<float>(width) / 256.0f;
		g.uv.v1 = 16.0f / 256.0f;
	}
};

constexpr uint32_t kWhite = 0xFFFFFFFFu;

int32_t timer_of(const inmatch::ChatDisplayBuffer &b, size_t slot) {
	const uint8_t *p = b.bytes.data() + (slot - 1) * inmatch::kChatDisplaySlotBytes +
			inmatch::kChatDisplayTimerOffset;
	return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

void test_no_wrap_without_a_narrow_start() {
	TestFont f;
	inmatch::ChatDisplayBuffer b;
	const inmatch::ChatDisplayWrap wrap{&f.font, inmatch::kChatDisplayWidthNoHudDef};
	inmatch::chat_display_post(b, wrap, "Bob: hello world", kWhite);
	CHECK(inmatch::chat_display_get(b) == "Bob: hello world\r\n");
	CHECK(timer_of(b, 1) == 930);
	// A second one-segment line staggers 186 ticks behind the first.
	inmatch::chat_display_post(b, wrap, "Ann: hi", kWhite);
	CHECK(inmatch::chat_display_get(b) == "Bob: hello world\r\nAnn: hi\r\n");
	CHECK(timer_of(b, 1) == 930 + 186 && timer_of(b, 2) == 930);
	inmatch::chat_display_tick(b);
	CHECK(timer_of(b, 1) == 930 + 185 && timer_of(b, 2) == 929);
}

void test_narrow_glyph_and_space_split() {
	TestFont f;
	inmatch::ChatDisplayBuffer b;
	const inmatch::ChatDisplayWrap wrap{&f.font, inmatch::kChatDisplayWidthNoHudDef};
	// 'i' advances 3 (2 + 1), within the 4; the space is seen before the walk passes 4.
	inmatch::chat_display_post(b, wrap, "i am here", kWhite);
	CHECK(inmatch::chat_display_get(b) == "i\r\n  am here\r\n");
	// The new slots were zeroed: the continuation (slot 1) takes the timer, the first
	// segment's slot none.
	CHECK(timer_of(b, 1) == 930 && timer_of(b, 2) == 0);
	// A leading space splits off an empty segment, which CHAT GET skips.
	inmatch::ChatDisplayBuffer c;
	inmatch::chat_display_post(c, wrap, " abc", kWhite);
	CHECK(inmatch::chat_display_get(c) == "  abc\r\n");
	// No font: the wrapper counts nothing, which is one line.
	inmatch::ChatDisplayBuffer d;
	inmatch::chat_display_post(d, inmatch::ChatDisplayWrap{}, "i am here", kWhite);
	CHECK(inmatch::chat_display_get(d) == "i am here\r\n");
}

void test_full_continuation_reads_on() {
	TestFont f;
	inmatch::ChatDisplayBuffer b;
	const inmatch::ChatDisplayWrap wrap{&f.font, inmatch::kChatDisplayWidthNoHudDef};
	// The continuation's 119 copied bytes run from +2 to +120, where the colour lands: its
	// text holds no NUL, so CHAT GET's strlen reads on through the colour's bytes and slot 1's
	// timer (930: A2 03 00 00) to that word's zero byte.
	const uint32_t colour = 0xFF2040FFu;
	inmatch::chat_display_post(b, wrap, "i " + std::string(130, 'a'), colour);
	const std::string want = std::string("i\r\n") + "  " + std::string(118, 'a') + "\xFF\x40\x20\xFF\xA2\x03" + "\r\n";
	CHECK(inmatch::chat_display_get(b) == want);
	// Once the timer has run down to 0 the read stops after the colour.
	for (int i = 0; i < 930; ++i) inmatch::chat_display_tick(b);
	CHECK(inmatch::chat_display_get(b) ==
			std::string("i\r\n") + "  " + std::string(118, 'a') + "\xFF\x40\x20\xFF" + "\r\n");
}

void test_forty_slots() {
	TestFont f;
	inmatch::ChatDisplayBuffer b;
	const inmatch::ChatDisplayWrap wrap{&f.font, inmatch::kChatDisplayWidthNoHudDef};
	for (int i = 0; i < 45; ++i) inmatch::chat_display_post(b, wrap, "line " + std::to_string(i), kWhite);
	const std::string get = inmatch::chat_display_get(b);
	CHECK(get.rfind("line 5\r\n", 0) == 0 && get.find("line 44\r\n") != std::string::npos &&
			get.find("line 4\r\n") == std::string::npos);
	// A wrap of more than 40 segments (each leading space past the first break splits again)
	// fills the 40 slots and stops there.
	inmatch::ChatDisplayBuffer c;
	inmatch::chat_display_post(c, wrap, "i" + std::string(60, ' ') + "z", kWhite);
	size_t lines = 0;
	const std::string all = inmatch::chat_display_get(c);
	for (size_t at = all.find("\r\n"); at != std::string::npos; at = all.find("\r\n", at + 2)) ++lines;
	CHECK(lines == 40);
}

} // namespace

int main() {
	test_no_wrap_without_a_narrow_start();
	test_narrow_glyph_and_space_split();
	test_full_continuation_reads_on();
	test_forty_slots();
	if (failures == 0) {
		std::printf("npruntime_chat_display: OK\n");
		return 0;
	}
	std::printf("%d failure(s)\n", failures);
	return 1;
}
