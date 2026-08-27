# Attach-label fire-gate parity

Deterministic OpenNova runtime capture for D-HUD-11.

![Scope-down, settled-ADS, and immediate third-person attach-label states](capture.png)

The probe runs a real `Simulation` with two separate B50Cal fixture entities and
one ordinary Scoped weapon. It records the public `get_attach_labels()` output
with the scope down, after the promoted ADS endpoint, and immediately after a
third-person camera toggle without another logic tick. The top-down cards are a
diagnostic projection of those returned positions and nearest flags, not a
retail mission capture.

The expected counts are `2 → 1 → 2`: a complete
`Player_CanFireWeapon @0x5cf780` verdict limits a ready player to the nearest
candidate entity, while a blocked fire verdict exposes both. GUT
`test_attach_labels_share_complete_can_fire_verdict` also pins the underwater
leg.

## Reproduce

```powershell
python scripts/mcp/game_mcp.py launch --windowed --resource-dir $env:OPENNOVA_JO_DIR
python scripts/mcp/game_mcp.py probe run retail_parity_visual '{"mode":"attach","output_dir":"screenshots/parity/attach-label-can-fire"}' --wait
python scripts/mcp/game_mcp.py stop
```
