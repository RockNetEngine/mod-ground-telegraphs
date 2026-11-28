# Using mod-ground-telegraphs

How to preview marks, call them from C++, and build data-driven bosses with attack visuals. Install and client setup:
[README](../README.md). What changed between versions: [CHANGELOG](../CHANGELOG.md).

## Preview in game

GM command, drawn at your feet facing where you face:

```
.groundmark line  [length=40] [width=2]  [ms=3000] [friendly 0/1]
.groundmark cone  [length=40] [angle=60] [ms] [friendly]
.groundmark nova  [radius=10]            [ms] [friendly]
.groundmark donut [outer=40]  [inner=20] [ms] [friendly]
.groundmark reload
```

## C++ API

```cpp
#include "ground_telegraph.h"

GroundTelegraph::Mark m;
m.shape = GroundTelegraph::SHAPE_CONE;                 // LINE, CONE, NOVA, DONUT
m.x = boss->GetPositionX(); m.y = boss->GetPositionY(); m.z = boss->GetPositionZ();
m.facing = boss->GetAngle(target);
m.size = 30.0f;                                        // length / radius / outer radius
m.param = 60.0f;                                       // width / angle / inner radius
m.durationMs = 3000;                                   // the nearest fill time in the grid is used
ObjectGuid mark = GroundTelegraph::Draw(boss, m);      // despawns by itself ~2 s after its fill
uint32 landIn = GroundTelegraph::HitMs(m);             // the real fill (+ FULL_HOLD_MS, 0): land the attack then

// when it lands: remove the mark first (at once, no fade), then hit
GroundTelegraph::Remove(boss, mark);
if (GroundTelegraph::Contains(m, player->GetPositionX(), player->GetPositionY()))
    /* hit the player */;
```

## Data-driven bosses

Give a creature `ScriptName = 'npc_ground_telegraph_boss'` and add rows. A full example (creature 9530998 with every
shape) is in `data/sql/examples/example_boss.sql`; run it by hand and `.npc add 9530998`.

`ground_telegraph_boss` (optional, one row per entry)

| column | meaning |
|---|---|
| `melee` | 1 = melees and chases like any creature (default), 0 = a caster |
| `chase_dist` | caster: 0 = rooted, > 0 = keeps this distance |
| `yield_pct`, `yield_say` | at this health % it gives up: kill credit to players within 40 yd, turns friendly, leaves |
| `visual_caster` | invisible creature that plays `visual_kit` (0 = 9530999) |
| `death_summon`, `death_x/y/z/o`, `death_despawn_s` | summon a creature there when the boss dies |
| `walker_entry`, `walker_x/y/z`, `walker_say` | when the boss dies, the nearest creature of that entry walks there and leaves |

`ground_telegraph_attack` (one row per attack; each has its own timer; one attack at a time)

| shape | marks | `size` (default) | `param` (default) |
|---|---|---|---|
| 1 line | from the boss's edge toward its target | length (40) | width (2) |
| 2 cone | from the boss's edge toward its target | length (40) | angle in degrees (60) |
| 3 nova | around the boss | radius (10) | |
| 4 donut | around the boss, safe middle | outer radius (20) | inner radius (size / 2) |
| 5 erupt | `count` (3) small novas: one on the target, the rest within `spread` (8) yd of it | radius of each (3) | |
| 6 dance | rings around the boss from `bands`, e.g. `0-10,20-40` | | |
| 7 cast | no mark: a real cast of `hit_spell` at the target (interruptible unless the spell isn't) | | |

Other columns: `delay_ms` (the fill time; the nearest one in the grid is used, then the mark is removed the moment its
fill ends and the attack lands), `every_ms` (0 = only as another attack's `next_attack`),
`first_ms`, `next_attack` (the attack that follows at once: chain two dance rows for "safe ring, then the inverse"),
`hit_spell` + `hit_damage` (exact direct damage of that spell's school, never misses; without `hit_damage` the spell is
cast on each player hit: a stun, a knockback, a heal), `friendly` (green mark, hits friendly players: heals),
visual columns (below). Players in a mark when it lands are hit once per attack (2D test, within 8 yd of the mark's
height). Bosses send no text for their attacks (it spammed long fights). `.groundmark reload` re-reads the tables.

### Attack visuals

When an attack lands, a SpellVisualKit (`visual_kit`) plays once on each of a set of invisible casters, which are placed
when the mark goes down (the client has to know a unit before it can play a kit on it). A kit's size is the caster's
scale times the kit effect's own `Scale` (SpellVisualEffectName field 4, clamped to its min / max): rows with Scale 1 make
`visual_scale` the size. A gameobject with a spell model is no substitute: the client loops the model while the object
fades in and out, so it plays 2-3 times. Kits are client data: this module ships no kit ids (use stock kits, or add your
own SpellVisualKit / SpellVisualEffectName rows to your client patch and server DBC).

| column | meaning |
|---|---|
| `visual_kit` | kit id; 0 = no visual |
| `visual_mode` | 0 by shape (cone wave, nova single, erupt single per mark, donut / dance spokes), 1 wave, 2 scatter, 3 single, 4 ring, 5 spokes |
| `visual_scale` | caster scale of each burst (0 = 1). Single on a nova: the scale for a 10 yd radius, it follows the radius |
| `visual_spacing` | yd between bursts (0 = 2); a wave over a big mark widens it to stay under 150 casters |
| `visual_pace` | ms for a wave to cross the mark, or the window in which scattered / ring bursts go off (0 = 800) |
| `visual_zoff` | yd added to each burst's ground height (models that rise with scale; negative sinks them: the caster flies, so it keeps a height below the ground) |
| `beam_spell` | a no-damage visual spell the boss casts when it lands: a line = a beam to a caster at the far end; erupt = a missile onto each mark, launched so it lands on the hit (travel = distance / the spell's `Speed`) |
| `nova_kit`, `nova_scale`, `nova_zoff` | dance / donut: every disc band plays this kit once in its centre (`nova_scale` = the scale for a 10 yd radius); ring bands keep `visual_kit` |

Wave = rows (cone) or rings (disc, donut) sweeping out from the origin; scatter = points over the area at random moments;
single = one burst at the centre of each mark; ring = points on the middle of each ring; spokes = radial lines across a
ring (ring width / spacing bursts each, 1-5), one seed spoke per ~25 yd of circle going off first and a wave running both
ways to the next seed within `visual_pace`: deterministic, e.g. a 16-30 yd ring at 4 yd spacing = 36 spokes x 4 = 144 bursts. Lines use `beam_spell`.

## Limits

- Very steep slopes stretch a mark (the texture is projected along the mark's axis; the server tilts it to the average
  slope of its area).
- Proportions and fill times come from the generated grid; the server uses the nearest one. A line keeps its length
  exact; add a proportion to the grid if you need an exact width.
- Marks are gameobjects: players far away (outside visibility range) do not see them.
