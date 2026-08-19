# Glass-userpoint shatter parity

Deterministic OpenNova presentation capture for D-ITEM-17.

![Broken in-range glass point beside an intact out-of-range point](capture.png)

The stage feeds the seeded native witness's ordered Glass/Paper/Dust rows
through the production `DestructionPresentPass` and mounted retail particle
catalog. The cyan fragments and rings are diagnostic overlays at the resolved
event point; the facade is a deterministic probe fixture, not a retail mission
capture.

Native `test_glass_userpoints_shatter_once_at_authored_radius` pins full-Euler
placement, the authored `kz_maxradius=8` gate despite a queued radius override
of 30, exact two-draw-per-slot PRNG advancement, ordered effect names, and
break-once behavior. GUT
`test_retail_glass_model_maps_exact_userpoint_into_death_traits` pins the exact
case-insensitive stock graphic/userpoint map and 3DI axis conversion; the
presenter test pins verbatim delivery.

## Reproduce

```powershell
$env:NOVA_PARITY_PROBE_MODE = 'glass'
$env:NOVA_PARITY_PROBE_DIR = (Resolve-Path '.\screenshots\parity\glass-shatter')
$env:NOVA_RESOURCE_DIR = $env:OPENNOVA_JO_DIR
& $env:GODOT_BIN --path godot res://tests/retail_parity_visual_probe.tscn
```
