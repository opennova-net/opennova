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
- **Every mutation goes through the shared `NovaDebugSession`.** This includes
  transport, vars, terrain draw modes, environment values, and dynamic
  AudioServer bus controls. Register a typed control with a re-resolving
  public target, then invoke it through the session from the page.
- The F3 overlay is only one presentation of that session. Runtime automation
  must see and exercise the same controls, policy checks, live readback, and
  errors. Pages render state; they never call engine setters or `AudioServer`
  mutations directly.
- Original host-owned checks remain declarative rows in
  `nova_debug_options.gd` and use `add_option_check(&"my_option")` in
  `_build()`. Page-specific and dynamic controls may register outside that
  legacy table, but still belong to the shared session catalog.
- If a system is not observable yet, add a minimal accessor to its owner
  (a `get_*_debug() -> Dictionary` on NovaSimulation, or a one-line node
  getter on GameWorld) — never reach into privates.

## Rules

- Shell-neutral: engine/ui primitives only, no game/ or modtools/ imports.
- Min content width <= 360 px (the panel width floor minus the sidebar);
  wide tables shrink their column minimums instead of overflowing.
- Artist-facing copy ("draw distance", not "CDEP"); no `print` — failures
  degrade to empty states or `push_warning` per the engine error policy.
