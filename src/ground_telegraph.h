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
 *   ... 3 s later: if (GroundTelegraph::Contains(m, player->GetPositionX(), player->GetPositionY())) hit it;
 *   GroundTelegraph::Remove(boss, g);
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

    // Spawns the mark; it despawns by itself about 2 s after durationMs (or call Remove). Empty guid when no variant
    // is loaded for that shape / colour.
    ObjectGuid Draw(WorldObject* owner, Mark const& m);
    void Remove(WorldObject* owner, ObjectGuid const& guid);
    // Exact hit test on the ground plane.
    bool Contains(Mark const& m, float px, float py);
    // Reloads ground_telegraph_variant and the boss tables; returns the number of mark variants.
    uint32 Reload();
    bool Available();
}

#endif
