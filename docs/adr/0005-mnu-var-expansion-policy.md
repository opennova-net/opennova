# `%VAR%` expansion: keep raw tokens in the document, expand per field at build

The original engine expands `%VAR%` tokens over the **whole `.mnu` buffer before XML parsing** ([orig: `NapiXML_ExpandVariablesInText` @ 0x63a000, called from `UIScene_LoadAndParseContent` @ 0x63c830]). A variable can therefore appear in any field — text, a `<POSITION>` edge, a color, a font name, any attribute — and by the time the element parser ([orig: `CUIElement_ParseXMLDefinition` @ 0x648120]) reads, say, a color, it already sees the substituted hex string. The variable list is host-supplied (game state, stylesheet defines merged in by the host).

We do not pre-expand. `NovaMnuDocument` parses once and keeps the **raw `%VAR%` tokens** in the model; the ONED editor reads and re-serializes that model, so a menu authored with `%DEF_TEXT_FG%` round-trips with the token intact rather than being baked to a concrete value on first save. The runtime builder (`nova_mnu_builder.cpp`) expands `%VAR%` **per consumed field** at build time, through the `MnsStyleSheet` (`substitute_var`), covering the same field set the engine's whole-buffer pass would: colors, fonts, textures, and literal text.

## Why the split

A whole-buffer pre-parse expansion is incompatible with lossless authoring: it would erase the token the editor needs to round-trip. Expanding at build time keeps the document faithful to the bytes on disk while the *rendered* result matches the engine. This is the same preserve-vs-honor boundary as [ADR 0002](0002-mnu-round-trip-preserves-superset.md): the model preserves the authored token; the runtime honors it by substituting at use.

## Consequences and the documented gap

- The runtime **result** matches the engine for any `%VAR%` resolvable from the stylesheet (the source of the defines real menus use for color/font theming).
- **Host variables in non-themed fields** (e.g. `%PLAYERNAME%` / `%VERSIONSTRING%` in `<STRING>` text) are a known gap: no host-provided variable map is plumbed into the builder yet, so such a token renders literally instead of expanding. Wiring a host var map into `MnuBuildContext` is the follow-up; it does not change this policy, only the source of the substitution table.
- Because expansion is per field, a `%VAR%` that resolves to a fragment spanning a parse boundary (e.g. a variable holding `left="10" top="20"`) is **not** supported — the engine's pre-parse pass would handle it, ours would not. No shipped menu uses a variable this way; it is called out only for completeness.
