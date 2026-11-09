-- mod-hcvault: the letter buffer now carries every donation.
--
-- Every collected mail that brought in goods or money is buffered and forwarded, written on or not,
-- so the website can keep a record of who gave what. A row with an empty body is a donation with
-- nothing written on it. The tables keep their names: they hold the only copy of anything not yet
-- forwarded, and renaming them for accuracy is not worth a step that could lose it.

ALTER TABLE `mod_hcvault_letter`
    COMMENT = 'Donations, with any letter written on them, not yet forwarded to the Hardcore Vault website';

ALTER TABLE `mod_hcvault_letter_item`
    COMMENT = 'What came in the donations buffered in mod_hcvault_letter';
