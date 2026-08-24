-- mod-hcvault: who wrote a letter, and what was written back.

-- What the realm knew about the sender when the letter was collected.
--
-- Recorded here rather than looked up when the backoffice reads it, because by then the character
-- may be gone: a hardcore death deletes it, and the `characters` row is eventually purged outright.
-- The challenge-modes table outlives both, but only until that module prunes it.
ALTER TABLE `mod_hcvault_letter`
    ADD COLUMN `sender_class` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'Character class id, 0 when unknown' AFTER `sender`,
    ADD COLUMN `sender_level` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'Level, 0 when unknown' AFTER `sender_class`,
    ADD COLUMN `sender_challenge` TINYINT NULL DEFAULT NULL
        COMMENT 'Challenge bitmask, NULL when the sender runs none' AFTER `sender_level`,
    ADD COLUMN `sender_dead` TINYINT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'Whether the sender had died by the time the letter was collected' AFTER `sender_challenge`;

-- Replies the operator has written and this module has already put in the mail.
--
-- The same job `mod_hcvault_delivery` does for order lines: written in the transaction that sends the
-- mail, so a reply whose report never reached the website is recognised on the next poll and reported
-- rather than sent a second time. The website owns the text; this remembers only that it went.
CREATE TABLE IF NOT EXISTS `mod_hcvault_reply` (
    `reply_id` INT NOT NULL COMMENT 'Website reply id',
    `sent_at` INT UNSIGNED NOT NULL COMMENT 'Unix seconds',
    PRIMARY KEY (`reply_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Replies the Hardcore Vault has already mailed';
