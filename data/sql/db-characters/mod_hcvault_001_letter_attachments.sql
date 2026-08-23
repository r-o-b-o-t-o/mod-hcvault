-- mod-hcvault: what came in the same mail as a letter.
--
-- A donor's note is usually about the goods — "the axe is for whoever needs it, the rest is spares"
-- is unreadable on its own. The items themselves go straight into the vault and are counted by the
-- stock push; this is only a record of what arrived together, forwarded with the letter and thrown
-- away with it.

ALTER TABLE `mod_hcvault_letter`
    ADD COLUMN `money` INT UNSIGNED NOT NULL DEFAULT 0
        COMMENT 'Copper that came in the same mail' AFTER `body`;

CREATE TABLE IF NOT EXISTS `mod_hcvault_letter_item` (
    `mail_id` INT UNSIGNED NOT NULL COMMENT 'mod_hcvault_letter.mail_id',
    `item_entry` INT UNSIGNED NOT NULL,
    `suffix_id` INT NOT NULL DEFAULT 0 COMMENT 'item_instance.randomPropertyId, signed',
    `count` INT UNSIGNED NOT NULL,
    PRIMARY KEY (`mail_id`, `item_entry`, `suffix_id`),
    -- Cascaded, so clearing a letter once the website has it takes its attachment list with it and
    -- the two can never be left disagreeing about what arrived.
    CONSTRAINT `fk_hcvault_letter_item` FOREIGN KEY (`mail_id`)
        REFERENCES `mod_hcvault_letter` (`mail_id`) ON DELETE CASCADE
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'What was attached to the mail a Hardcore Vault letter came in';
