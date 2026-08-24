#include "HcVaultDelivery.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "HcVaultCharacters.h"
#include "HcVaultConfig.h"
#include "HcVaultStock.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"

#include <algorithm>
#include <unordered_set>

namespace HcVault
{
    namespace
    {
        /// Items a single mail can carry. Anything more has to be a second mail.
        constexpr std::size_t kMailItemLimit = MAX_MAIL_ITEMS;

        /// Line id recorded for an order's money, which has no line of its own.
        constexpr int32 kMoneyLineId = 0;

        bool ChallengeSatisfies(Config const& config, int32 challenge)
        {
            auto const mask = static_cast<int32>(config.AllowedChallengeMask);

            // Exact by default: the server only lets characters trade with others running the
            // identical set of challenges, so a character carrying extra ones cannot receive the mail
            // even though its mask contains what is asked for.
            return config.ChallengeMatchExact ? challenge == mask : (challenge & mask) == mask;
        }

        /// Line ids of this order already recorded as delivered.
        std::unordered_set<int32> AlreadyDelivered(int32 orderId)
        {
            std::unordered_set<int32> delivered;

            if (QueryResult result = CharacterDatabase.Query(
                    "SELECT line_id FROM mod_hcvault_delivery WHERE order_id = {}", orderId))
            {
                do
                {
                    delivered.insert(result->Fetch()[0].Get<int32>());
                } while (result->NextRow());
            }

            return delivered;
        }

        /// One mail's worth of goods, and the lines that are fully satisfied by it.
        struct MailBatch
        {
            std::vector<Item*> Items;
            std::vector<int32> LineIds;
            uint64 Copper = 0;
        };

        /// Puts one batch in the mail and records it, in the order's transaction.
        void SendBatch(Config const& config, MailBatch& batch, Delivery const& delivery,
                       ObjectGuid::LowType recipientGuid, CharacterDatabaseTransaction trans)
        {
            int32 const orderId = delivery.OrderId;

            MailDraft draft(MailSubjectFor(delivery.Reference), config.MailBody);

            for (Item* item : batch.Items)
            {
                // The item changes hands here: owned by the recipient and saved before the mail row
                // references it, which is the order WorldSession::HandleSendMail uses.
                item->SetOwnerGUID(ObjectGuid(HighGuid::Player, recipientGuid));
                item->FSetState(ITEM_CHANGED);
                item->SaveToDB(trans);
                draft.AddItem(item);
            }

            if (batch.Copper > 0)
            {
                draft.AddMoney(static_cast<uint32>(batch.Copper));

                // Guarded on the balance so the purse can never go negative, however the caller's
                // running figure and the table came to disagree.
                trans->Append("UPDATE characters SET money = money - {} WHERE guid = {} AND money >= {}",
                    batch.Copper, config.VaultCharacterGuid, batch.Copper);
            }

            // Recorded in the same transaction as the mail, so "the goods left" and "we know they
            // left" are one fact. A report lost on the way back to the website is then harmless.
            uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());
            for (int32 lineId : batch.LineIds)
            {
                trans->Append(
                    "INSERT INTO mod_hcvault_delivery (order_id, line_id, sent_at) VALUES ({}, {}, {}) "
                    "ON DUPLICATE KEY UPDATE sent_at = sent_at",
                    orderId, lineId, now);
            }

            Player* online = ObjectAccessor::FindPlayerByLowGUID(recipientGuid);
            MailReceiver const receiver = online
                ? MailReceiver(online, recipientGuid)
                : MailReceiver(recipientGuid);

            // Sent as the vault character so the mail reads as coming from it, and can be returned to
            // it — a return simply lands back in the mailbox and is collected as a donation.
            draft.SendMailTo(trans, receiver,
                MailSender(MAIL_NORMAL, config.VaultCharacterGuid, MAIL_STATIONERY_DEFAULT),
                MAIL_CHECK_MASK_NONE);

            // SendMailTo took the items: an online receiver keeps them, an offline one has them
            // deleted from memory. Either way they are no longer ours to free.
            batch.Items.clear();
        }

