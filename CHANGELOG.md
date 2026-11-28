# Changelog

## 1.1.1 (2026-11-28)

### Fixed
- The mark still showed under the first bursts of an attack visual (it was removed in the same server update as the
  hit). It is now removed `CLEAR_BEFORE_HIT_MS` (150 ms) before the hit and the visual. New API
  `GroundTelegraph::RemoveMs`; `HitMs` = `RemoveMs` + 150.

## 1.1.0 (2026-11-28)

Attack visuals and timing. Tested in game on an AzerothCore Playerbots server; compile-checked (`-fsyntax-only`, no PCH)
against azerothcore-wotlk master 65e5a98 (2026-11-28).

### Added
- **Attack visuals**: when an attack lands, a SpellVisualKit plays once on a set of invisible casters placed when the mark
  goes down. New `ground_telegraph_attack` columns `visual_kit`, `visual_mode` (0 by shape, 1 wave, 2 scatter,
  3 single, 4 ring, 5 spokes), `visual_scale`, `visual_spacing`, `visual_pace`, `visual_zoff`.
- **Spokes** (`visual_mode` 5, the default on donut and dance rings): radial lines of bursts across a ring, seed spokes
  first, then a wave both ways to the next seed.
- **`beam_spell`**: a no-damage visual spell cast when the attack lands: a beam to a caster at the far end of a line, or a
  missile onto each erupt mark, launched so it arrives on the hit.
- **`nova_kit`, `nova_scale`, `nova_zoff`**: one nova kit in the centre of each disc band of a dance or donut.
- API: `GroundTelegraph::FillMs`, `GroundTelegraph::HitMs` (the fill really drawn, from the grid) and `GroundTelegraph::Remove`
  (removes a mark at once, without the despawn fade).

### Changed
- Timing: the mark fills, disappears the moment the fill ends, and then the hit and its visual land. The boss lands each
  attack at `HitMs` of its first mark (1.0 used `delay_ms`, but the fill is the nearest duration in the grid, so a short
  delay drew a longer fill).
- The visual caster 9530999 flies (`creature_template_movement`, as the stock Invisible Stalker), so a kit below the
  ground (negative `visual_zoff`) still shows.
- GM command `.telegraph` renamed to **`.groundmark`** (it clashed with `.tele`).
- Bosses send no attack text (it spammed long fights).

### Removed
- Columns `visual_spell`, `say`, `text_type`, `interrupt_say`, `advice` (attack) and `advisor_entry` (boss). The
  migrations drop them; move a visual to `visual_kit` or `beam_spell`.

### Upgrading from 1.0
Pull, rebuild the worldserver, start it: the DB updater applies `2026_11_27_2330_ground_telegraph_kit_visuals.sql` and
`2026_11_27_2352_ground_telegraph_beams.sql` (conditional ALTERs, safe to run on a 1.0 database) and the updated base SQL.
The client patch is unchanged (kits are client data you add yourself; see "Attack visuals" in [docs/USAGE.md](docs/USAGE.md)).

## 1.0.0 (2026-11-24)

First release: projected ground marks (line, cone, nova, donut) that follow terrain and building floors, a growing fill,
the C++ API, the `.telegraph` GM command and the SQL-driven boss AI (`npc_ground_telegraph_boss`). The client art ships
as `patch-G.MPQ`; `GameObjectDisplayInfo.dbc` is built by each server from its own copy.
