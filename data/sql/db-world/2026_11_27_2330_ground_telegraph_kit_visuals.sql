-- mod-ground-telegraphs 1.1: attack visuals are SpellVisualKits on invisible casters; attacks send no text.
-- Converts a 1.0 install in place. Self-contained and idempotent: every change runs only when the table exists and the
-- column is (or is not) there, so it does nothing on a fresh install (ground_telegraph_00_base.sql has the 1.1 tables).

DROP PROCEDURE IF EXISTS `ground_telegraph_alter`;
DELIMITER //
CREATE PROCEDURE `ground_telegraph_alter`(IN tbl VARCHAR(64), IN col VARCHAR(64), IN want_present TINYINT, IN ddl TEXT)
BEGIN
    IF EXISTS (SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl)
        AND (SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = tbl AND COLUMN_NAME = col) <> want_present
    THEN
        SET @ground_telegraph_ddl = ddl;
        PREPARE s FROM @ground_telegraph_ddl;
        EXECUTE s;
        DEALLOCATE PREPARE s;
    END IF;
END//
DELIMITER ;

CALL `ground_telegraph_alter`('ground_telegraph_boss', 'advisor_entry', 0, 'ALTER TABLE `ground_telegraph_boss` DROP COLUMN `advisor_entry`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_kit', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `visual_kit` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''SpellVisualKit played once per burst when the attack lands; 0 = none (see README)'' AFTER `friendly`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_mode', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `visual_mode` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''0 by shape, 1 wave, 2 scatter, 3 single, 4 ring'' AFTER `visual_kit`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_spacing', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `visual_spacing` FLOAT NOT NULL DEFAULT 0 COMMENT ''yd between bursts (0 = 2)'' AFTER `visual_scale`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_pace', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `visual_pace` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''ms for a wave to cross the mark, or the window of scattered bursts (0 = 800)'' AFTER `visual_spacing`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_zoff', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `visual_zoff` FLOAT NOT NULL DEFAULT 0 COMMENT ''yd added to the ground height of each burst'' AFTER `visual_pace`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_spell', 0, 'ALTER TABLE `ground_telegraph_attack` DROP COLUMN `visual_spell`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'say', 0, 'ALTER TABLE `ground_telegraph_attack` DROP COLUMN `say`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'text_type', 0, 'ALTER TABLE `ground_telegraph_attack` DROP COLUMN `text_type`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'interrupt_say', 0, 'ALTER TABLE `ground_telegraph_attack` DROP COLUMN `interrupt_say`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'advice', 0, 'ALTER TABLE `ground_telegraph_attack` DROP COLUMN `advice`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'visual_scale', 0, 'ALTER TABLE `ground_telegraph_attack` MODIFY COLUMN `visual_scale` FLOAT NOT NULL DEFAULT 0 COMMENT ''caster scale per burst (0 = 1); single on a nova: the scale for a 10 yd radius'' AFTER `visual_mode`');

DROP PROCEDURE `ground_telegraph_alter`;
