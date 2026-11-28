-- mod-ground-telegraphs 1.1, part 2: beam_spell (beams on lines, missiles onto erupt marks) and the dance nova kit.
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

CALL `ground_telegraph_alter`('ground_telegraph_attack', 'beam_spell', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `beam_spell` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''line: beam to the far end; erupt: a missile onto each mark (see README)'' AFTER `visual_zoff`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'nova_kit', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `nova_kit` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT ''dance / donut: kit played once in the centre of each disc band'' AFTER `beam_spell`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'nova_scale', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `nova_scale` FLOAT NOT NULL DEFAULT 0 COMMENT ''nova_kit caster scale for a 10 yd radius (follows the radius)'' AFTER `nova_kit`');
CALL `ground_telegraph_alter`('ground_telegraph_attack', 'nova_zoff', 1, 'ALTER TABLE `ground_telegraph_attack` ADD COLUMN `nova_zoff` FLOAT NOT NULL DEFAULT 0 COMMENT ''yd added to the ground height of the nova'' AFTER `nova_scale`');

DROP PROCEDURE `ground_telegraph_alter`;