        /// Fails every line of a delivery for the same reason — except any already in the post.
        ///
        /// A line whose mail went out on an earlier attempt is reported delivered whatever is wrong
        /// with the recipient now. The goods have changed hands; a character that has since died, or
        /// swapped challenge modes, does not make that untrue, and reporting it failed would leave the
        /// website waiting to send something it has already sent.
        std::vector<DeliveryOutcome> RefuseAll(Delivery const& delivery, std::string const& reason,
                                               std::unordered_set<int32> const& alreadyDelivered)
        {
            std::vector<DeliveryOutcome> outcomes;
            outcomes.reserve(delivery.Items.size() + 1);

            for (DeliveryLine const& line : delivery.Items)
            {
                bool const sent = alreadyDelivered.count(line.LineId) != 0;
                outcomes.push_back({ delivery.OrderId, line.LineId, false, sent, sent ? std::string() : reason });
            }

            if (delivery.Copper > 0)
            {
                bool const sent = alreadyDelivered.count(kMoneyLineId) != 0;
                outcomes.push_back({ delivery.OrderId, kMoneyLineId, true, sent, sent ? std::string() : reason });
            }

            return outcomes;
        }
    }

    RecipientInfo DescribeRecipient(Config const& config, RecipientRequest const& request)
    {
        RecipientInfo info;
        info.OrderId = request.OrderId;

        ResolvedCharacter const character = ResolveByName(config, request.Recipient);
        if (!character.Found)
            return info;

        info.Exists = true;
        info.Live = character.Live;
        info.Guid = character.Guid;
        info.HasLevel = character.HasLevel;
        info.Level = character.Level;
        info.HasClass = character.HasClass;
        info.Class = character.Class;
        info.HasChallenge = character.HasChallenge;
        info.Challenge = character.Challenge;
        info.Dead = character.Dead;

        // Live first, and not as a formality: a character the realm has deleted cannot be mailed at
        // all, whatever its record says. That is what a hardcore death leaves behind.
        info.Eligible = character.Live
            && info.HasChallenge
            && ChallengeSatisfies(config, info.Challenge)
            && !(config.RefuseDeadRecipients && info.Dead);

        return info;
    }

    ReplyOutcome SendReply(Config const& config, Reply const& reply)
    {
        ReplyOutcome outcome;
        outcome.ReplyId = reply.ReplyId;

        // Already posted on an attempt whose report never arrived. Saying so again is the whole point
        // of keeping the record.
        if (QueryResult existing = CharacterDatabase.Query(
                "SELECT 1 FROM mod_hcvault_reply WHERE reply_id = {}", reply.ReplyId))
        {
            LOG_INFO("module.hcvault",
                "[HCVault] Reply {} was already sent; reporting it again rather than resending.", reply.ReplyId);

            outcome.Sent = true;
            return outcome;
        }

        ResolvedCharacter const recipient = ResolveByName(config, reply.Recipient);

        // A reply needs somewhere to land, and that is all it needs: no challenge check and no
        // eligibility, because answering a letter is not the same as handing out goods. Somebody who
        // donated and then died deserves an answer if the character is still there to read it.
        if (!recipient.Found)
        {
            outcome.Reason = "no character named " + reply.Recipient + " has ever existed on this realm";
            return outcome;
        }

        if (!recipient.Live)
        {
            outcome.Reason = recipient.Dead
                ? reply.Recipient + " has died, so there is no mailbox left to write to"
                : reply.Recipient + " is no longer on the realm";
            return outcome;
        }

        auto const recipientLow = recipient.Guid;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        MailDraft draft(reply.Subject, reply.Body);

        trans->Append("INSERT INTO mod_hcvault_reply (reply_id, sent_at) VALUES ({}, {}) "
                      "ON DUPLICATE KEY UPDATE sent_at = sent_at",
            reply.ReplyId, static_cast<uint32>(GameTime::GetGameTime().count()));

        Player* online = ObjectAccessor::FindPlayerByLowGUID(recipientLow);
        MailReceiver const receiver = online
            ? MailReceiver(online, recipientLow)
            : MailReceiver(recipientLow);

        draft.SendMailTo(trans, receiver,
            MailSender(MAIL_NORMAL, config.VaultCharacterGuid, MAIL_STATIONERY_DEFAULT),
            MAIL_CHECK_MASK_NONE);

        CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module.hcvault", "[HCVault] Replied to {} (reply {}).", reply.Recipient, reply.ReplyId);

        outcome.Sent = true;
        return outcome;
    }

    uint64 ReadVaultMoney(uint32 vaultCharacterGuid)
    {
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT money FROM characters WHERE guid = {}", vaultCharacterGuid))
            return result->Fetch()[0].Get<uint32>();

        return 0;
    }

    std::vector<DeliveryOutcome> DeliverOrder(Config const& config, Stock& stock, Delivery const& delivery,
                                              uint64& availableCopper)
    {
        // Read first, because it outranks every refusal below: what has already been mailed has already
        // been mailed.
        auto const alreadyDelivered = AlreadyDelivered(delivery.OrderId);

        RecipientRequest describe;
        describe.OrderId = delivery.OrderId;
        describe.Recipient = delivery.Recipient;

        RecipientInfo const info = DescribeRecipient(config, describe);
        if (!info.Eligible)
        {
            // Ordered by what actually stops the mail, the way SendReply orders it: whether there is
            // a mailbox at all, then whether the vault serves whoever owns it. Death enters twice and
            // means two different things — as the reason the mailbox is gone, and further down as the
            // policy refusal it is for a character still standing. Reported ahead of both, it told a
            // character refused for the wrong challenge that it was dead instead.
            std::string reason;
            if (!info.Exists)
                reason = "no character named " + delivery.Recipient + " has ever existed on this realm";
            else if (!info.Live)
                reason = info.Dead
                    ? delivery.Recipient + " has died, so there is no mailbox left to deliver to"
                    : delivery.Recipient + " is no longer on the realm";
            else if (!info.HasChallenge)
                reason = delivery.Recipient + " is not running any challenge mode";
            else if (!ChallengeSatisfies(config, info.Challenge))
                reason = delivery.Recipient + " runs challenge " + std::to_string(info.Challenge)
                    + ", and the vault only serves " + std::to_string(config.AllowedChallengeMask);
            else
                reason = delivery.Recipient + " is dead";

            return RefuseAll(delivery, reason, alreadyDelivered);
        }

        // The guid the description resolved, rather than a second lookup of the same name. Asking
        // twice invited the two answers to differ, and an empty guid quietly becomes 0 — an address
        // for nobody, which the mail would be written to all the same.
        auto const recipientLow = info.Guid;

        // One transaction for the whole order: the claims, the mails, the money and the delivery
        // records land together or not at all.
        //
        // It used to be one per claim and one per mail, which left a window between them where the
        // goods belonged to nobody and were no longer listed in the vault — invisible to the module,
        // to the recipient and to every player. A crash there lost them outright. Nothing in this
        // function reads back what it writes, so there is no reason for the commits to be separate.
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        std::vector<DeliveryOutcome> outcomes;

        // Claimed up front, one entry per line, so a line that cannot be filled fails on its own and
        // the goods for the rest are already in hand when the mails are built.
        struct ClaimedLine
        {
            int32 LineId = 0;
            std::vector<Item*> Items;
        };

        std::vector<ClaimedLine> claimed;

        for (DeliveryLine const& line : delivery.Items)
        {
            if (alreadyDelivered.count(line.LineId))
            {
                // Sent on an earlier attempt whose report never arrived. Saying so again is the whole
                // point of the delivery log.
                LOG_INFO("module.hcvault",
                    "[HCVault] Order {} line {} was already delivered; reporting it again rather than resending.",
                    delivery.OrderId, line.LineId);
                outcomes.push_back({ delivery.OrderId, line.LineId, false, true, {} });
                continue;
            }

            StackKey key;
            key.Entry = line.ItemId;
            key.SuffixId = line.SuffixId;

            // Asked before anything is claimed, not after. A line has to fit in one mail, because the
            // delivery record is written with that mail: spread over two, a crash between them would
            // leave goods sent and unrecorded, and the retry would send them again. Refusing after the
            // fact is not an option either — a claim mutates the in-memory vault, and dropping the
            // transaction would roll back only the database half.
            //
            // The count is what the game itself would use: a claim merges what it takes into full
            // stacks, so this is only ever reached by an order genuinely larger than twelve stacks of
            // the thing, never by a vault that happens to be holding it in fragments.
            if (std::size_t const slots = stock.SlotsRequired(key, line.Quantity); slots > kMailItemLimit)
            {
                outcomes.push_back({ delivery.OrderId, line.LineId, false, false,
                    "this is " + std::to_string(slots) + " stacks and one mail holds "
                    + std::to_string(kMailItemLimit) + "; order it in smaller amounts" });
                continue;
            }

            ClaimedLine entry;
            entry.LineId = line.LineId;

            std::string error;

            // A failed claim writes nothing but the odd stale-reference cleanup, which rides along in
            // the same transaction — worth keeping so the next cycle reports an honest count.
            bool const claimedOk = stock.Claim(key, line.Quantity, trans, entry.Items, error);

            if (!claimedOk)
            {
                outcomes.push_back({ delivery.OrderId, line.LineId, false, false, error });
                continue;
            }

            claimed.push_back(std::move(entry));
        }

        bool sendMoney = delivery.Copper > 0 && !alreadyDelivered.count(kMoneyLineId);
        if (delivery.Copper > 0 && !sendMoney)
            outcomes.push_back({ delivery.OrderId, kMoneyLineId, true, true, {} });

        if (sendMoney && availableCopper < delivery.Copper)
        {
            outcomes.push_back({ delivery.OrderId, kMoneyLineId, true, false,
                "the vault character is carrying " + std::to_string(availableCopper / 10000)
                + " gold, not " + std::to_string(delivery.Copper / 10000) });
            sendMoney = false;
        }

        // Packed greedily, never splitting a line across mails. Most lines are one or two items, so
        // an order usually leaves as a single mail.
        std::vector<MailBatch> batches;
        for (ClaimedLine& line : claimed)
        {
            if (batches.empty() || batches.back().Items.size() + line.Items.size() > kMailItemLimit)
                batches.emplace_back();

            MailBatch& batch = batches.back();
            batch.Items.insert(batch.Items.end(), line.Items.begin(), line.Items.end());
            batch.LineIds.push_back(line.LineId);
            line.Items.clear();
        }

        if (sendMoney)
        {
            if (batches.empty())
                batches.emplace_back();

            // Money rides on the first mail rather than one of its own: it is the same delivery, and
            // two envelopes for one order reads like a mistake.
            batches.front().Copper = delivery.Copper;
            batches.front().LineIds.push_back(kMoneyLineId);
        }

        for (MailBatch& batch : batches)
        {
            SendBatch(config, batch, delivery, recipientLow, trans);

            if (batch.Copper > 0)
                availableCopper -= std::min(availableCopper, batch.Copper);

            for (int32 lineId : batch.LineIds)
                outcomes.push_back({ delivery.OrderId, lineId, lineId == kMoneyLineId, true, {} });
        }

        // Queued rather than committed inline: Item::SaveToDB and the mail statements are registered
        // CONNECTION_ASYNC and are only ever prepared on the async connections, so a direct commit
        // takes a synchronous one, finds them unprepared and asserts.
        //
        // An order that refused every line and moved nothing leaves an empty transaction, which a
        // release build would otherwise hand to the worker to open and close for nothing.
        if (trans->GetSize() > 0)
            CharacterDatabase.CommitTransaction(trans);

        LOG_INFO("module.hcvault", "[HCVault] Order {} to {}: {} outcome(s) across {} mail(s).",
            delivery.OrderId, delivery.Recipient, outcomes.size(), batches.size());

        return outcomes;
    }
}
