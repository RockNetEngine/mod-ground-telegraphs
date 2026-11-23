# mod-ground-telegraphs

Retail-style ground telegraphs for AzerothCore (WotLK 3.3.5a): lines, cones, novas and donuts painted **on the ground**
that follow terrain and building floors, sized exactly to the area an attack will hit, with a fill that grows to the
edge when the hit lands. Plus data-driven telegraph bosses you can make with SQL only.

- Exact sizes: a 2 x 40 yd beam, a 60° cone of 30 yd, a 10 yd nova, a 20–40 yd donut look exactly as big as they hit.
- They lie on the ground like the spell targeting circle (terrain, building floors, uneven ground).
- Hostile marks are orange-red, friendly (heal) marks green.
- A fill grows from the boss to the edge over the cast time (the donut fades in), so players see when it lands.
- A C++ API any module or script can call, a GM command to preview, and an SQL-driven boss AI.
- Self-made art: no Blizzard or retail files are shipped or needed.

## How it works

The 3.3.5a client can draw a model's texture as a *projected texture* (skin batch flag `0x4`): instead of drawing the
triangles it paints the texture down onto whatever is below, terrain and buildings alike. A projected batch paints the
whole box of each triangle, so each mark is one quad per layer (area, rim, fill) with the shape drawn in the texture's
alpha, and every texture keeps a clear margin (the client's texture clamp would otherwise smear its edge outward).

Every mark is a gameobject with one of these generated models (`tools/telegraph_assets.py`). A model is 1 yd at scale 1;
the server scales it to the exact size and tilts it to the slope, and picks the model variant with the right proportion
(line length / width, cone angle, donut inner / outer) and fill duration. Hit tests are exact geometry on the server.

## Requirements

- AzerothCore (wotlk master; compile-checked against master of 2026-11-23, also runs on the Playerbots fork).
- WoW 3.3.5a (12340) clients with the module's client patch (one MPQ).
- Only to rebuild the client files yourself: Python 3 with Pillow (`pip install pillow`) and an MPQ tool.

## Install

**Server**

```bash
cd azerothcore/modules
git clone https://github.com/RockNetEngine/mod-ground-telegraphs.git
cd ../build && cmake .. && make -j$(nproc) worldserver && make install
```

The SQL in `data/sql/db-world/` is applied by the DB updater at the next start (tables, the mark variants and their
`gameobjectdisplayinfo_dbc` rows, and the visual caster creature). No server DBC file needs to change.

**Client**

Download the MPQ from the latest [release](https://github.com/RockNetEngine/mod-ground-telegraphs/releases) and put it in
the client's `Data\` folder as a patch, for example `Data\patch-G.MPQ`. Delete the client's `Cache` folder.

The patch carries `DBFilesClient\GameObjectDisplayInfo.dbc` (stock rows + 252 new ones) and `Spells\Telegraph\*`. The
client loads **one whole** `GameObjectDisplayInfo.dbc`, from the last patch that has it. If your server already ships a
changed `GameObjectDisplayInfo.dbc`, build the patch yourself with your file as the base (below) and ship that.

## Rebuild the client files (optional)

```bash
cd tools
python3 telegraph_assets.py --base path/to/your/GameObjectDisplayInfo.dbc --out out
```

This writes `out/Spells/Telegraph/*`, `out/DBFilesClient/GameObjectDisplayInfo.dbc` and
`out/ground_telegraph_variants.sql`. Pack the two folders into an MPQ (for example with Ladik's MPQ Editor: add
`Spells` and `DBFilesClient` with their paths) and copy the SQL over `data/sql/db-world/ground_telegraph_variants.sql`.
The shape grid (cone angles, donut ratios, line proportions, fill times, colours) is at the top of the script. Same
input, same bytes.

IDs used (change with `--display-base`, `--go-base`): GameObjectDisplayInfo 9700–9951 (block 9700–9999),
gameobject_template 9530000–9530251 (block 9530000–9530899), creature 9530999 (visual caster), 9530998 (example boss).

## Preview in game

GM command, drawn at your feet facing where you face:

```
.telegraph line  [length=40] [width=2]  [ms=3000] [friendly 0/1]
.telegraph cone  [length=40] [angle=60] [ms] [friendly]
.telegraph nova  [radius=10]            [ms] [friendly]
.telegraph donut [outer=40]  [inner=20] [ms] [friendly]
.telegraph reload
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
m.durationMs = 3000;                                   // the fill reaches the edge then
ObjectGuid mark = GroundTelegraph::Draw(boss, m);      // despawns by itself ~2 s after durationMs

// when it lands:
if (GroundTelegraph::Contains(m, player->GetPositionX(), player->GetPositionY()))
    /* hit the player */;
GroundTelegraph::Remove(boss, mark);
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
| `advisor_entry` | a nearby creature that says each attack's `advice` |
| `visual_caster` | creature that plays `visual_spell` (0 = 9530999, an invisible caster) |
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

Other columns: `delay_ms` (mark on the ground until the hit), `every_ms` (0 = only as another attack's `next_attack`),
`first_ms`, `next_attack` (the attack that follows at once: chain two dance rows for "safe ring, then the inverse"),
`hit_spell` + `hit_damage` (exact direct damage of that spell's school, never misses; without `hit_damage` the spell is
cast on each player hit: a stun, a knockback, a heal), `friendly` (green mark, hits friendly players: heals),
`visual_spell` / `visual_scale` (played when it lands: a beam to the far end of a line, a breath / nova on the boss, bursts
on rings and erupts; a scaled visual caster makes a breath or nova visual bigger), `say` / `text_type` (12 say, 14 yell,
16 emote, 41 boss emote), `interrupt_say`, `advice`. Players in a mark when it lands are hit once per attack (2D test, within
8 yd of the mark's height). `.telegraph reload` re-reads the tables.

## Limits

- Very steep slopes stretch a mark (the texture is projected along the mark's axis; the server tilts it to the average
  slope of its area).
- Proportions and fill times come from the generated grid; the server uses the nearest one. A line keeps its length
  exact; add a proportion to the grid if you need an exact width.
- Marks are gameobjects: players far away (outside visibility range) do not see them.

## Credits

Built for and play-tested on the RockNetEngine solo / Playerbots server. The projected-texture flag came from 3.3.5
modding forum knowledge (projected ground spell art such as Charred Earth). Code, models and textures are original.

## License

MIT, see [LICENSE](LICENSE).
