/*
 * mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
 *
 * npc_ground_telegraph_boss: a data-driven boss whose attacks are telegraphed on the ground.
 *   ground_telegraph_boss    one row per creature entry (optional): melee or not, chase distance, yield, death summon ...
 *   ground_telegraph_attack  the attacks: shape, size, timing, what a hit does, visual, text.
 * Shapes: 1 line, 2 cone (from the boss's edge toward its target), 3 nova, 4 donut (around the boss), 5 erupt (small
 * novas on and around the target), 6 dance (rings around the boss: bands "0-10,20-40"), 7 cast (a real, interruptible
 * cast of hit_spell at the target, no mark). Players standing in a mark when it ends are hit once each.
 * Timing: the mark fills, stays full for GroundTelegraph::FULL_HOLD_MS, is removed, and CLEAR_BEFORE_HIT_MS later the hit
 * and the visual land.
 * Visual (visual_kit): a SpellVisualKit played once on invisible casters (visual_caster, default 9530999) placed when the
 * mark goes down, so the client knows them before the kit packet. visual_mode 0 = by shape (cone wave, nova single,
 * erupt single per mark, donut / dance ring), 1 wave (rows sweeping out over `visual_pace` ms), 2 scatter (points over the
 * area at random moments within the pace), 3 single (the centre of each mark; nova: visual_scale is for a 10 yd radius),
 * 4 ring (points on the ring), 5 spokes (default for donut / dance rings: radial lines of bursts across the ring, a few
 * seed spokes first, then a wave both ways to the next seed, all within the pace); nova_kit = the discs of a dance. beam_spell = a beam along a line, or a missile
 * from the boss onto each erupt mark, landing on the hit. Bosses send no attack text (it spammed long fights).
 */
#include "ground_telegraph.h"

