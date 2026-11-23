/*
 * mod-ground-telegraphs - Copyright (c) 2026 RockNetEngine. MIT License, see LICENSE.
 *
 * npc_ground_telegraph_boss: a data-driven boss whose attacks are telegraphed on the ground.
 *   ground_telegraph_boss    one row per creature entry (optional): melee or not, chase distance, yield, death summon ...
 *   ground_telegraph_attack  the attacks: shape, size, timing, what a hit does, visual, text.
 * Shapes: 1 line, 2 cone (from the boss's edge toward its target), 3 nova, 4 donut (around the boss), 5 erupt (small
 * novas on and around the target), 6 dance (rings around the boss: bands "0-10,20-40"), 7 cast (a real, interruptible
 * cast of hit_spell at the target, no mark). Players standing in a mark when it ends are hit once each.
 */
#include "ground_telegraph.h"

#include "Creature.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"

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

    constexpr uint32 DEFAULT_VISUAL_CASTER = 9530999;   // creature from the module SQL (invisible, casts visual_spell)

    struct Attack
    {
        uint32 boss = 0, delayMs = 3000, everyMs = 12000, firstMs = 5000, hitSpell = 0, visualSpell = 0;
        uint8 id = 0, shape = ATK_NOVA, count = 0, textType = 41, next = 0;
        float size = 0.0f, param = 0.0f, spread = 0.0f, visualScale = 0.0f;
        int32 hitDamage = 0;
        bool friendly = false;
        std::string say, interruptSay, advice;
        std::vector<std::pair<float, float>> bands;
    };

    struct Boss
    {
        uint32 entry = 0, yieldPct = 0, advisor = 0, visualCaster = 0, deathSummon = 0, deathDespawnS = 900, walker = 0;
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
        if (QueryResult r = WorldDatabase.Query("SELECT entry, melee, chase_dist, yield_pct, yield_say, advisor_entry, visual_caster, death_summon, "
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
                b.advisor = f[5].Get<uint32>();
                b.visualCaster = f[6].Get<uint32>();
                b.deathSummon = f[7].Get<uint32>();
                b.dx = f[8].Get<float>();
                b.dy = f[9].Get<float>();
                b.dz = f[10].Get<float>();
                b.dorient = f[11].Get<float>();
                b.deathDespawnS = f[12].Get<uint32>();
                b.walker = f[13].Get<uint32>();
                b.wx = f[14].Get<float>();
                b.wy = f[15].Get<float>();
                b.wz = f[16].Get<float>();
                b.walkerSay = f[17].Get<std::string>();
                sBosses[b.entry] = b;
            } while (r->NextRow());
        }
        if (QueryResult r = WorldDatabase.Query("SELECT boss_entry, attack_id, shape, size, param, `count`, spread, bands, delay_ms, every_ms, first_ms, "
            "next_attack, hit_spell, hit_damage, friendly, visual_spell, visual_scale, say, text_type, interrupt_say, advice "
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
                a.visualSpell = f[15].Get<uint32>();
                a.visualScale = f[16].Get<float>();
                a.say = f[17].Get<std::string>();
                a.textType = f[18].Get<uint8>();
                a.interruptSay = f[19].Get<std::string>();
                a.advice = f[20].Get<std::string>();
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

    void Text(Creature* me, Attack const& a, std::string const& text)
    {
        if (text.empty())
            return;
        switch (a.textType)
        {
            case 12: me->Say(text, LANG_UNIVERSAL); break;
            case 14: me->Yell(text, LANG_UNIVERSAL); break;
            case 16: me->TextEmote(text); break;
            default: me->TextEmote(text, nullptr, true); break;   // 41 = boss emote (centre of the screen)
        }
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

        void Reset() override
        {
            Clear();
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
            _casting = -1;
            me->InterruptNonMeleeSpells(false);
            me->SetControlled(false, UNIT_STATE_ROOT);
            ScriptedAI::EnterEvadeMode(why);
        }

        void JustDied(Unit* /*killer*/) override
        {
            Clear();
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
            Text(me, a, a.say);
            if (!a.advice.empty())   // a friendly advisor nearby calls out what to do
                if (Boss const* b = Row())
                    if (b->advisor)
                        if (Creature* adv = me->FindNearestCreature(b->advisor, 60.0f, true))
                            adv->Say(a.advice, LANG_UNIVERSAL);
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
            _eruptIn = a.delayMs;
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

        Creature* VisualCaster(float x, float y, float z, float o, uint32 ms)
        {
            Boss const* b = Row();
            uint32 entry = b && b->visualCaster ? b->visualCaster : DEFAULT_VISUAL_CASTER;
            Creature* c = me->SummonCreature(entry, x, y, z, o, TEMPSUMMON_TIMED_DESPAWN, ms);
            if (c)
            {
                c->SetFaction(35);
                c->SetReactState(REACT_PASSIVE);
            }
            return c;
        }

        // How an attack looks when it goes off (visual_spell): line = the boss casts it at a caster at the far end (a beam);
        // cone / nova = cast on the boss, or on a caster of visual_scale at the boss (a spell visual grows with its caster);
        // donut / dance = casters around each ring; erupt = a caster on each mark.
        void Visual(Attack const& a)
        {
            if (!a.visualSpell || _marks.empty())
                return;
            if (a.shape == ATK_LINE)
            {
                GroundTelegraph::Mark const& m = _marks.front();
                me->SetFacingTo(_ang);
                if (Creature* c = VisualCaster(m.x + m.size * std::cos(m.facing), m.y + m.size * std::sin(m.facing), m.z + 1.0f, 0.0f, 2500))
                    me->CastSpell(c, a.visualSpell, true);
                return;
            }
            if (a.shape == ATK_CONE || a.shape == ATK_NOVA)
            {
                float o = a.shape == ATK_CONE ? _ang : me->GetOrientation();
                if (a.visualScale > 0.0f)
                {
                    if (Creature* c = VisualCaster(me->GetPositionX(), me->GetPositionY(), me->GetPositionZ(), o, 5000))
                    {
                        c->SetObjectScale(a.visualScale);
                        c->SetFacingTo(o);
                        c->CastSpell(c, a.visualSpell, true);
                    }
                    return;
                }
                if (a.shape == ATK_CONE)
                    me->SetFacingTo(_ang);
                me->CastSpell(me, a.visualSpell, true);
                return;
            }
            std::vector<Position> points;
            for (GroundTelegraph::Mark const& m : _marks)
            {
                if (m.shape == GroundTelegraph::SHAPE_NOVA && a.shape == ATK_ERUPT)
                {
                    points.emplace_back(m.x, m.y, m.z);
                    continue;
                }
                float mid = (m.size + (m.shape == GroundTelegraph::SHAPE_DONUT ? m.param : 0.0f)) / 2.0f;
                uint32 n = std::max<uint32>(6, std::min<uint32>(24, uint32(2.0f * float(M_PI) * mid / 6.0f)));
                for (uint32 k = 0; k < n; ++k)
                {
                    float t = 2.0f * float(M_PI) * float(k) / float(n);
                    points.emplace_back(m.x + mid * std::cos(t), m.y + mid * std::sin(t), m.z);
                }
            }
            for (Position& pt : points)
            {
                float x = pt.GetPositionX(), y = pt.GetPositionY(), z = pt.GetPositionZ() + 4.0f;
                me->UpdateGroundPositionZ(x, y, z);
                if (Creature* c = VisualCaster(x, y, z + 0.2f, 0.0f, 3000))
                {
                    if (a.visualScale > 0.0f)
                        c->SetObjectScale(a.visualScale);
                    c->CastSpell(c, a.visualSpell, true);
                }
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
                Visual(a);
                if (a.next)
                    for (uint32 k = 0; k < rows->size(); ++k)
                        if ((*rows)[k].id == a.next)
                            follow = int32(k);
            }
            Clear();
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
            {
                Text(me, (*rows)[_casting], (*rows)[_casting].interruptSay);   // stopped (interrupt, stun)
                _casting = -1;
            }
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
