/*
 * mod-ground-telegraphs - retail-style ground telegraphs for AzerothCore (3.3.5a)
 * Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
 *
 * A telegraph is one gameobject with a generated model (tools/telegraph_assets.py): a quad whose texture is the shape,
 * drawn by the client as a PROJECTED texture (skin batch flag 0x4), so it lies on terrain and building floors like the
 * spell targeting circle. The model is 1 yd at scale 1; the server scales it to the exact size. Its fill grows from the
 * origin to the border (donut: fades in) over the variant's duration, so it is full when the hit lands.
 *
 * Usage from any script:
 *   GroundTelegraph::Mark m;
 *   m.shape = GroundTelegraph::SHAPE_DONUT; m.x = ...; m.y = ...; m.z = ...;
 *   m.size = 40.0f; m.param = 20.0f;          // outer / inner radius
 *   m.durationMs = 3000;
 *   ObjectGuid g = GroundTelegraph::Draw(boss, m);
 *   ... GroundTelegraph::RemoveMs(m) later (the fill is full): remove it; at HitMs(m) land the attack:
 *   GroundTelegraph::Remove(boss, g);         // gone at once, no fade
 *   if (GroundTelegraph::Contains(m, player->GetPositionX(), player->GetPositionY())) hit it;
 *
 * Data-driven bosses without C++: ScriptName npc_ground_telegraph_boss + rows in ground_telegraph_boss /
 * ground_telegraph_attack (see README.md).
 */
#ifndef MOD_GROUND_TELEGRAPH_H
#define MOD_GROUND_TELEGRAPH_H

#include "ObjectGuid.h"

class WorldObject;

namespace GroundTelegraph
{
    enum Shape : uint8
    {
        SHAPE_LINE  = 1,   // rectangle from the origin along `facing`: length `size`, width `param`
        SHAPE_CONE  = 2,   // wedge from the origin along `facing`: length `size`, full angle `param` degrees
        SHAPE_NOVA  = 3,   // filled disc around the origin: radius `size`
        SHAPE_DONUT = 4    // ring around the origin: outer radius `size`, inner (safe) radius `param`
    };

    struct Mark
    {
        Shape shape = SHAPE_NOVA;
        float x = 0.0f, y = 0.0f, z = 0.0f;   // origin: the centre (nova, donut) or the caster's end (line, cone)
        float facing = 0.0f;                  // line / cone direction (radians)
        float size = 5.0f;
        float param = 0.0f;
        bool friendly = false;                // green instead of orange-red
        uint32 durationMs = 3000;             // time to impact: the fill reaches the border then
    };

    // How long a full mark stays on the ground before it is removed and the attack lands (user 2026-11-27: 250 ms felt
    // like a fade in combat; 0 = the mark goes the moment its fill ends).
    constexpr uint32 FULL_HOLD_MS = 0;
    // The mark is removed this long BEFORE the attack lands (user 2026-11-28: removed in the same update as the hit, the
    // mark still showed under the first bursts on the client).
    constexpr uint32 CLEAR_BEFORE_HIT_MS = 150;

    // Spawns the mark; it despawns by itself about 2 s after its fill (or call Remove). Empty guid when no variant
    // is loaded for that shape / colour.
    ObjectGuid Draw(WorldObject* owner, Mark const& m);
    // Removes the mark at once (no despawn fade: the client keeps drawing a fading object).
    void Remove(WorldObject* owner, ObjectGuid const& guid);
    // The fill time of the model Draw picks for this mark (the nearest duration in the grid), in ms.
    uint32 FillMs(Mark const& m);
    // When to remove the mark, counted from Draw: FillMs + FULL_HOLD_MS.
    uint32 RemoveMs(Mark const& m);
    // When to land the attack (damage and visual), counted from Draw: RemoveMs + CLEAR_BEFORE_HIT_MS.
    uint32 HitMs(Mark const& m);
    // Exact hit test on the ground plane.
    bool Contains(Mark const& m, float px, float py);
    // Reloads ground_telegraph_variant and the boss tables; returns the number of mark variants.
    uint32 Reload();
    bool Available();
}

#endif