#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    enum AttackShape : uint8
    {
        ATK_LINE  = 1,
        ATK_CONE  = 2,
        ATK_NOVA  = 3,
        ATK_DONUT = 4,
        ATK_ERUPT = 5,
        ATK_DANCE = 6,
        ATK_CAST  = 7
    };

    enum VisualMode : uint8
    {
        VIS_BY_SHAPE = 0,
        VIS_WAVE     = 1,
        VIS_SCATTER  = 2,
        VIS_SINGLE   = 3,
        VIS_RING     = 4,
        VIS_SPOKES   = 5
    };

    constexpr uint32 DEFAULT_VISUAL_CASTER = 9530999;   // creature from the module SQL (invisible display 11686)
    constexpr uint32 MAX_VISUAL_BURSTS = 150;           // casters per attack; a wave widens its spacing to stay below
    constexpr uint32 KIT_CASTER_TAIL_MS = 5000;         // a caster stays this long after its kit (the effect plays out)

    struct Attack
    {
        uint32 boss = 0, delayMs = 3000, everyMs = 12000, firstMs = 5000, hitSpell = 0, visualKit = 0, visualPace = 0, beamSpell = 0, novaKit = 0;
        uint8 id = 0, shape = ATK_NOVA, count = 0, next = 0, visualMode = VIS_BY_SHAPE;
        float size = 0.0f, param = 0.0f, spread = 0.0f, visualScale = 0.0f, visualSpacing = 0.0f, visualZoff = 0.0f, novaScale = 0.0f, novaZoff = 0.0f;
        int32 hitDamage = 0;
        bool friendly = false;
        std::vector<std::pair<float, float>> bands;
    };

    struct Boss
    {
        uint32 entry = 0, yieldPct = 0, visualCaster = 0, deathSummon = 0, deathDespawnS = 900, walker = 0;
        bool melee = false;
        float chaseDist = 0.0f, dx = 0, dy = 0, dz = 0, dorient = 0, wx = 0, wy = 0, wz = 0;
        std::string yieldSay, walkerSay;
    };

    std::unordered_map<uint32, std::vector<Attack>> sAttacks;
    std::unordered_map<uint32, Boss> sBosses;
    bool sLoaded = false;

    std::vector<std::pair<float, float>> ParseBands(std::string const& s)
    {
        std::vector<std::pair<float, float>> out;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t end = s.find(',', pos);
            std::string part = s.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            size_t dash = part.find('-');
            if (dash != std::string::npos)
            {
                float lo = std::strtof(part.substr(0, dash).c_str(), nullptr);
                float hi = std::strtof(part.substr(dash + 1).c_str(), nullptr);
                if (hi > lo && lo >= 0.0f)
                    out.emplace_back(lo, hi);
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }
        return out;
    }

    void Load()
    {
        sAttacks.clear();
        sBosses.clear();
        sLoaded = true;
        if (QueryResult r = WorldDatabase.Query("SELECT entry, melee, chase_dist, yield_pct, yield_say, visual_caster, death_summon, "
            "death_x, death_y, death_z, death_o, death_despawn_s, walker_entry, walker_x, walker_y, walker_z, walker_say FROM ground_telegraph_boss"))
        {
            do
            {
                Field* f = r->Fetch();
                Boss b;
                b.entry = f[0].Get<uint32>();
                b.melee = f[1].Get<uint8>() != 0;
                b.chaseDist = f[2].Get<float>();
                b.yieldPct = f[3].Get<uint8>();
                b.yieldSay = f[4].Get<std::string>();
                b.visualCaster = f[5].Get<uint32>();
                b.deathSummon = f[6].Get<uint32>();
                b.dx = f[7].Get<float>();
                b.dy = f[8].Get<float>();
                b.dz = f[9].Get<float>();
                b.dorient = f[10].Get<float>();
                b.deathDespawnS = f[11].Get<uint32>();
                b.walker = f[12].Get<uint32>();
                b.wx = f[13].Get<float>();
                b.wy = f[14].Get<float>();
                b.wz = f[15].Get<float>();
                b.walkerSay = f[16].Get<std::string>();
                sBosses[b.entry] = b;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, attack_id, shape, size, param, `count`, spread, bands, delay_ms, every_ms, first_ms, "
            "next_attack, hit_spell, hit_damage, friendly, visual_kit, visual_mode, visual_scale, visual_spacing, visual_pace, visual_zoff, beam_spell, nova_kit, nova_scale, nova_zoff "
            "FROM ground_telegraph_attack ORDER BY boss_entry, attack_id"))
        {
            do
            {
                Field* f = r->Fetch();
                Attack a;
                a.boss = f[0].Get<uint32>();
                a.id = f[1].Get<uint8>();
                a.shape = f[2].Get<uint8>();
                a.size = f[3].Get<float>();
                a.param = f[4].Get<float>();
                a.count = f[5].Get<uint8>();
                a.spread = f[6].Get<float>();
                a.bands = ParseBands(f[7].Get<std::string>());
                a.delayMs = f[8].Get<uint32>();
                a.everyMs = f[9].Get<uint32>();
                a.firstMs = f[10].Get<uint32>();
                a.next = f[11].Get<uint8>();
                a.hitSpell = f[12].Get<uint32>();
                a.hitDamage = f[13].Get<int32>();
                a.friendly = f[14].Get<uint8>() != 0;
                a.visualKit = f[15].Get<uint32>();
                a.visualMode = f[16].Get<uint8>();
                a.visualScale = f[17].Get<float>();
                a.visualSpacing = f[18].Get<float>();
                a.visualPace = f[19].Get<uint32>();
                a.visualZoff = f[20].Get<float>();
                a.beamSpell = f[21].Get<uint32>();
                a.novaKit = f[22].Get<uint32>();
                a.novaScale = f[23].Get<float>();
                a.novaZoff = f[24].Get<float>();
                sAttacks[a.boss].push_back(a);
            } while (r->NextRow());
        }
        LOG_INFO("module", "mod-ground-telegraphs: {} boss row(s), {} boss(es) with attacks.", sBosses.size(), sAttacks.size());
    }

    std::vector<Attack> const* Attacks(uint32 entry)
    {
        if (!sLoaded)
            Load();
        auto it = sAttacks.find(entry);
        return it == sAttacks.end() ? nullptr : &it->second;
    }

    Boss const* BossRow(uint32 entry)
    {
        if (!sLoaded)
            Load();
        auto it = sBosses.find(entry);
        return it == sBosses.end() ? nullptr : &it->second;
    }
}

void GroundTelegraphLoadBosses()
{
    Load();
}

class npc_ground_telegraph_boss : public CreatureScript
{
public:
    npc_ground_telegraph_boss() : CreatureScript("npc_ground_telegraph_boss") { }

