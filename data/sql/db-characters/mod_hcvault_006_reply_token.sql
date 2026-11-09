-- mod-hcvault: which reply each record is for, beyond its id.
--
-- The website's reply ids start again from 1 when its database is rebuilt, so an id alone can name two
-- different replies: a new reply offered under an old one's id would be reported sent without being
-- posted. The website now draws a GUID token for every reply, which is never reused, so it joins the
-- key and travels with every result — as an order's reference does in `mod_hcvault_delivery`.
--
-- Records already here keep an empty token, so they never match a reply that carries one. Matching
-- them on the id alone, as old deliveries are, would lose every reply a rebuilt website hands those ids
-- to; not matching them costs at most one letter posted twice.
--
-- Every step checks before it acts, so the file can run more than once: AzerothCore applies a module
-- file again whenever its contents change.

SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.COLUMNS
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_reply' AND COLUMN_NAME = 'token') = 0,
    'ALTER TABLE `mod_hcvault_reply` ADD COLUMN `token` VARCHAR(36) NOT NULL DEFAULT '''' COMMENT ''Website reply GUID; empty for records older than this column'' AFTER `reply_id`',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;

-- The token joins the key, so the same id under a different token is a different record.
SET @hcvault_sql := IF(
    (SELECT COUNT(*) FROM information_schema.KEY_COLUMN_USAGE
     WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'mod_hcvault_reply'
       AND CONSTRAINT_NAME = 'PRIMARY' AND COLUMN_NAME = 'token') = 0,
    'ALTER TABLE `mod_hcvault_reply` DROP PRIMARY KEY, ADD PRIMARY KEY (`reply_id`, `token`)',
    'DO 0');
PREPARE hcvault_stmt FROM @hcvault_sql;
EXECUTE hcvault_stmt;
DEALLOCATE PREPARE hcvault_stmt;
