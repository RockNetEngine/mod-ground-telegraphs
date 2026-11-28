/*
 * mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
 * The mark API (ground_telegraph.h) and the .telegraph GM command.
 */
#include "ground_telegraph.h"

#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <cmath>
#include <vector>

using namespace Acore::ChatCommands;

void GroundTelegraphLoadBosses();   // ground_telegraph_boss.cpp

namespace
{
    struct MarkVariant
    {
        uint32 entry = 0;
        uint8 shape = 0;
        float ratio = 0.0f;
        bool friendly = false;
        uint32 durationMs = 0;
    };

    std::vector<MarkVariant> sVariants;
    bool sLoaded = false;

    void Load()
    {
        sVariants.clear();
        sLoaded = true;
        if (QueryResult r = WorldDatabase.Query("SELECT entry, shape, ratio, friendly, duration_ms FROM ground_telegraph_variant"))
        {
            do
            {
                Field* f = r->Fetch();
                MarkVariant v;
                v.entry = f[0].Get<uint32>();
                v.shape = f[1].Get<uint8>();
                v.ratio = f[2].Get<float>();
                v.friendly = f[3].Get<uint8>() != 0;
                v.durationMs = f[4].Get<uint32>();
                sVariants.push_back(v);
            } while (r->NextRow());
        }
        LOG_INFO("module", "mod-ground-telegraphs: {} mark variants.", sVariants.size());
    }

    // the ratio a shape's model is built for: line length / width, cone angle, donut inner / outer, nova 0
    float WantedRatio(GroundTelegraph::Mark const& m)
    {
        switch (m.shape)
        {
            case GroundTelegraph::SHAPE_LINE:  return m.param > 0.0f ? m.size / m.param : 20.0f;
            case GroundTelegraph::SHAPE_CONE:  return m.param;
            case GroundTelegraph::SHAPE_DONUT: return m.size > 0.0f ? m.param / m.size : 0.5f;
            default:                           return 0.0f;
        }
    }

    // nearest ratio first (log distance, so 15 vs 30 degrees counts like 30 vs 60), then the nearest duration
    MarkVariant const* Pick(GroundTelegraph::Mark const& m)
    {
        if (!sLoaded)
            Load();
        float want = WantedRatio(m);
        MarkVariant const* best = nullptr;
        float bestRatio = 0.0f, bestDur = 0.0f;
        for (MarkVariant const& v : sVariants)
        {
            if (v.shape != m.shape || v.friendly != m.friendly)
                continue;
            float rd = (want > 0.0f && v.ratio > 0.0f) ? std::fabs(std::log(v.ratio / want)) : std::fabs(v.ratio - want);
            float dd = std::fabs(float(v.durationMs) - float(m.durationMs));
            if (!best || rd < bestRatio - 1e-4f || (std::fabs(rd - bestRatio) <= 1e-4f && dd < bestDur))
            {
                best = &v;
                bestRatio = rd;
                bestDur = dd;
            }
        }
        if (best && bestRatio > 0.05f)
            LOG_DEBUG("module", "mod-ground-telegraphs: shape {} wants ratio {:.3f}, nearest model {:.3f} (add it to the generator grid).",
                uint32(m.shape), want, best->ratio);
        return best;
    }

    struct Quat { float x, y, z, w; };

    Quat Mul(Quat const& a, Quat const& b)
    {
        return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                 a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                 a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                 a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }
}

namespace GroundTelegraph
{
    bool Available()
    {
        if (!sLoaded)
            Load();
        return !sVariants.empty();
    }

    uint32 Reload()
    {
        Load();
        GroundTelegraphLoadBosses();
        return uint32(sVariants.size());
    }

