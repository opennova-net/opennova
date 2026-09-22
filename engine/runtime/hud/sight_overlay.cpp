// The SIGHTS card's per-row draw modes and the sight-scale cycle
// [orig: draw_weapon_sight_overlays @0x4dce00; Input_HandleActionBinding_0
//  case 216 @0x4e0c31].

#include <runtime/hud/sight_overlay.h>
#include <runtime/hud/hud_math.h>
#include <runtime/renderer/aspect_ratio.h>

namespace opennova::hud {

// Both scope range and mortar impact labels format STROVER_DIST, whose
// installed text uses %ld. Retail's 32-bit long is independent of our ABI.
// [orig: HUD_DrawScopeOverlayDetails @ 0x59E420, sprintf @ 0x59E530;
// HUD_RenderAllOverlays @ 0x5A8070, sprintf @ 0x5A8972]
// docs/interface/hud-re.md (D-HUD-30).
std::string sight_integer_text(const std::string &format, int32_t value) {
    std::string out;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '%' && i + 1 < format.size()) {
            size_t conversion = i + 1;
            if (format[conversion] == '%') {
                out += '%';
                i = conversion;
                continue;
            }
            if (format[conversion] == 'l') ++conversion;
            if (conversion < format.size()) {
                const char kind = format[conversion];
                if (kind == 'd' || kind == 'i' || kind == 'u') {
                    out += kind == 'u' ? std::to_string(static_cast<uint32_t>(value))
                                       : std::to_string(value);
                    i = conversion;
                    continue;
                }
            }
        }
        out += format[i];
    }
    return out;
}

int next_sight_scale_index(int index) {
	// [orig: `mov eax,dword_B76780; add eax,ebx` (ebx = 1) @0x4e0c31..0x4e0c36;
	//  `cmp eax,3; mov dword_B76780,eax; jl` keeps the sum @0x4e0c38..0x4e0c40;
	//  else `mov dword_B76780,0` @0x4e0c46]. The compare is signed, so a
	// negative index climbs back toward the default rather than wrapping.
	const int next = index + 1;
	return next < kSightScaleIndexCount ? next : 0;
}

int32_t sight_scaled_half_extent(int32_t extent, int sight_scale_index) {
	// [orig: `add eax,2; imul eax,extent; sar eax,3` @0x4dcead..0x4dcecc]
	return (extent * (static_cast<int32_t>(sight_scale_index) + 2)) >> 3;
}

SightRect sight_row_rect(const SightRowSpec &row, int sight_scale_index,
		int32_t slide_multiplier) {
	SightRect out;
	out.x1 = row.x1;
	out.y1 = row.y1;
	out.x2 = row.x2;
	out.y2 = row.y2;
	if (row.scale) {
		// [orig: `cmp dword ptr [esi+1Ch],0` @0x4dce8d; the centre halves
		//  @0x4dcec4/@0x4dceca; top-left @0x4dcecf/@0x4dcee6, bottom-right
		//  @0x4dcf18/@0x4dcf2c]
		const int32_t center_x = (row.x1 + row.x2) >> 1;
		const int32_t center_y = (row.y1 + row.y2) >> 1;
		const int32_t half_w = sight_scaled_half_extent(row.x2 - row.x1, sight_scale_index);
		const int32_t half_h = sight_scaled_half_extent(row.y2 - row.y1, sight_scale_index);
		out.x1 = center_x - half_w;
		out.y1 = center_y - half_h;
		out.x2 = center_x + half_w;
		out.y2 = center_y + half_h;
		return out;
	}
	if (row.slide) {
		// [orig: `cmp dword ptr [esi+20h],0` @0x4dcf42; edi = [esi+18h] *
		//  multiplier; `add ecx,edi` onto y1 @0x4dd01b and y2 @0x4dd049]
		const int32_t offset = row.slide_frames * slide_multiplier;
		out.y1 += offset;
		out.y2 += offset;
	}
	return out;
}

SightViewportRect sight_rect_to_viewport(const SightRect &rect, float width, float height, int aspect_mode) {
	if (width <= 0.0f || height <= 0.0f) return {};
	const float aspect = renderer::aspect_height_over_width(aspect_mode, width, height);
	const float correction = 3.0f / (4.0f * aspect);
	const float center_y = height * 0.5f;
	SightViewportRect out;
	out.x1 = static_cast<float>(scale_axis(rect.x1, width, kDesignWidth));
	out.x2 = static_cast<float>(scale_axis(rect.x2, width, kDesignWidth));
	const float y1 = static_cast<float>(scale_axis(rect.y1, height, kDesignHeight));
	const float y2 = static_cast<float>(scale_axis(rect.y2, height, kDesignHeight));
	out.y1 = center_y + (y1 - center_y) * correction;
	out.y2 = center_y + (y2 - center_y) * correction;
	return out;
}

int32_t sight_slide_multiplier(const ScopeZeroInputs &in) {
	// [orig: `movzx ecx,word ptr [ebx+60h]; xor edi,edi; test cx,cx; jnz`
	//  @0x4dcf4c..0x4dcf55; `mov eax,[edx+0A0h]; test eax,eax; jz` @0x4dcf5a..0x4dcf62]
	const int16_t zero_word = in.slot_zero_word;
	if (zero_word == 0 && in.scope_zero_default != 0) {
		// [orig: `cmp byte_24D217C,cl; jnz loc_4DD000` @0x4dcf64 — cl is the
		//  zero word's low byte, 0 on this arm, so a set byte skips to the
		//  y-offset apply with edi still 0]
		if (in.scoring_disabled) return 0;
		// [orig: `shl eax,10h` @0x4dcf70; the signed magic division by
		//  1638400 @0x4dcf75..0x4dcf84; Weapon_GetScopeZoomLevel(0, that)
		//  returns the default @0x4dcf88 -> @0x422fd1..0x422fd5; `imul eax,19h`
		//  @0x4dcf8d; `cdq; idiv dword ptr [ecx+9Ch]` @0x4dcf9f..0x4dcfa6]
		if (in.scope_zero_step == 0) return 0;
		const int32_t default_zoom =
				static_cast<int32_t>(static_cast<uint32_t>(in.scope_zero_default) << 16) /
				kScopeZeroDefaultDivisor;
		return default_zoom * kScopeZeroDefaultStepMetres / in.scope_zero_step;
	}
	if (zero_word == -1) {
		// [orig: `cmp cx,0FFFFh; jnz` @0x4dcfb1; `mov edi,[ecx+9Ch]; shl edi,10h;
		//  cdq; idiv edi` @0x4dcfcb..0x4dcfd5; `test eax,eax; jge` @0x4dcfd7
		//  (negative -> `xor eax,eax` @0x4dcfde); `cmp eax,[ecx+84h]; jle;
		//  mov eax,ecx` @0x4dcfe5..0x4dcfef]
		const int32_t range_units =
				static_cast<int32_t>(static_cast<uint32_t>(in.scope_zero_step) << 16);
		if (range_units == 0) return 0;
		const int32_t steps = in.rangefinder_q16 / range_units;
		if (steps < 0) return 0;
		return steps > in.scope_max_zero_steps ? in.scope_max_zero_steps : steps;
	}
	// [orig: `movsx edi,cx` @0x4dcff9]
	return static_cast<int32_t>(zero_word);
}

} // namespace opennova::hud
