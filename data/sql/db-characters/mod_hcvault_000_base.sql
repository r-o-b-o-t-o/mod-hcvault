-- mod-hcvault: the three tables the module owns, all in the characters database.
--
-- Nothing here duplicates what the core already stores. The vault's goods are ordinary
-- `item_instance` rows that belong to no character; `mod_hcvault_stock` is the list of which rows
-- those are, so counts and rolls are always read from the item itself and the two can never drift.

-- What the vault is holding.
CREATE TABLE IF NOT EXISTS `mod_hcvault_stock` (
    `item_guid` INT UNSIGNED NOT NULL COMMENT 'item_instance.guid, owned by nobody',
    `stored_at` INT UNSIGNED NOT NULL COMMENT 'Unix seconds, when it was taken out of the mail',
    PRIMARY KEY (`item_guid`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Items held by the Hardcore Vault';

-- Which order lines have already been put in the mail.
--
-- Written in the same transaction as the mail itself, which is what makes delivery exactly-once: if
-- the report back to the website is lost, the line is offered again and recognised here rather than
-- being sent a second time. `line_id` is the website's order line id, or 0 for an order's money.
CREATE TABLE IF NOT EXISTS `mod_hcvault_delivery` (
    `order_id` INT NOT NULL COMMENT 'Website order id',
    `line_id` INT NOT NULL COMMENT 'Website order line id, or 0 for the order money',
    `sent_at` INT UNSIGNED NOT NULL COMMENT 'Unix seconds',
    PRIMARY KEY (`order_id`, `line_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Order lines the Hardcore Vault has already mailed';

-- Letters that came in on donations, waiting to be forwarded to the website.
--
-- Buffered rather than pushed straight from memory because the mail they came from is deleted the
-- moment it is collected: a push that failed would otherwise lose the message for good. Rows are
-- removed once the website has acknowledged them.
CREATE TABLE IF NOT EXISTS `mod_hcvault_letter` (
    `mail_id` INT UNSIGNED NOT NULL COMMENT 'The mail row it came from, and the website key',
    `sender` VARCHAR(12) NOT NULL DEFAULT '' COMMENT 'Empty when the mail was not sent by a player',
    `subject` VARCHAR(255) NOT NULL DEFAULT '',
    `body` TEXT NOT NULL,
    `sent_at` INT UNSIGNED NOT NULL COMMENT 'Unix seconds, when the game delivered the mail',
    PRIMARY KEY (`mail_id`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Donation letters not yet forwarded to the Hardcore Vault website';
