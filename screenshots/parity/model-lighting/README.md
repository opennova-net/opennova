# Model-lighting parity capture (`revx02`)

This comparison is a fresh same-pose `00TRa.bms` training-courtyard capture.
Both sides explicitly select `revx02`; the older retail panel with no
expansion provenance is not reused.

![Retail and OpenNova revx02 model-lighting comparison](courtyard-retail-vs-opennova.png)

The normalized median model-brightness factor is `1.048` (`1.0` is the retail
target; the enforced range is `0.850..1.150`). The gate compares the two raw
frames below rather than cropping the superseded composite.

## Retail provenance

The accepted retail frame was captured from a fresh process:

```text
"C:\GAMES\JOTAC\Game\JO\Jointops.exe" /w /many /game jo /exp revx02 /frisk
```

- Retail title: `Joint Operations: Typhoon Rising V1.7.5.7`
- Process start: `2026-07-30T00:04:25.9730445-04:00`
- `Jointops.exe` SHA-256:
  `B9971C8273B7BBB1C8518A738596D669CD7794E9D307AE63A7A9A530EB802FAC`
- The fresh `/frisk` log starts by loading
  `expansion\revx02\revx02.pff`, `revx02L.pff`, and `revx02.bin`, then records
  `PFF LOADED FILE: 00TRA.BMS` in the same session.
- Expansion archive SHA-256:
  `revx02.pff`
  `E20F2D81D26B82A616772D63F8844613D3FB400B10A0D5EDE841D512DE52E8A5`;
  `revx02L.pff`
  `C379D8A6239D4B885DEDB852FDA3439532A39C2E6C2834746F2BEF53C9E30A18`.
- The first gameplay attempt was rejected because a briefing-screen mouse
  click pitched the camera down. The accepted frame came from an in-session
  restart with the cursor parked at client center and no subsequent gameplay
  input. It is the doorway-forward pose: crates left, window right, and
  `32 m to Marketplace`.
- Raw client frame:
  [`courtyard-retail-revx02.png`](courtyard-retail-revx02.png), `1920x1080`,
  SHA-256
  `6974947EC521BB14C3C333DC9681DAC9D2EFAA1A44031196043AC8A474B1FCA9`.

## OpenNova provenance

The capture probe first opens the loose authoring copy of `00TRa`, then
replaces that root with a packed runtime mount before Play Mission. It requires
the mount-reported expansion to equal the requested expansion, so a missing
expansion's silent fallback to base assets fails the capture.

The accepted run reported:

```text
requested=revx02 actual=revx02 mount=packed
00TRa.bms -> localres.pff
dvxg6.trn -> resource.pff
FULL_00.ENV -> resource.pff
```

Those are the actual winning packed sources. In particular, `00TRa.bms`
legitimately comes from the base `localres.pff`; expansion provenance does not
claim that every winning file lives in `RevX02.pff`. A negative real-data run
with a missing expansion was rejected as a fallback to base assets.

- Raw OpenNova frame:
  [`courtyard-opennova-revx02.png`](courtyard-opennova-revx02.png), `1600x900`,
  SHA-256
  `ECE2BB981C096D12D91170ADBC7CE269AB501907D14ADC28F692CEF4E0959DDD`.
- Player origin: `(297.8055725, 28.0037384, 409.1231689)`
- Camera origin: `(297.9372, 28.72005, 409.1305)`
- Camera basis: identity
- Vertical FOV: `50.5340`
- Mission time: approximately `15:00`
- Resolved interior items.def ID: `101216` (`Ihq01`)

The generated side-by-side is `2560x756`, SHA-256
`F4F2FDB5B2D4D5A0EC185702E0169E01E2247ABA1A3E32CF80224B716F4E716D`.

## Reproduce

Generate and validate a fresh OpenNova image:

```powershell
.\scripts\render\test_model_lighting_parity.ps1 `
    -GodotPath $env:GODOT_BIN `
    -MissionResourceDir $env:OPENNOVA_MISSION_CORPUS `
    -RuntimeResourceDir $env:OPENNOVA_JO_DIR `
    -Expansion revx02 `
    -RetailImage .\screenshots\parity\model-lighting\courtyard-retail-revx02.png `
    -OutputDir .\.scratch\model-lighting-parity
```

Rebuild the side-by-side:

```powershell
.\scripts\render\build_model_lighting_comparison.ps1 `
    -RetailImage .\screenshots\parity\model-lighting\courtyard-retail-revx02.png `
    -CurrentImage .\screenshots\parity\model-lighting\courtyard-opennova-revx02.png
```

The visual comparison is a parity witness, not a pixel-exact golden. Its
historical capture predates the runtime `LGHT` correction: retail and OpenNova
now both instantiate authored model lights through EffectWorld, while
OpenNova's current delivery is a camera-global object approximation rather
than retail's per-draw owner/interior selection. Streamed shadow admission,
static-light destruction rebinding, and the remaining outdoor per-entity
sun-visibility work are covered in
[`render-lighting-re.md`](../../../docs/render/render-lighting-re.md).
