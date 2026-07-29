# F3 debug pages

One script per page of the F3 debug overlay (`nova_debug_overlay.gd`), the
project's live-inspection surface. The overlay is the shell — sidebar,
resizable panel, refresh timer, persistence — and every readout/knob lives on
a page here. Every major engine system should have (or grow) a page: see what
it is doing, turn its knobs.

## Adding a page

1. Create `debug_<system>_page.gd`:

```gdscript
class_name DebugMySystemPage
extends NovaDebugPage

func page_id() -> StringName:        # stable: node name, select_page() key,
    return &"MySystem"               # persisted last-page value

func page_title() -> String:         # artist-facing sidebar label
    return "My system"

func page_category() -> StringName:  # CATEGORY_SIM / _WORLD / _PLAYER / _DIAGNOSTICS
    return CATEGORY_WORLD

func _build() -> void:               # one-time UI; NEVER a sim read
    add_theme_constant_override("separation", 6)
    ...

func refresh() -> void:              # 0.25 s cadence, ONLY while active
    var sim := _ctx.sim()            # resolve per call; render an empty state
    if sim == null: ...              # when a source is gone
```

2. Register it in `nova_debug_overlay.gd`'s `_build_default_pages()` (one
   line). Hosts and tests can also `overlay.register_page(page)` at runtime —
   a custom category grows its own sidebar section.
3. Add `godot/tests/debug_<system>_page_test.gd`: drive the page with a
   fabricated `NovaDebugContext` + duck-typed stubs (empty states, canned
   formatting, knob pokes + mirror-back).

## Data and knobs

- All live data comes through `NovaDebugContext` (`_ctx.runtime()/sim()/
  world()/effect_world()/view_context()`), re-resolved on every call and
  `has_method`-guarded — mission reloads must never leave a stale reference,
  and harness stubs must degrade to empty states.
- **Host-actionable toggles** (the host must build/free a world view or flip
  host-owned state) are declarative: one row in `nova_debug_options.gd` +
  `add_option_check(&"my_option")` in `_build()`. Both hosts consume the row
  through one generic handler — zero host edits per new toggle.
- **Page-local knobs** that poke a live object directly (terrain draw mode,
  env time-of-day, AudioServer mutes) skip the registry: poke in the control
  handler, and re-mirror the live value each `refresh()` (`select()` /
  `set_value_no_signal`) so an external poke never fights the UI.
- If a system is not observable yet, add a minimal accessor to its owner
  (a `get_*_debug() -> Dictionary` on NovaSimulation, or a one-line node
  getter on GameWorld) — never reach into privates.

## Rules

- Host-neutral: engine/ui primitives only, no game/ or modtools/ imports.
- Min content width <= 360 px (the panel width floor minus the sidebar);
  wide tables shrink their column minimums instead of overflowing.
- Artist-facing copy ("draw distance", not "CDEP"); no `print` — failures
  degrade to empty states or `push_warning` per the engine error policy.
