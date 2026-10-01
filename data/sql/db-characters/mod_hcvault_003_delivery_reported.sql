-- mod-hcvault: whether the website has heard about each delivery, and which order it was for.
--
-- A report lost on the way back used to be recovered by the website offering the line again, and
-- this module recognising it in `mod_hcvault_delivery` and saying it went. That stopped being enough
-- once the operator could take an approved line back from the queue: a line taken back is never
-- offered again, so the report was never repeated, and the website went on reserving goods that were
-- already in somebody's mailbox.
--
-- Now a delivery is repeated every cycle, asked about or not, until a results push carrying it has
-- been answered with a success.
--
-- That makes the key matter more than it did. The website's order and line ids start again from 1
-- when its database is rebuilt, so an id alone can name two different orders: a delivery repeated
-- after a rebuild would mark a line on a brand-new order as sent, and a new order offered under an
-- old id would be reported delivered without anything being mailed. The reference — the first group
-- of the order's opaque public id, the one in the mail subject — is not reused, so it joins the key,
-- travels with every result, and the website refuses a result whose reference is not the order's.
--
-- Every step checks before it acts, so the file can run more than once. AzerothCore applies a module
-- file again whenever its contents change, and this one changed after it was first written: a realm
-- that applied an earlier version gets this one run on top, and both end at the same table instead
-- of an ALTER failing on a column that is already there and the worldserver refusing to start.

-- The order's reference.
SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_delivery' AND COLUMN_NAME = 'reference') = 0,
    'ALTER TABLE `mod_hcvault_delivery` ADD COLUMN `reference` VARCHAR(16) NOT NULL DEFAULT '''' COMMENT ''Short public reference of the order; empty for records older than this column'' AFTER `line_id`',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;

-- Whether the website has acknowledged it.
--
-- Added with a default of 1, so every record already here is taken as acknowledged rather than
-- repeated, and only then switched to 0 for what is written from now on. Done that way rather than
-- with an UPDATE because an UPDATE would run again with the file, and mark acknowledged whatever had
-- been written in between.
--
-- What that gives up: an unacknowledged record is either on a line the website still has as approved
-- — it is offered again and reported the old way — or on one it will never offer again. That is an
-- order deleted after the module picked it up, whose lines stay pending holding nothing; or a line
-- taken back from the queue while a website that could already do that ran alongside an older module,
-- which goes on reserving goods that are gone. Repeating the whole history instead would risk the
-- rebuilt-database mix-up above for every delivery ever made, with no reference to catch it.
SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_delivery' AND COLUMN_NAME = 'reported') = 0,
    'ALTER TABLE `mod_hcvault_delivery` ADD COLUMN `reported` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT ''Whether the website has acknowledged this delivery'' AFTER `sent_at`',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;

ALTER TABLE `mod_hcvault_delivery` ALTER COLUMN `reported` SET DEFAULT 0;

-- The reference joins the key, so the same ids under a different reference are a different record.
SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.KEY_COLUMN_USAGE
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_delivery'
       AND CONSTRAINT_NAME = 'PRIMARY' AND COLUMN_NAME = 'reference') = 0,
    'ALTER TABLE `mod_hcvault_delivery` DROP PRIMARY KEY, ADD PRIMARY KEY (`order_id`, `line_id`, `reference`)',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;

-- What the module reads every cycle: the oldest unacknowledged records.
SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.STATISTICS
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_delivery' AND INDEX_NAME = 'idx_unreported') = 0,
    'ALTER TABLE `mod_hcvault_delivery` ADD KEY `idx_unreported` (`reported`, `sent_at`)',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;