    ObjectGuid Draw(WorldObject* owner, Mark const& m)
    {
        if (!owner || !owner->IsInWorld() || m.size <= 0.0f)
            return ObjectGuid::Empty;
        MarkVariant const* v = Pick(m);
        if (!v)
            return ObjectGuid::Empty;

        // scale = the model's 1 yd unit: nova / donut outer radius, cone length; a line keeps its length exact
        // (scale = length / the model's length-to-width ratio), so its width is exact when the ratio is in the grid
        float scale = m.shape == SHAPE_LINE ? m.size / std::max(0.01f, v->ratio) : m.size;

        // ground under the origin, then the slope over the shape's area (a plane through 4 samples around its middle).
        // The texture is projected straight down the model's axis: tilting it to the slope keeps it from stretching.
        float ox = m.x, oy = m.y, oz = m.z + 4.0f;
        owner->UpdateGroundPositionZ(ox, oy, oz);
        float cx = ox, cy = oy;
        if (m.shape == SHAPE_LINE || m.shape == SHAPE_CONE)
        {
            cx += std::cos(m.facing) * m.size * 0.5f;
            cy += std::sin(m.facing) * m.size * 0.5f;
        }
        float d = std::max(1.0f, std::min(12.0f, m.size * 0.4f));
        Map* map = owner->GetMap();
        float top = oz + 10.0f + m.size;   // sample from above the highest point the shape can reach
        auto ground = [&](float x, float y)
        {
            float h = map->GetHeight(owner->GetPhaseMask(), x, y, top, true, 2.0f * (10.0f + m.size));
            return h > INVALID_HEIGHT + 1.0f ? h : oz;
        };
        float hE = ground(cx + d, cy), hW = ground(cx - d, cy), hN = ground(cx, cy + d), hS = ground(cx, cy - d);
        float gx = (hE - hW) / (2.0f * d), gy = (hN - hS) / (2.0f * d);
        Quat q = { 0.0f, 0.0f, std::sin(m.facing / 2.0f), std::cos(m.facing / 2.0f) };   // yaw
        bool tilted = false;
        if (std::fabs(gx) < 1.2f && std::fabs(gy) < 1.2f && (std::fabs(gx) > 0.02f || std::fabs(gy) > 0.02f))   // < ~50 degrees
        {
            float nx = -gx, ny = -gy, nz = 1.0f;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            float ax = -ny, ay = nx;                         // up x n
            float al = std::sqrt(ax * ax + ay * ay);
            float half = std::acos(std::max(-1.0f, std::min(1.0f, nz))) / 2.0f;
            Quat t = { ax / al * std::sin(half), ay / al * std::sin(half), 0.0f, std::cos(half) };
            q = Mul(t, q);                                   // yaw first, then tilt to the ground
            tilted = true;
        }
        float z = oz + 0.12f;
        if (tilted && (m.shape == SHAPE_LINE || m.shape == SHAPE_CONE))
            z = (hE + hW + hN + hS) / 4.0f - gx * (cx - ox) - gy * (cy - oy) + 0.12f;   // the plane's height at the origin

        // Same steps as WorldObject::SummonGameObject (timed despawn), but the scale is set before AddToMap so it is in
        // the create packet: a scale change after the object reached the client did not always show.
        uint32 secs = (m.durationMs + 999) / 1000 + 2;
        GameObject* go = new GameObject();
        if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(), v->entry, map, owner->GetPhaseMask(), ox, oy, z, m.facing,
            G3D::Quat(q.x, q.y, q.z, q.w), 100, GO_STATE_READY))
        {
            delete go;
            return ObjectGuid::Empty;
        }
        go->SetObjectScale(scale);
        go->SetRespawnTime(secs);
        go->SetSpellId(1);
        if (Player* pl = owner->ToPlayer())
            pl->AddGameObject(go);
        else
            go->SetSpawnedByDefault(false);
        map->AddToMap(go, false);
        return go->GetGUID();
    }

    void Remove(WorldObject* owner, ObjectGuid const& guid)
    {
        if (!owner || !guid)
            return;
        if (GameObject* go = ObjectAccessor::GetGameObject(*owner, guid))
        {
            // DespawnOrUnsummon plays the fade-out, and the client keeps the mark on the ground during it (it lingered
            // after the hit): destroy it for the players who see it and take it off the map instead
            go->DestroyForVisiblePlayers();
            go->AddObjectToRemoveList();
        }
    }

    uint32 FillMs(Mark const& m)
    {
        MarkVariant const* v = Pick(m);
        return v ? v->durationMs : m.durationMs;
    }

    uint32 HitMs(Mark const& m)
    {
        return FillMs(m) + FULL_HOLD_MS;
    }

    bool Contains(Mark const& m, float px, float py)
    {
        float dx = px - m.x, dy = py - m.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        switch (m.shape)
        {
            case SHAPE_NOVA:
                return dist <= m.size;
            case SHAPE_DONUT:
                return dist <= m.size && dist >= m.param;
            case SHAPE_LINE:
            {
                float along = dx * std::cos(m.facing) + dy * std::sin(m.facing);
                float side = std::fabs(-dx * std::sin(m.facing) + dy * std::cos(m.facing));
                return along >= 0.0f && along <= m.size && side <= m.param / 2.0f;
            }
            case SHAPE_CONE:
            {
                if (dist > m.size)
                    return false;
                if (dist < 0.01f)
                    return true;
                float a = std::atan2(dy, dx) - m.facing;
                while (a > float(M_PI)) a -= 2.0f * float(M_PI);
                while (a < -float(M_PI)) a += 2.0f * float(M_PI);
                return std::fabs(a) <= m.param * float(M_PI) / 360.0f;
            }
        }
        return false;
    }
}

