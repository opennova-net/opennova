// Windows-1252, the code page a stock game server reads a ServerCommand's text
// in (the service converts each arg to it and refuses a character it has no
// byte for; apps/novaworld_server/README.md, "Pushing statements to a listed
// server"). Every character cp1252 has is one byte there, so a text's length
// on the host is its count of characters.

// The characters at 0x80..0x9F, the rest of 0x00..0xFF being Latin-1's.
const CP1252_HIGH = new Set<number>([
  0x20ac, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039,
  0x0152, 0x017d, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122,
  0x0161, 0x203a, 0x0153, 0x017e, 0x0178,
]);

// Whether a printable character has a cp1252 byte (control characters never
// pass the service).
function fits(codepoint: number): boolean {
  return (
    (codepoint >= 0x20 && codepoint < 0x7f) ||
    (codepoint >= 0xa0 && codepoint <= 0xff) ||
    CP1252_HIGH.has(codepoint)
  );
}

// The text's length on the host: one byte per character.
export function cp1252Length(text: string): number {
  return Array.from(text).length;
}

// Why the service would refuse `text` as a name, message or chat line, or
// null: a character the host's code page has none for, or more characters
// than `cap` (the bytes the host keeps).
export function cp1252Problem(text: string, cap?: number): string | null {
  const missing: string[] = [];
  for (const ch of Array.from(text)) {
    const codepoint = ch.codePointAt(0) ?? 0;
    if (!fits(codepoint) && !missing.includes(ch)) missing.push(ch);
  }
  if (missing.length > 0) {
    const shown = missing.slice(0, 4).map((ch) => `“${ch}”`).join(', ');
    return `The game server's text is Windows-1252, which has no character for ${shown}. Remove ${
      missing.length === 1 ? 'it' : 'them'
    } and try again.`;
  }
  const length = cp1252Length(text);
  if (cap !== undefined && length > cap) {
    return `The game server keeps ${cap} characters at most; this is ${length}.`;
  }
  return null;
}