    struct ai : public ScriptedAI
    {
        ai(Creature* c) : ScriptedAI(c) { }

        static constexpr int32 FOLLOW_ONLY = 0x7FFFFFFF;   // every_ms 0: only runs as another row's next_attack

        std::vector<int32> _timers;
        std::vector<GroundTelegraph::Mark> _marks;          // the marks of the pending attack (the hit test)
        std::vector<ObjectGuid> _guids;
        std::vector<ObjectGuid> _kitCasters;                // invisible casters of the visuals still to play
        int32 _pending = -1;                                // index of the attack on the ground, -1 = none
        int32 _casting = -1;                                // index of a cast attack being cast, -1 = none
        uint32 _eruptIn = 0, _faceIn = 0;
        float _ang = 0.0f;
        bool _yielded = false;

        std::vector<Attack> const* Rows() const { return Attacks(me->GetEntry()); }
        Boss const* Row() const { return BossRow(me->GetEntry()); }
        bool Melee() const { Boss const* b = Row(); return b ? b->melee : true; }
        float ChaseDist() const { Boss const* b = Row(); return b ? b->chaseDist : 0.0f; }

        void Clear()
        {
            for (ObjectGuid const& g : _guids)
                GroundTelegraph::Remove(me, g);
            _guids.clear();
            _marks.clear();
            _pending = -1;
        }

        // visuals not played yet (evade, death, yield): their casters go, so the scheduled kits find nothing
        void CancelVisuals()
        {
            for (ObjectGuid const& g : _kitCasters)
                if (Creature* c = ObjectAccessor::GetCreature(*me, g))
                    c->DespawnOrUnsummon();
            _kitCasters.clear();
        }

        void Reset() override
        {
            Clear();
            CancelVisuals();
            _casting = -1;
            _yielded = false;
            _timers.clear();
            if (std::vector<Attack> const* rows = Rows())
                for (Attack const& a : *rows)
                    _timers.push_back(a.everyMs ? int32(a.firstMs) : FOLLOW_ONLY);
            me->SetControlled(false, UNIT_STATE_ROOT);
        }

        void AttackStart(Unit* who) override
        {
            if (!who)
                return;
            // a summon told to attack has no threat yet; without it UpdateVictim() finds no victim and it evades
            if (!me->IsInCombatWith(who))
                me->EngageWithTarget(who);
            if (!me->Attack(who, Melee()))
                return;
            float chase = ChaseDist();
            if (Melee() || chase > 0.0f)
                me->GetMotionMaster()->MoveChase(who, chase);
            else
            {
                me->GetMotionMaster()->Clear();
                me->StopMoving();
                me->SetControlled(true, UNIT_STATE_ROOT);   // a caster that never moves
            }
        }

        void EnterEvadeMode(EvadeReason why) override
        {
            Clear();
            CancelVisuals();
            _casting = -1;
            me->InterruptNonMeleeSpells(false);
            me->SetControlled(false, UNIT_STATE_ROOT);
            ScriptedAI::EnterEvadeMode(why);
        }

        void JustDied(Unit* /*killer*/) override
        {
            Clear();
            CancelVisuals();
            Boss const* b = Row();
            if (!b)
                return;
            if (b->deathSummon && !me->FindNearestCreature(b->deathSummon, 80.0f, true))
                me->SummonCreature(b->deathSummon, b->dx, b->dy, b->dz, b->dorient, TEMPSUMMON_TIMED_OR_DEAD_DESPAWN, b->deathDespawnS * 1000);
            if (b->walker)
                if (Creature* w = me->FindNearestCreature(b->walker, 80.0f, true))
                {
                    if (!b->walkerSay.empty())
                        w->Say(b->walkerSay, LANG_UNIVERSAL);
                    w->CombatStop(true);
                    w->GetMotionMaster()->Clear();
                    w->GetMotionMaster()->MovePoint(1, b->wx, b->wy, b->wz);
                    w->DespawnOrUnsummon(9000ms);
                }
        }

