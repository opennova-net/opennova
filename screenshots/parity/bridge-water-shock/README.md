# Bridge water-shock parity

Deterministic OpenNova runtime capture for D-ITEM-19. The probe mounts the
retail particle catalog and renders the same bridge/water stage on both sides.

| Before destruction events | After the UnitType 11 port |
|---|---|
| ![Bridge stage before destruction events](before.png) | ![Three Effect_ShockWaterBrdg event anchors on the water plane](after.png) |

The cyan rings are a diagnostic overlay at the three ordinary, unowned
`Effect_ShockWaterBrdg` submissions—the same event family now emitted once per
transformed husk `DEAD` point at the authoritative water plane. This installed
retail base/RevX02 catalog does not author that exact effect name, so both retail
and OpenNova resolve it through the intentionally invisible `stockeffect` path;
the probe does not substitute the visible but different `Effect_ShockWater`.
The bridge geometry is a deterministic probe fixture, not a retail mission
capture.
