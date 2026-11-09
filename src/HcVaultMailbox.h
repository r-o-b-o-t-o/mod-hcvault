#ifndef MOD_HCVAULT_MAILBOX_H
#define MOD_HCVAULT_MAILBOX_H

#include "Define.h"

#include <string>
#include <vector>

namespace HcVault
{
    struct Config;
    class Stock;

    /// One stack that came in a donation.
    ///
    /// A record of what arrived, not a claim on it: the items are in the vault by the time this is
    /// forwarded, and the stock push is what accounts for them.
    struct DonatedStack
    {
        uint32 Entry = 0;
        int32 SuffixId = 0;
        uint32 Count = 0;
    };

    /// A donation as it is forwarded to the website: who sent it, what came in it, and whatever was
    /// written on it. The mail is deleted once it has been collected, so this is the only thing that
    /// survives of it.
    ///
    /// Every mail that brought in goods or money is one of these, written on or not — the website
    /// keeps a record of each donation, and files the body as a letter only when there is one. A mail
    /// with a body and nothing else is one too, for the letter. That includes mail no player sent —
    /// auction, creature, or a GM's from the console — which goes with an empty `Sender` and shows as
    /// unnamed. Returned mail is not one: it is the vault's own delivery coming back, not a gift.
    struct CollectedDonation
    {
        uint32 MailId = 0;

        /// Sender's character name. Empty when the mail was not sent by a player, which the website
        /// shows as an unnamed sender rather than inventing one.
        std::string Sender;

        /// What the realm knew about the sender when the mail was collected.
        ///
        /// Kept rather than looked up later, because a hardcore death deletes the character and the
        /// row is purged in time — by the time somebody reads the record there may be nothing left to
        /// ask. Class and level are 0 when unknown; HasChallenge is false when they ran none.
        uint8 SenderClass = 0;
        uint8 SenderLevel = 0;
        bool SenderHasChallenge = false;
        int32 SenderChallenge = 0;
        bool SenderDead = false;

        std::string Subject;
        std::string Body;

        /// When the game delivered the mail, in Unix seconds.
        uint32 SentAt = 0;

        /// Copper that came in the same mail, or 0.
        uint64 Copper = 0;

        /// What was attached to it, so the note can be read against what it is about.
        std::vector<DonatedStack> Attachments;
    };

    struct ScanResult
    {
        uint32 MailsCollected = 0;
        uint32 ItemsCollected = 0;
        uint64 CopperCollected = 0;

        /// Mails deliberately left in the box — COD, and anything whose items could not be taken.
        uint32 MailsSkipped = 0;

        /// Collected mails that were the vault's own deliveries coming back. Their goods are in the
        /// vault again; they are not reported as donations.
        uint32 MailsReturned = 0;

        std::vector<CollectedDonation> Donations;
    };

    /// Empties the vault character's mailbox into the vault.
    ///
    /// Items are moved into `stock`, money is added to the character's purse, and who sent what —
    /// with anything written in the body — is kept so it can be forwarded. The mail row is then
    /// deleted and the character cache told about it, exactly as the in-game handlers do.
    ///
    /// Must run on the world thread, and only while the vault character is offline: a logged-in
    /// player holds its mail in memory and would save its own view of the mailbox back over this.
    ScanResult CollectDonations(Config const& config, uint32 vaultCharacterGuid, Stock& stock);
}

#endif // MOD_HCVAULT_MAILBOX_H