        // yield_pct: at that health the fight is over. Players within 40 yd get the kill credit of the entry, the boss says
        // yield_say, turns friendly and leaves after 5 s (back after 30 s). For sparring / training bosses.
        void DamageTaken(Unit* /*attacker*/, uint32& damage, DamageEffectType /*type*/, SpellSchoolMask /*school*/) override
        {
            Boss const* b = Row();
            if (!b || !b->yieldPct)
                return;
            if (_yielded)
            {
                damage = 0;
                return;
            }
            uint32 floor = me->CountPctFromMaxHealth(int32(b->yieldPct));
            if (me->GetHealth() <= damage || me->GetHealth() - damage <= floor)
            {
                damage = me->GetHealth() > floor ? me->GetHealth() - floor : 0;
                Yield(*b);
            }
        }

        void Yield(Boss const& b)
        {
            _yielded = true;
            Clear();
            CancelVisuals();
            _casting = -1;
            me->InterruptNonMeleeSpells(false);
            for (auto const& ref : me->GetMap()->GetPlayers())
                if (Player* p = ref.GetSource())
                    if (p->IsAlive() && p->GetDistance(me) <= 40.0f)
                        p->KilledMonsterCredit(me->GetEntry());
            if (!b.yieldSay.empty())
                me->Say(b.yieldSay, LANG_UNIVERSAL);
            me->CombatStop(true);
            me->SetFaction(35);
            me->SetImmuneToPC(true);
            me->SetReactState(REACT_PASSIVE);
            me->SetControlled(false, UNIT_STATE_ROOT);
            me->DespawnOrUnsummon(5000ms, 30s);
        }

        void Draw(GroundTelegraph::Mark const& m)
        {
            _marks.push_back(m);
            if (ObjectGuid g = GroundTelegraph::Draw(me, m))
                _guids.push_back(g);
        }

        void Start(uint32 i)
        {
            std::vector<Attack> const* rows = Rows();
            if (!rows || i >= rows->size())
                return;
            Attack const& a = (*rows)[i];
            Unit* target = me->GetVictim();
            if (!target || !target->IsPlayer())
                if (Player* p = me->SelectNearestPlayer(60.0f))
                    target = p;
            if (!target)
                return;
            me->SetFacingToObject(target);
            if (a.shape == ATK_CAST)
            {
                // a real cast with its cast bar; power cost ignored. Interrupts stop it (unless the spell can't be).
                SpellCastResult res = a.hitDamage > 0
                    ? me->CastCustomSpell(a.hitSpell, SPELLVALUE_BASE_POINT0, a.hitDamage, target, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST)
                    : me->CastSpell(target, a.hitSpell, TRIGGERED_IGNORE_POWER_AND_REAGENT_COST);
                if (res == SPELL_CAST_OK && me->HasUnitState(UNIT_STATE_CASTING))
                    _casting = int32(i);
                else
                    LOG_DEBUG("module", "mod-ground-telegraphs: boss {} could not start cast {} (result {}).", me->GetEntry(), a.hitSpell, uint32(res));
                return;
            }

            Clear();
            _ang = me->GetAngle(target);
            float reach = me->GetCombatReach();
            GroundTelegraph::Mark m;
            m.friendly = a.friendly;
            m.durationMs = a.delayMs;
            m.x = me->GetPositionX();
            m.y = me->GetPositionY();
            m.z = me->GetPositionZ();
            m.facing = _ang;
            switch (a.shape)
            {
                case ATK_LINE:
                case ATK_CONE:
                    m.shape = a.shape == ATK_LINE ? GroundTelegraph::SHAPE_LINE : GroundTelegraph::SHAPE_CONE;
                    m.x += reach * std::cos(_ang);   // from the boss's edge
                    m.y += reach * std::sin(_ang);
                    m.size = a.size > 0.0f ? a.size : 40.0f;
                    m.param = a.param > 0.0f ? a.param : (a.shape == ATK_LINE ? 2.0f : 60.0f);
                    Draw(m);
                    break;
                case ATK_NOVA:
                    m.shape = GroundTelegraph::SHAPE_NOVA;
                    m.size = a.size > 0.0f ? a.size : 10.0f;
                    Draw(m);
                    break;
                case ATK_DONUT:
                    m.shape = GroundTelegraph::SHAPE_DONUT;
                    m.size = a.size > 0.0f ? a.size : 20.0f;
                    m.param = a.param > 0.0f ? a.param : m.size / 2.0f;
                    Draw(m);
                    break;
                case ATK_DANCE:
                    for (auto const& b : a.bands)
                    {
                        m.shape = b.first < 0.5f ? GroundTelegraph::SHAPE_NOVA : GroundTelegraph::SHAPE_DONUT;
                        m.size = b.second;
                        m.param = b.first;
                        Draw(m);
                    }
                    break;
                case ATK_ERUPT:
                {
                    uint32 n = a.count ? a.count : 3;
                    float spread = a.spread > 0.0f ? a.spread : 8.0f;
                    m.shape = GroundTelegraph::SHAPE_NOVA;
                    m.size = a.size > 0.0f ? a.size : 3.0f;
                    for (uint32 k = 0; k < n; ++k)
                    {
                        float t = frand(0.0f, 2.0f * float(M_PI));
                        float d = k ? spread * std::sqrt(frand(0.05f, 1.0f)) : 0.0f;   // the first one on the target
                        m.x = target->GetPositionX() + d * std::cos(t);
                        m.y = target->GetPositionY() + d * std::sin(t);
                        m.z = target->GetPositionZ();
                        Draw(m);
                    }
                    break;
                }
                default:
                    return;
            }
            _pending = int32(i);
            // the fill of the model actually drawn (the nearest duration in the grid), then a short beat while it is full
            _eruptIn = _marks.empty() ? a.delayMs : GroundTelegraph::HitMs(_marks.front());
            if (!_marks.empty())
            {
                // the marks go CLEAR_BEFORE_HIT_MS before the hit: removed in the same update, they still showed under
                // the first bursts. Erupt's Clear() then finds them gone.
                std::vector<ObjectGuid> marks = _guids;
                Creature* self = me;
                me->m_Events.AddEventAtOffset([self, marks]()
                {
                    for (ObjectGuid const& g : marks)
                        GroundTelegraph::Remove(self, g);
                }, Milliseconds(GroundTelegraph::RemoveMs(_marks.front())));
            }
            PlanVisual(a, _eruptIn);
        }

