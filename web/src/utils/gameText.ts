// The game's inline text markup, as its font drawer reads it: a port of
// GameFont::parse_format_tag (engine/runtime/hud/game_font.cpp) [orig:
// GText_ParseFormatTag @ 0x674200] and the draw walk's '\' and '<' arms
// [orig: CGameFont_DrawText @ 0x6752c0]. Server names, missions, game types and
// messages on NovaWorld carry it ("<c00FF00>COOP/AAS</c>"), so the site shows
// them as the game's browser does. parseGameText yields styled runs, which
// components/GameText.vue renders as spans (never as HTML); stripGameText is
// the visible text alone, for sorting and filtering.

// The live colour: the caller's own (the original), the original's
// half-bright, or a #RRGGBB the text set.
export type GameTextColor = 'original' | 'half' | `#${string}`;

export interface GameTextStyle {
  bold: boolean;
  italic: boolean;
  underline: boolean;
  color: GameTextColor;
}

export interface GameTextRun extends GameTextStyle {
  text: string;
}

const PLAIN: GameTextStyle = { bold: false, italic: false, underline: false, color: 'original' };

function hexValue(c: string): number {
  if (c >= '0' && c <= '9') return c.charCodeAt(0) - 48;
  if (c >= 'A' && c <= 'F') return c.charCodeAt(0) - 55;
  if (c >= 'a' && c <= 'f') return c.charCodeAt(0) - 87;
  return -1;
}

// The bytes the drawer skips: controls below space, and 0x7F, 0x80 and 0x81
// [orig: CGameFont_DrawText @ 0x6752c0, v11 < 0x20 || 0x7F || 0x80 || 0x81;
// fnt.h's fnt_byte_is_nonprinting]. The service reads the host's cp1252, where
// 0x80 is the euro sign.
function isSkipped(c: string): boolean {
  const code = c.codePointAt(0) ?? 0;
  return code < 0x20 || code === 0x7f || code === 0x80 || code === 0x81 || code === 0x20ac;
}

// One tag from the '<' at chars[start]: its '>' and the style after it, or null
// when no '>' ends it (nothing commits then, and the '<' draws).
function parseTag(chars: string[], start: number, style: GameTextStyle): { end: number; style: GameTextStyle } | null {
  const local: GameTextStyle = { ...style };
  // A '-' turns off whatever follows it in the tag.
  let off = false;
  for (let i = start + 1; i < chars.length; i++) {
    const c = chars[i];
    if (c === '>') return { end: i, style: local };
    switch (c) {
      case '-':
        off = true;
        break;
      case 'B':
      case 'b':
        local.bold = !off;
        break;
      case 'I':
      case 'i':
        local.italic = !off;
        break;
      case 'U':
      case 'u':
        local.underline = !off;
        break;
      case 'C':
      case 'c': {
        // The rest of the tag is the colour: hex digits, O for the original
        // colour, H for its half-bright. The original shifts the original
        // colour's bits under any digits that follow an O or H; the page has
        // no such bits, so a digit after one starts a new value (no tag the
        // game writes does that).
        let value = 0;
        let named: 'original' | 'half' | null = null;
        for (i++; i < chars.length && chars[i] !== '>'; i++) {
          const h = chars[i];
          const digit = hexValue(h);
          if (digit >= 0) {
            if (named) value = 0;
            named = null;
            value = (value * 16 + digit) >>> 0;
          } else if (h === 'O' || h === 'o') {
            named = 'original';
          } else if (h === 'H' || h === 'h') {
            named = 'half';
          }
        }
        if (i >= chars.length) return null;
        if (named) {
          local.color = named;
        } else if (value !== 0) {
          // The value's low 24 bits replace the live colour's; a zero value
          // ("</c>", "<-c>") changes nothing [orig: @0x674346..0x674357].
          local.color = `#${(value & 0xffffff).toString(16).toUpperCase().padStart(6, '0')}`;
        }
        return { end: i, style: local };
      }
      case 'T':
      case 't':
        // A tab width to the '>': the page lays out no tab stops.
        for (i++; i < chars.length && chars[i] !== '>'; i++);
        if (i >= chars.length) return null;
        return { end: i, style: local };
      default:
        // Any other letter in a tag does nothing.
        break;
    }
  }
  return null;
}

function sameStyle(a: GameTextStyle, b: GameTextStyle): boolean {
  return a.bold === b.bold && a.italic === b.italic && a.underline === b.underline && a.color === b.color;
}

// The text as the game draws it: its runs, each in one style, tags left out.
export function parseGameText(text: string | null | undefined): GameTextRun[] {
  const runs: GameTextRun[] = [];
  let style = PLAIN;
  const draw = (c: string) => {
    const last = runs[runs.length - 1];
    if (last && sameStyle(last, style)) last.text += c;
    else runs.push({ ...style, text: c });
  };
  // The drawer reads a line at a time [orig: CGameFont_ReadLine @ 0x676480],
  // so a tag never runs past its line's end; the style carries on.
  (text ?? '').split('\n').forEach((line, n) => {
    if (n > 0) draw('\n');
    const chars = Array.from(line);
    for (let i = 0; i < chars.length; i++) {
      let c = chars[i];
      if (c === '\\') {
        // The next character is drawn as it is: a glyph for any but a
        // control below space, which has none.
        i++;
        if (i >= chars.length) break;
        c = chars[i];
        if (c < ' ') continue;
      } else if (c === '<') {
        const tag = parseTag(chars, i, style);
        if (tag) {
          style = tag.style;
          i = tag.end;
          continue;
        }
        // Unterminated: the '<' draws, and the walk goes on after it.
      } else if (c === '\t') {
        c = ' ';
      } else if (isSkipped(c)) {
        continue;
      }
      draw(c);
    }
  });
  return runs;
}

// The visible text alone, for sorting and filtering.
export function stripGameText(text: string | null | undefined): string {
  return parseGameText(text)
    .map((run) => run.text)
    .join('');
}
