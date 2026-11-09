-- mod-hcvault: plain mail the operator wrote to a character, answering no letter.

-- Plain mails this module has already put in the mail.
--
-- The same job `mod_hcvault_reply` does for replies: written in the transaction that sends the mail,
-- so a mail whose report never reached the website is recognised on the next poll and reported rather
-- than sent a second time. The website owns the text; this remembers only that it went.
--
-- Keyed on the website's token rather than its id. The website's ids start again from 1 when its
-- database is rebuilt, and a new mail matched against an old one's id would be reported sent without
-- being posted.
CREATE TABLE IF NOT EXISTS `mod_hcvault_outgoing` (
    `token` CHAR(36) NOT NULL COMMENT 'Website plain-mail GUID',
    `outgoing_id` INT NOT NULL COMMENT 'Website plain-mail id, for reading the table by hand',
    `sent_at` INT UNSIGNED NOT NULL COMMENT 'Unix seconds',
    PRIMARY KEY (`token`)
) ENGINE = InnoDB DEFAULT CHARSET = utf8mb4 COLLATE = utf8mb4_unicode_ci
  COMMENT = 'Plain mail the Hardcore Vault has already sent';