        bool Inside(Player* p) const
        {
            for (GroundTelegraph::Mark const& m : _marks)
                if (std::fabs(p->GetPositionZ() - m.z) < 8.0f && GroundTelegraph::Contains(m, p->GetPositionX(), p->GetPositionY()))
                    return true;
            return false;
        }

        void Hit(Attack const& a, Player* p)
        {
            if (!a.hitSpell)
                return;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(a.hitSpell);
            if (!si)
                return;
            if (!a.friendly && a.hitDamage > 0)
            {
                // direct spell damage of hit_spell's school: standing in it always lands (no miss roll, range or sight check)
                SpellNonMeleeDamage d(me, p, si, si->GetSchoolMask());
                me->CalculateSpellDamageTaken(&d, a.hitDamage, si);
                Unit::DealDamageMods(p, d.damage, &d.absorb);
                me->SendSpellNonMeleeDamageLog(&d);
                me->DealSpellDamage(&d, true);
                return;
            }
            // anything else the spell does (a heal for a friendly mark, a stun, a knockback ...), cast triggered on the player
            me->CastSpell(p, a.hitSpell, true);
        }

        struct Burst
        {
            float x, y, o, scale;
            uint32 at;    // ms after the hit
            uint32 kit;
            float zoff;
        };

        // the points of one mark for a visual mode, at spacing sp (yd); at = ms after the hit, within `pace`
        void Points(Attack const& a, GroundTelegraph::Mark const& m, uint8 mode, float sp, uint32 pace, std::vector<Burst>& out) const
        {
            float sc = a.visualScale > 0.0f ? a.visualScale : 1.0f;
            bool wedge = m.shape == GroundTelegraph::SHAPE_CONE;
            float half = wedge ? m.param * float(M_PI) / 360.0f : float(M_PI);
            float inner = m.shape == GroundTelegraph::SHAPE_DONUT ? m.param : 0.0f;
            float outer = std::max(inner + 0.5f, m.size);
            auto add = [&](float d, float t, uint32 at, float scale)
            {
                float o = (wedge ? m.facing : 0.0f) + t;
                out.push_back({ m.x + d * std::cos(o), m.y + d * std::sin(o), o, scale, at, a.visualKit, a.visualZoff });
            };
            switch (mode)
            {
                case VIS_WAVE:   // rows (cone) or rings (disc, donut) sweeping out from the origin
                    for (float d = std::max(inner, sp * 0.5f); d <= outer + 0.01f; d += sp)
                    {
                        uint32 n = std::max<uint32>(1, uint32(std::lround(2.0f * half * d / sp)) + (wedge ? 1 : 0));
                        uint32 at = uint32(float(pace) * (d - inner) / std::max(0.5f, outer - inner));
                        for (uint32 k = 0; k < n; ++k)
                            add(d, wedge ? (n == 1 ? 0.0f : 0.9f * (-half + 2.0f * half * float(k) / float(n - 1)))
                                : 2.0f * float(M_PI) * float(k) / float(n), at, sc);
                    }
                    break;
                case VIS_SCATTER:   // points over the area, each at a random moment within the pace
                {
                    float area = half * (outer * outer - inner * inner);
                    uint32 n = std::max<uint32>(4, std::min<uint32>(90, uint32(area / (sp * sp))));
                    for (uint32 k = 0; k < n; ++k)
                        add(std::sqrt(frand(inner * inner, outer * outer)), wedge ? 0.9f * frand(-half, half) : frand(0.0f, 2.0f * float(M_PI)),
                            urand(0, pace), sc);
                    break;
                }
                case VIS_SPOKES:
                {
                    // spokes (user design 2026-11-27): M radial lines across the ring, `sp` apart on its middle circle, each
                    // with k bursts (ring width / sp, 1-5) going off together. S seed spokes (one per ~25 yd of circle, >= 3)
                    // start first; each step lights the next spoke on both sides of every seed until the waves meet. M is a
                    // multiple of S, so every segment is the same: deterministic count and timing.
                    float mid = (outer + inner) / 2.0f, circ = 2.0f * float(M_PI) * mid;
                    uint32 seeds = std::max<uint32>(3, uint32(std::lround(circ / 25.0f)));
                    uint32 per = std::max<uint32>(1, uint32(std::lround(circ / sp / float(seeds))));
                    uint32 spokes = per * seeds;
                    uint32 k = std::max<uint32>(1, std::min<uint32>(5, uint32(std::lround((outer - inner) / sp))));
                    uint32 steps = per / 2 + 1;
                    uint32 stepMs = steps > 1 ? pace / (steps - 1) : 0;
                    float off = frand(0.0f, 2.0f * float(M_PI));   // where the first seed sits
                    for (uint32 i = 0; i < spokes; ++i)
                    {
                        uint32 q = i % per;
                        uint32 at = std::min(q, per - q) * stepMs;
                        float t = off + 2.0f * float(M_PI) * float(i) / float(spokes);
                        for (uint32 j = 0; j < k; ++j)
                            add(inner + (float(j) + 0.5f) * (outer - inner) / float(k), t, at, sc);
                    }
                    break;
                }
                case VIS_RING:   // points on the middle of the ring (a disc: half its radius)
                {
                    float mid = (outer + inner) / 2.0f;
                    uint32 n = std::max<uint32>(4, uint32(std::lround(2.0f * float(M_PI) * mid / sp)));
                    for (uint32 k = 0; k < n; ++k)
                        add(mid, 2.0f * float(M_PI) * float(k) / float(n), urand(0, pace), sc);
                    break;
                }
                default:   // VIS_SINGLE: the centre; a nova attack scales it with its radius (visual_scale = at 10 yd)
                    add(0.0f, 0.0f, a.shape == ATK_ERUPT ? urand(0, pace) : 0, a.shape == ATK_NOVA ? sc * m.size / 10.0f : sc);
                    break;
            }
        }

