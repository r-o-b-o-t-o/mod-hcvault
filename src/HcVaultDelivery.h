#ifndef MOD_HCVAULT_DELIVERY_H
#define MOD_HCVAULT_DELIVERY_H

#include "Define.h"

#include <string>
#include <vector>

namespace HcVault
{
    struct Config;
    class Stock;

    /// One approved line the website wants put in the mail.
    struct DeliveryLine
    {
        int32 LineId = 0;
        uint32 ItemId = 0;
        int32 SuffixId = 0;
        uint32 Quantity = 0;
    };

    /// Everything approved for one order.
    struct Delivery
    {
        int32 OrderId = 0;

        /// The short reference the website showed whoever placed the order. It goes in the mail's
        /// subject, so a parcel in game can be matched to a card in the backoffice. May be empty.
        std::string Reference;

        std::string Recipient;

        /// Copper to include, or 0 when the order asked for none or it is not approved.
        uint64 Copper = 0;

        std::vector<DeliveryLine> Items;
    };

    /// What became of one line — or, when `IsMoney`, of the order's money.
    struct DeliveryOutcome
    {
        int32 OrderId = 0;
        int32 LineId = 0;
        bool IsMoney = false;
        bool Delivered = false;

        /// Why not, phrased for whoever reads the order card. Empty on success.
        std::string Reason;
    };

    /// A reply the operator wrote to a donation letter, waiting to be posted.
    ///
    /// The mail it answers is long gone — collecting it is what deletes it — so this is an ordinary
    /// new mail that happens to carry "RE:" and the original subject.
    struct Reply
    {
        int32 ReplyId = 0;
        std::string Recipient;
        std::string Subject;
        std::string Body;
    };

    /// What became of one reply.
    struct ReplyOutcome
    {
        int32 ReplyId = 0;
        bool Sent = false;

        /// Why not, phrased for whoever reads the message card. Empty on success.
        std::string Reason;
    };

    /// Posts one reply, recording it in `mod_hcvault_reply` in the same transaction so a report lost
    /// on the way back cannot send it twice.
    ///
    /// Must run on the world thread, with the vault character offline.
    ReplyOutcome SendReply(Config const& config, Reply const& reply);

    /// An order whose recipient the website wants described.
    struct RecipientRequest
    {
        int32 OrderId = 0;
        std::string Recipient;
    };

    /// What the game knows about that recipient.
    struct RecipientInfo
    {
        int32 OrderId = 0;

        /// False only when no character has ever borne the name. A hardcore death deletes the
        /// character, so this stays true for one that died — the card should say "dead", not "no such
        /// character".
        bool Exists = false;

        /// The realm still has this character, so mail can reach it. False for one that died.
        bool Live = false;

        /// Low guid of whoever the name resolved to, or 0 when it resolved to nobody. Carried so the
        /// caller need not look the name up a second time and hope for the same answer.
        uint32 Guid = 0;

        bool HasLevel = false;
        uint8 Level = 0;

        bool HasClass = false;
        uint8 Class = 0;

        /// False when the character is running no challenge at all, which is not the same as one
        /// whose challenge simply does not match.
        bool HasChallenge = false;
        int32 Challenge = 0;

        bool Eligible = false;
        bool Dead = false;
    };

    /// Looks up one recipient: whether the name exists, its level, and its challenge state.
    /// Runs on the world thread; the challenge table is read through the characters connection, which
    /// can reach it because both databases live on the same server.
    RecipientInfo DescribeRecipient(Config const& config, RecipientRequest const& request);

    /// Mails one order's approved lines.
    ///
    /// Returns one outcome per line and, when money was approved, one more for the money. A line that
    /// cannot be delivered fails on its own: the rest of the order still goes out, because refusing
    /// everything over one missing stack helps nobody.
    ///
    /// The whole order is one transaction: the goods leaving the vault, the mail carrying them, the
    /// money and the delivery records land together or not at all. Delivery is therefore recorded at
    /// the same instant the goods move, so a report lost on the way back to the website cannot cause
    /// them to be sent twice — the next attempt recognises the line and reports it delivered without
    /// touching the vault.
    ///
    /// `availableCopper` is the vault character's purse for this cycle, seeded once by the caller and
    /// decremented as money goes out. Threaded through rather than re-read per order because the
    /// characters table is only updated when the mail is committed, and two orders in one cycle would
    /// otherwise each see the full balance and together overdraw it.
    ///
    /// Must run on the world thread, with the vault character offline.
    std::vector<DeliveryOutcome> DeliverOrder(Config const& config, Stock& stock, Delivery const& delivery,
                                              uint64& availableCopper);

    /// Reads what the vault character is carrying, in copper.
    uint64 ReadVaultMoney(uint32 vaultCharacterGuid);
}

#endif // MOD_HCVAULT_DELIVERY_H
