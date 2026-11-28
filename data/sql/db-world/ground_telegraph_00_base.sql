-- mod-ground-telegraphs: tables and the visual caster creature. Self-contained and idempotent (safe to run again).
-- The mark variants (gameobject_template + ground_telegraph_variant + gameobjectdisplayinfo_dbc) are in
-- ground_telegraph_variants.sql, written by tools/telegraph_assets.py together with the client models.

CREATE TABLE IF NOT EXISTS `ground_telegraph_variant` (
  `entry` INT UNSIGNED NOT NULL COMMENT 'gameobject_template entry of the mark',
  `shape` TINYINT UNSIGNED NOT NULL COMMENT '1 line 2 cone 3 nova 4 donut',
  `ratio` FLOAT NOT NULL COMMENT 'line length/width, cone angle deg, donut inner/outer, nova 0',
  `friendly` TINYINT UNSIGNED NOT NULL,
  `duration_ms` INT UNSIGNED NOT NULL COMMENT 'fill time',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-ground-telegraphs: generated, do not edit';

-- One row per boss entry (optional; without a row the boss melees and chases like any creature).
CREATE TABLE IF NOT EXISTS `ground_telegraph_boss` (
  `entry` INT UNSIGNED NOT NULL COMMENT 'creature_template entry (ScriptName npc_ground_telegraph_boss)',
  `melee` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 = never melees (a caster)',
  `chase_dist` FLOAT NOT NULL DEFAULT 0 COMMENT 'caster: 0 = rooted in place, > 0 = keeps this distance',
  `yield_pct` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '> 0: at this health % it gives up (kill credit, turns friendly, leaves)',
  `yield_say` VARCHAR(255) NOT NULL DEFAULT '',
  `visual_caster` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'invisible creature that plays visual_kit; 0 = 9530999',
  `death_summon` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'creature summoned at death_x/y/z/o when the boss dies',
  `death_x` FLOAT NOT NULL DEFAULT 0,
  `death_y` FLOAT NOT NULL DEFAULT 0,
  `death_z` FLOAT NOT NULL DEFAULT 0,
  `death_o` FLOAT NOT NULL DEFAULT 0,
  `death_despawn_s` INT UNSIGNED NOT NULL DEFAULT 900,
  `walker_entry` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'nearest creature of this entry walks to walker_x/y/z and leaves when the boss dies',
  `walker_x` FLOAT NOT NULL DEFAULT 0,
  `walker_y` FLOAT NOT NULL DEFAULT 0,
  `walker_z` FLOAT NOT NULL DEFAULT 0,
  `walker_say` VARCHAR(255) NOT NULL DEFAULT '',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-ground-telegraphs: data-driven bosses';

-- The attacks. shape: 1 line, 2 cone, 3 nova, 4 donut, 5 erupt, 6 dance, 7 cast. 0 in size / param = the default.
CREATE TABLE IF NOT EXISTS `ground_telegraph_attack` (
  `boss_entry` INT UNSIGNED NOT NULL,
  `attack_id` TINYINT UNSIGNED NOT NULL,
  `shape` TINYINT UNSIGNED NOT NULL DEFAULT 3,
  `size` FLOAT NOT NULL DEFAULT 0 COMMENT 'line / cone length (40), nova radius (10), donut outer radius (20), erupt radius of each (3)',
  `param` FLOAT NOT NULL DEFAULT 0 COMMENT 'line width (2), cone angle in degrees (60), donut inner radius (size / 2)',
  `count` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'erupt: number of marks (3)',
  `spread` FLOAT NOT NULL DEFAULT 0 COMMENT 'erupt: radius around the target (8)',
  `bands` VARCHAR(64) NOT NULL DEFAULT '' COMMENT 'dance: rings around the boss, e.g. 0-10,20-40',
  `delay_ms` INT UNSIGNED NOT NULL DEFAULT 3000 COMMENT 'mark on the ground until the hit',
  `every_ms` INT UNSIGNED NOT NULL DEFAULT 12000 COMMENT '0 = only as another attack''s next_attack',
  `first_ms` INT UNSIGNED NOT NULL DEFAULT 5000 COMMENT 'first use after the pull',
  `next_attack` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'attack_id that follows at once (a dance)',
  `hit_spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'with hit_damage: its school and log name; without: cast on each player hit',
  `hit_damage` INT NOT NULL DEFAULT 0 COMMENT 'direct damage per player hit (exact, never misses)',
  `friendly` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 = green mark, hit_spell cast on friendly players inside',
  `visual_kit` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'SpellVisualKit played once per burst when the attack lands; 0 = none (see README)',
  `visual_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 by shape, 1 wave, 2 scatter, 3 single, 4 ring',
  `visual_scale` FLOAT NOT NULL DEFAULT 0 COMMENT 'caster scale per burst (0 = 1); single on a nova: the scale for a 10 yd radius',
  `visual_spacing` FLOAT NOT NULL DEFAULT 0 COMMENT 'yd between bursts (0 = 2)',
  `visual_pace` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'ms for a wave to cross the mark, or the window of scattered bursts (0 = 800)',
  `visual_zoff` FLOAT NOT NULL DEFAULT 0 COMMENT 'yd added to the ground height of each burst',
  `beam_spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'line: beam to the far end; erupt: a missile onto each mark (see README)',
  `nova_kit` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'dance / donut: kit played once in the centre of each disc band',
  `nova_scale` FLOAT NOT NULL DEFAULT 0 COMMENT 'nova_kit caster scale for a 10 yd radius (follows the radius)',
  `nova_zoff` FLOAT NOT NULL DEFAULT 0 COMMENT 'yd added to the ground height of the nova',
  `comment` VARCHAR(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`boss_entry`, `attack_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-ground-telegraphs: boss attacks';

-- 9530999 Ground Telegraph Visual Caster: a copy of the Invisible Stalker (15214) that clients can see cast spells
-- (only its invisible display 11686, no trigger flag 128, not attackable).
DELETE FROM `creature_template_model` WHERE `CreatureID` = 9530999;
DELETE FROM `creature_template` WHERE `entry` = 9530999;
DROP TEMPORARY TABLE IF EXISTS `tmp_ground_telegraph_ct`;
CREATE TEMPORARY TABLE `tmp_ground_telegraph_ct` LIKE `creature_template`;
INSERT INTO `tmp_ground_telegraph_ct` SELECT * FROM `creature_template` WHERE `entry` = 15214;
UPDATE `tmp_ground_telegraph_ct` SET `entry` = 9530999, `name` = 'Ground Telegraph Visual Caster', `subname` = '',
  `flags_extra` = `flags_extra` & ~128, `unit_flags` = `unit_flags` | 2, `AIName` = '', `ScriptName` = '';
INSERT INTO `creature_template` SELECT * FROM `tmp_ground_telegraph_ct`;
DROP TEMPORARY TABLE `tmp_ground_telegraph_ct`;
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`)
VALUES (9530999, 0, 11686, 1, 1, 0);
-- Movement as the stock Invisible Stalker 15214 (a flyer: no gravity, so the client keeps the exact height it is given;
-- the clone above copies creature_template only, and without this row the caster was a ground unit). 1.1, 2026-11-28.
DELETE FROM `creature_template_movement` WHERE `CreatureId` = 9530999;
INSERT INTO `creature_template_movement` (`CreatureId`, `Ground`, `Swim`, `Flight`, `Rooted`, `Chase`, `Random`, `InteractionPauseTimer`)
VALUES (9530999, 0, 0, 1, 0, 0, 0, NULL);