        // an invisible caster on the ground at x, y (+ zoff yd), gone after lifeMs; cancelled with the fight
        Creature* PlaceCaster(float x, float y, float z, float o, uint32 lifeMs, float zoff = 0.0f)
        {
            Boss const* b = Row();
            uint32 entry = b && b->visualCaster ? b->visualCaster : DEFAULT_VISUAL_CASTER;
            me->UpdateGroundPositionZ(x, y, z);
            // the caster flies (creature_template_movement Flight 1): with gravity a caster below the ground showed no kit
            Creature* c = me->SummonCreature(entry, x, y, z + zoff, o, TEMPSUMMON_TIMED_DESPAWN, lifeMs);
            if (!c)
                return nullptr;
            c->SetFaction(35);
            c->SetReactState(REACT_PASSIVE);
            _kitCasters.push_back(c->GetGUID());
            return c;
        }

        // The visual of an attack, placed when its marks go down: one invisible caster per burst NOW (the client must know
        // it before the kit packet; the fill is >= 2 s), the kit sent at hit + the burst's moment. A kit plays once (a
        // gameobject with a spell model loops it while it fades); its size = caster scale x the kit effect's own Scale.
        // nova_kit: every disc of a dance plays it once in its centre (nova_scale = at 10 yd); its rings keep visual_kit.
        void PlanVisual(Attack const& a, uint32 hitMs)
        {
            _kitCasters.erase(std::remove_if(_kitCasters.begin(), _kitCasters.end(),
                [this](ObjectGuid const& g) { return !ObjectAccessor::GetCreature(*me, g); }), _kitCasters.end());
            PlanBeam(a, hitMs);
            if ((!a.visualKit && !a.novaKit) || _marks.empty() || a.shape == ATK_LINE)
                return;
            uint32 pace = a.visualPace ? a.visualPace : 800;
            float sp = std::max(1.0f, a.visualSpacing > 0.0f ? a.visualSpacing : 2.0f);
            std::vector<Burst> bursts;
            for (uint32 tries = 0; tries < 4; ++tries)
            {
                bursts.clear();
                for (GroundTelegraph::Mark const& m : _marks)
                {
                    bool disc = m.shape == GroundTelegraph::SHAPE_NOVA && (a.shape == ATK_DANCE || a.shape == ATK_DONUT);
                    if (disc && a.novaKit)
                    {
                        float ns = (a.novaScale > 0.0f ? a.novaScale : 1.0f) * m.size / 10.0f;
                        bursts.push_back({ m.x, m.y, 0.0f, ns, 0, a.novaKit, a.novaZoff });
                        continue;
                    }
                    if (!a.visualKit)
                        continue;
                    uint8 mode = a.visualMode;
                    if (mode == VIS_BY_SHAPE)
                        mode = a.shape == ATK_CONE ? VIS_WAVE : (a.shape == ATK_NOVA || a.shape == ATK_ERUPT) ? VIS_SINGLE : VIS_SPOKES;
                    Points(a, m, mode, sp, pace, bursts);
                }
                if (bursts.size() <= MAX_VISUAL_BURSTS)
                    break;
                sp *= 1.05f * std::sqrt(float(bursts.size()) / float(MAX_VISUAL_BURSTS));   // a big cone: wider spacing, same look
            }
            if (bursts.size() > MAX_VISUAL_BURSTS)
                bursts.resize(MAX_VISUAL_BURSTS);
            for (Burst const& u : bursts)
            {
                Creature* c = PlaceCaster(u.x, u.y, _marks.front().z + 4.0f, u.o, hitMs + u.at + KIT_CASTER_TAIL_MS, u.zoff);
                if (!c)
                    continue;
                c->SetObjectScale(std::max(0.01f, u.scale));
                ObjectGuid g = c->GetGUID();
                uint32 kit = u.kit;
                Creature* self = me;
                me->m_Events.AddEventAtOffset([self, g, kit]()
                {
                    if (Creature* k = ObjectAccessor::GetCreature(*self, g))
                        k->SendPlaySpellVisual(kit);
                }, Milliseconds(hitMs + u.at));
            }
        }