// .groundmark line|cone|nova|donut [size] [param] [ms] [friendly 0/1]   and   .groundmark reload
// (was .telegraph until 1.1: it clashed with .teleport when typing .tele)
// Draws one mark at your feet facing where you face. Defaults: line 40 x 2, cone 40 yd 60 deg, nova 10, donut 40 / 20; 3000 ms.
class ground_telegraph_commands : public CommandScript
{
public:
    ground_telegraph_commands() : CommandScript("ground_telegraph_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable root = { { "groundmark", HandleTelegraph, SEC_GAMEMASTER, Console::No } };
        return root;
    }

    static bool HandleTelegraph(ChatHandler* handler, std::string what, Optional<float> size, Optional<float> param, Optional<uint32> ms,
        Optional<uint32> friendly)
    {
        Player* p = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!p)
            return true;
        if (what == "reload")
        {
            handler->PSendSysMessage("mod-ground-telegraphs reloaded: {} mark variants (boss tables too).", GroundTelegraph::Reload());
            return true;
        }
        GroundTelegraph::Mark m;
        if (what == "line")       { m.shape = GroundTelegraph::SHAPE_LINE;  m.size = 40.0f; m.param = 2.0f; }
        else if (what == "cone")  { m.shape = GroundTelegraph::SHAPE_CONE;  m.size = 40.0f; m.param = 60.0f; }
        else if (what == "nova")  { m.shape = GroundTelegraph::SHAPE_NOVA;  m.size = 10.0f; }
        else if (what == "donut") { m.shape = GroundTelegraph::SHAPE_DONUT; m.size = 40.0f; m.param = 20.0f; }
        else
        {
            handler->SendSysMessage("Usage: .groundmark line|cone|nova|donut [size] [param] [ms] [friendly 0/1]  or  .groundmark reload");
            return true;
        }
        if (size && *size > 0.0f)
            m.size = std::min(100.0f, *size);
        if (param)
            m.param = *param;
        m.durationMs = ms ? std::max<uint32>(500, std::min<uint32>(20000, *ms)) : 3000;
        m.friendly = friendly && *friendly;
        m.x = p->GetPositionX();
        m.y = p->GetPositionY();
        m.z = p->GetPositionZ();
        m.facing = p->GetOrientation();
        if (!GroundTelegraph::Draw(p, m))
            handler->SendSysMessage("No mark drawn: no ground_telegraph_variant rows (run the module SQL) or no variant for that shape.");
        else
            handler->PSendSysMessage("groundmark {} size {:.1f} param {:.1f}, {} ms{}.", what, m.size, m.param, m.durationMs, m.friendly ? ", friendly" : "");
        return true;
    }
};

void AddSC_ground_telegraph_commands()
{
    new ground_telegraph_commands();
}
