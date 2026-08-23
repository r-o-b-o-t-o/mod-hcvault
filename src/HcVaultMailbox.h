#ifndef MOD_HCVAULT_MAILBOX_H
#define MOD_HCVAULT_MAILBOX_H

#include "Define.h"

#include <string>
#include <vector>

namespace HcVault
{
    class Stock;

    /// One stack attached to the mail a letter came in.
    ///
    /// A record of what arrived, not a claim on it: the items are in the vault by the time this is
    /// forwarded, and the stock push is what accounts for them.
    struct LetterAttachment
    {
        uint32 Entry = 0;
        int32 SuffixId = 0;
        uint32 Count = 0;
    };

    /// A letter somebody wrote on a donation. The mail it came in is deleted once it has been
    /// collected, so this is the only thing that survives of it.
    struct CollectedLetter
    {
        uint32 MailId = 0;

        /// Sender's character name. Empty when the mail was not sent by a player, which the website
        /// shows as an unnamed sender rather than inventing one.
        std::string Sender;

        std::string Subject;
        std::string Body;

        /// When the game delivered the mail, in Unix seconds.
        uint32 SentAt = 0;

        /// Copper that came in the same mail, or 0.
        uint64 Copper = 0;

        /// What was attached to it, so the note can be read against what it is about.
        std::vector<LetterAttachment> Attachments;
    };

    struct ScanResult
    {
        uint32 MailsCollected = 0;
        uint32 ItemsCollected = 0;
        uint64 CopperCollected = 0;

        /// Mails deliberately left in the box — COD, and anything whose items could not be taken.
        uint32 MailsSkipped = 0;

        std::vector<CollectedLetter> Letters;
    };

    /// Empties the vault character's mailbox into the vault.
    ///
    /// Items are moved into `stock`, money is added to the character's purse, and anything written in
    /// the body is kept so it can be forwarded. The mail row is then deleted and the character cache
    /// told about it, exactly as the in-game handlers do.
    ///
    /// Must run on the world thread, and only while the vault character is offline: a logged-in
    /// player holds its mail in memory and would save its own view of the mailbox back over this.
    ScanResult CollectDonations(uint32 vaultCharacterGuid, Stock& stock);
}

#endif // MOD_HCVAULT_MAILBOX_H