        // beam_spell (a no-damage visual spell): a line = the boss casts it at a caster at the far end when the attack lands
        // (a beam); erupt = the boss casts it at a caster on each mark, launched so that a missile lands on the hit
        // (travel = distance / the spell's Speed). The casters are placed now, so the client knows them in time.
        void PlanBeam(Attack const& a, uint32 hitMs)
        {
            if (!a.beamSpell || _marks.empty() || (a.shape != ATK_LINE && a.shape != ATK_ERUPT))
                return;
            SpellInfo const* si = sSpellMgr->GetSpellInfo(a.beamSpell);
            if (!si)
                return;
            uint32 spell = a.beamSpell;
            float ang = _ang;
            for (GroundTelegraph::Mark const& m : _marks)
            {
                float x = m.x, y = m.y;
                if (a.shape == ATK_LINE)
                {
                    x += m.size * std::cos(m.facing);
                    y += m.size * std::sin(m.facing);
                }
                Creature* t = PlaceCaster(x, y, m.z + 4.0f, 0.0f, hitMs + KIT_CASTER_TAIL_MS, a.shape == ATK_LINE ? 1.0f : 0.0f);
                if (!t)
                    continue;
                uint32 travel = si->Speed > 0.0f ? uint32(1000.0f * me->GetExactDist2d(x, y) / si->Speed) : 0;
                uint32 at = hitMs > travel ? hitMs - travel : 0;
                ObjectGuid g = t->GetGUID();
                Creature* self = me;
                bool line = a.shape == ATK_LINE;
                me->m_Events.AddEventAtOffset([self, g, spell, line, ang]()
                {
                    if (Creature* k = ObjectAccessor::GetCreature(*self, g))
                    {
                        if (line)
                            self->SetFacingTo(ang);
                        self->CastSpell(k, spell, true);
                    }
                }, Milliseconds(at));
            }
        }

