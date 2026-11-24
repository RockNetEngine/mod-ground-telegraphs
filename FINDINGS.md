# Ground telegraphs and decals on the WoW 3.3.5a client: what we tested

Notes from building mod-ground-telegraphs (AzerothCore, WotLK 3.3.5a build 12340, November 2026). Everything below was
tested in game unless it says otherwise. We hope it saves the next person some evenings.

Short version: **the 3.3.5a client can paint textures on the ground like retail telegraphs.** Mark a model's skin batch as
*projected texture* (flag `0x4`), make it one quad per layer, put the shape in the texture's alpha and leave a clear margin
around it. The texture then lies on terrain and building (WMO) floors, the same way the spell targeting circle does.

## 1. What did not work

**Stock spell art as telegraphs.** Breath, nova and beam visuals are fixed-size art, not drawn to a radius:

- A visual played on its caster (nova, breath, eruption) grows with the caster's object scale. An invisible caster with
  `SetObjectScale` can size it roughly, but never exactly.
- A beam does not grow with scale; scale only moves the point it starts from.
- At scale 1 every nova we tried was about twice the size of a 5 yd ring, and breaths were 10–20 yd long.
- The Invisible Stalker (15214) randomly uses display 1126 (a small visible infernal) or 11686. A copy that clients should
  see casting needs display 11686 only and no trigger flag (`flags_extra` 128).

**Rows of small flat sigils** (a stock ground rune gameobject, scaled): they work on flat ground, but every bump hides part
of them.

**One flat model** (an M2 with the shape as triangles, unit-sized and scaled to the exact size): it looks right on flat
ground. On slopes, small bumps and terrain/building transitions it floats or sinks. Tilting it to the average slope
helps on an even slope, but nothing helps on uneven ground.

## 2. What works: projected textures

The M2 skin format has per-batch flags (`M2Batch.flags`, the first byte of each 24-byte texture unit in the `.skin`).
Tested on the 3.3.5a client:

| batch flag | result |
|---|---|
| `0x4` | **projected**: the texture is painted down onto the terrain and WMO floors / walls below the model |
| `0x14` | same as `0x4` |
| `0x20` | nothing visible (the wowdev wiki lists it as "projected texture?") |

How a projected batch is drawn (as observed):

1. **Each triangle paints its whole bounding box.** A disc made of 144 triangles painted a stepped square. So the
   geometry only sets *where* the texture goes; the **shape must be in the texture's alpha**. Use one quad per layer
   covering the shape's box, with UVs 0..1 across the box (u along the model's +X, v along +Y).
2. **The texture is clamped, and the clamp is visible.** Wherever the shape touches the texture's border, the border
   texels are repeated outward past the quad. A disc that touches the edge gave a cross-hair of four streaks, and a full
   rectangle spilled bands across the ground. Fix: a clear margin (we use 6 % of the box on each side) and the outermost
   2 texels forced fully transparent (the margin keeps the smaller mip levels clear too).
3. **Bone animation still works.** A quad on a bone with a scale track grows smoothly, so a fill that grows from the
   origin to the edge works (nova, cone, line). A donut can't grow that way (scaling a ring scales its safe middle too),
   so we fade its fill in with an animated texture weight (transparency) track instead.
4. **It follows the ground well:** terrain, building floors, uneven grass, tilted ground. Very steep slopes stretch
   it, because the texture is projected along the model's own axis. Tilting the gameobject to the average slope of the
   area reduces the stretch.

Working material setup: material flags `0x17` (unlit, unfogged, two-sided, no depth write), blend mode 2 (alpha). Three
layers drawn in order with `priorityPlane`: area 0, fill 1, rim 2. Textures are white with the look in the alpha, and the
colour comes from the model's colour track, so hostile (orange-red) and friendly (green) models share textures. DXT5 BLP
(smooth alpha), 256 px (512 x 64 for long lines).

## 3. Writing 3.3.5a models by hand (MD20 v264)

We generated every model from scratch in Python (`tools/telegraph_assets.py`). Things that mattered:

- The header is `0x130` bytes; every animated track holds one sub-array per sequence.
- One "Stand" sequence (id 0) with flag `0x20` ("stored in the model"). Without it the client looks for an external
  `.anim` file.
- **The sequence lookup is a hash table and must keep an empty slot (`0xFFFF`).** A one-entry table counts as "full"
  and hangs the client; use 3 slots for 1 sequence (`[0, 0xFFFF, 0xFFFF]`).
- A bone with an animated scale track has flag `0x200`.
- Check every model before packing. The model checker of
  [wow-mod-retail-creatures](https://github.com/buildthehomelab/wow-mod-retail-creatures) (`tools/check_m2.py`) caught the
  full lookup table above.
- Model sizes: build the model at 1 yd and scale the gameobject to the size you need (`SetObjectScale`). Scaling is
  uniform, so a rectangle or wedge needs one model per proportion (length / width, angle); a circle needs only one.
- BLP: Pillow's own BLP writer makes palette BLPs without mip maps, and the client showed such textures (world map tiles)
  as solid green for us. Write DXT1 / DXT5 with a full mip chain (`tools/blp.py`, pure Python).

## 4. Server side (AzerothCore)

- **Set a gameobject's scale before it enters the map.** `WorldObject::SummonGameObject` adds the object to the map
  first; a `SetObjectScale` afterwards did not always reach the client (a mark stayed 1 yd). Create the `GameObject`
  yourself, set the scale, then `Map::AddToMap` (`src/ground_telegraph.cpp`).
- Rotation: `GameObject::Create` takes a full quaternion, so a mark can be tilted (yaw first, then the tilt to the
  ground normal).
- Ground height for the tilt: sample from above the highest point the shape can reach. Sampling from just above the
  origin misses uphill ground.
- New gameobject displays need rows in the client's `GameObjectDisplayInfo.dbc`. On the server, the
  `gameobjectdisplayinfo_dbc` table is enough; no server DBC file has to change.
- Hit tests are plain 2D geometry (distance, angle, along / across a line) at the moment the mark ends, plus a height
  window. They don't depend on the art.

## 5. Reproduce it

`.telegraph nova 10` (GM) draws a 10 yd nova at your feet once the module and its client patch are installed. The
generator, the model writer and the BLP writer are in `tools/` (MIT).
