-- mod-ground-telegraphs: an example telegraph boss (NOT applied automatically; run it by hand on your world DB).
-- Creature 9530998 "Telegraph Example Boss" (a copy of the Defias Thug, entry 38), level 80, a caster that never melees
-- and stands rooted. Spawn it with:  .npc add 9530998   (and remove it with .npc delete)
-- Every attack row shows one shape; dance = two rows chained with next_attack (safe ring, then the inverse).

DELETE FROM `creature_template_model` WHERE `CreatureID` = 9530998;
DELETE FROM `creature_template` WHERE `entry` = 9530998;
DROP TEMPORARY TABLE IF EXISTS `tmp_ground_telegraph_ex`;
CREATE TEMPORARY TABLE `tmp_ground_telegraph_ex` LIKE `creature_template`;
INSERT INTO `tmp_ground_telegraph_ex` SELECT * FROM `creature_template` WHERE `entry` = 38;
UPDATE `tmp_ground_telegraph_ex` SET `entry` = 9530998, `name` = 'Telegraph Example Boss', `subname` = 'mod-ground-telegraphs',
  `minlevel` = 80, `maxlevel` = 80, `faction` = 14, `rank` = 1, `HealthModifier` = 20, `AIName` = '', `ScriptName` = 'npc_ground_telegraph_boss';
INSERT INTO `creature_template` SELECT * FROM `tmp_ground_telegraph_ex`;
DROP TEMPORARY TABLE `tmp_ground_telegraph_ex`;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
SELECT 9530998, `Idx`, `CreatureDisplayID`, 1.5, `Probability`, 0 FROM `creature_template_model` WHERE `CreatureID` = 38;

DELETE FROM `ground_telegraph_boss` WHERE `entry` = 9530998;
INSERT INTO `ground_telegraph_boss` (`entry`, `melee`, `chase_dist`, `comment`) VALUES
(9530998, 0, 0, 'example: a rooted caster');

-- hit_spell gives the school / combat log name of hit_damage (133 Fireball, 116 Frostbolt); cast rows really cast it.
DELETE FROM `ground_telegraph_attack` WHERE `boss_entry` = 9530998;
INSERT INTO `ground_telegraph_attack` (`boss_entry`, `attack_id`, `shape`, `size`, `param`, `count`, `spread`, `bands`, `delay_ms`, `every_ms`, `first_ms`,
  `next_attack`, `hit_spell`, `hit_damage`, `friendly`, `visual_spell`, `say`, `comment`) VALUES
(9530998, 1, 3, 10, 0, 0, 0, '', 3000, 30000, 4000, 0, 133, 1500, 0, 0, '%s gathers fire around itself!', 'nova: 10 yd disc'),
(9530998, 2, 1, 40, 2, 0, 0, '', 2500, 30000, 9000, 0, 133, 1500, 0, 0, '%s takes aim at you.', 'line: 2 x 40 yd beam'),
(9530998, 3, 2, 30, 60, 0, 0, '', 3000, 30000, 14000, 0, 133, 1500, 0, 0, '%s draws a deep breath.', 'cone: 60 degrees, 30 yd'),
(9530998, 4, 4, 30, 10, 0, 0, '', 3500, 30000, 19000, 0, 116, 1500, 0, 0, 'Get close to %s!', 'donut: safe within 10 yd'),
(9530998, 5, 6, 0, 0, 0, 0, '0-10,20-35', 4000, 45000, 24000, 6, 133, 1500, 0, 0, '%s pulses in rings!', 'dance part 1'),
(9530998, 6, 6, 0, 0, 0, 0, '10-20', 3000, 0, 0, 0, 133, 1500, 0, 0, 'The rings invert!', 'dance part 2 (every_ms 0 = only after 5)'),
(9530998, 7, 5, 3, 0, 4, 8, '', 3000, 30000, 29000, 0, 133, 1000, 0, 0, 'The ground erupts!', 'erupt: 4 small novas on and around you'),
(9530998, 8, 7, 0, 0, 0, 0, '', 0, 8000, 2000, 0, 116, 0, 0, 0, '', 'cast: a real Frostbolt (interruptible)');