        void Erupt()
        {
            std::vector<Attack> const* rows = Rows();
            int32 follow = -1;
            if (rows && _pending >= 0 && uint32(_pending) < rows->size())
            {
                Attack const& a = (*rows)[_pending];
                for (auto const& ref : me->GetMap()->GetPlayers())
                {
                    Player* p = ref.GetSource();
                    if (!p || !p->IsAlive() || p->IsGameMaster() || me->GetExactDist2d(p) > 100.0f)
                        continue;
                    if (a.friendly ? !me->IsFriendlyTo(p) : !me->IsValidAttackTarget(p))
                        continue;
                    if (Inside(p))
                        Hit(a, p);   // one hit per player per attack
                }
                if (a.next)
                    for (uint32 k = 0; k < rows->size(); ++k)
                        if ((*rows)[k].id == a.next)
                            follow = int32(k);
            }
            Clear();   // the marks go at once (no fade), in the same update as the hit and the first bursts
            if (follow >= 0 && me->IsAlive() && me->GetVictim())
                Start(uint32(follow));   // the dance: the next attack goes down at once
        }

        void OnSpellCast(SpellInfo const* spell) override
        {
            std::vector<Attack> const* rows = Rows();
            if (rows && _casting >= 0 && uint32(_casting) < rows->size() && spell && spell->Id == (*rows)[_casting].hitSpell)
                _casting = -1;   // it went off: the spell itself deals its damage
        }

        void OnSpellFailed(SpellInfo const* spell) override
        {
            std::vector<Attack> const* rows = Rows();
            if (rows && _casting >= 0 && uint32(_casting) < rows->size() && spell && spell->Id == (*rows)[_casting].hitSpell)
                _casting = -1;   // stopped (interrupt, stun)
        }

        void UpdateAI(uint32 diff) override
        {
            if (_yielded || !UpdateVictim())
                return;

            if (_faceIn <= diff)
            {
                _faceIn = 500;
                if (_pending < 0 && me->GetVictim() && !Melee() && ChaseDist() <= 0.0f)
                    me->SetFacingToObject(me->GetVictim());
            }
            else
                _faceIn -= diff;

            for (int32& t : _timers)
                if (t != FOLLOW_ONLY)
                    t = std::max<int32>(t - int32(diff), -600000);   // below 0 = overdue by that much

            if (_casting >= 0)
            {
                if (!me->HasUnitState(UNIT_STATE_CASTING))
                    _casting = -1;
                return;
            }

            if (_pending >= 0)
            {
                if (_eruptIn <= diff)
                    Erupt();
                else
                    _eruptIn -= diff;
                if (Melee())
                    DoMeleeAttackIfReady();
                return;
            }

            if (std::vector<Attack> const* rows = Rows())
            {
                // the attack that has waited longest goes first
                int32 best = -1;
                for (uint32 k = 0; k < _timers.size() && k < rows->size(); ++k)
                    if (_timers[k] <= 0 && (best < 0 || _timers[k] < _timers[best]))
                        best = int32(k);
                if (best >= 0)
                {
                    _timers[best] = int32(std::max<uint32>((*rows)[best].everyMs, (*rows)[best].delayMs + 500));
                    Start(uint32(best));
                }
            }
            if (Melee())
                DoMeleeAttackIfReady();
        }
    };

    CreatureAI* GetAI(Creature* c) const override { return new ai(c); }
};

void AddSC_npc_ground_telegraph_boss()
{
    new npc_ground_telegraph_boss();
}
