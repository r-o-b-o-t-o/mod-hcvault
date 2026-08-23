#include "HcVaultMailbox.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "HcVaultStock.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "MailMgr.h"
#include "ObjectMgr.h"
#include "QueryResult.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HcVault
{
    namespace
    {
        /// One mail as the scan needs to see it.
        struct MailRow
        {
            uint32 Id = 0;
            uint8 MessageType = 0;
            uint32 Sender = 0;
            std::string Subject;
            std::string Body;
            uint32 Money = 0;
            uint32 Cod = 0;
            uint32 DeliverTime = 0;
        };

        /// Items asked for in one statement. High enough that a full mailbox is one round trip, low
        /// enough that the statement cannot grow towards what the server will accept.
        constexpr std::size_t kItemQueryChunk = 500;

        /// Mails emptied in one cycle. See the note on the query that uses it.
        constexpr uint32 kMailsPerCycle = 100;

        /// Loads every mailed item in one or two queries, keyed by guid.
        ///
        /// One query per item was the obvious way to write this and the wrong one: each is a
        /// synchronous round trip on the world thread, and a mailbox holding a hundred donations of a
        /// few stacks each would spend the tick doing nothing else. The count of round trips now
        /// depends on how much is in the box only in the loosest sense.
        ///
        /// An item whose row has gone, or whose entry has no template, is simply absent from the
        /// result — the caller reads that as "this mail cannot be collected" and leaves it alone.
        std::unordered_map<uint32, std::unique_ptr<Item>> LoadMailedItems(std::vector<uint32> const& guids)
        {
            std::unordered_map<uint32, std::unique_ptr<Item>> loaded;
            if (guids.empty())
                return loaded;

            loaded.reserve(guids.size());

            for (std::size_t start = 0; start < guids.size(); start += kItemQueryChunk)
            {
                std::size_t const stop = std::min(guids.size(), start + kItemQueryChunk);

                std::string list;
                for (std::size_t index = start; index < stop; ++index)
                {
                    if (index != start)
                        list += ',';

                    list += std::to_string(guids[index]);
                }

                // Column order is what Item::LoadFromDB expects; it reads fields 0 to 10 by index.
                // guid comes after those, so keying the map disturbs nothing.
                QueryResult result = CharacterDatabase.Query(
                    "SELECT creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, "
                    "randomPropertyId, durability, playedTime, text, itemEntry, guid "
                    "FROM item_instance WHERE guid IN ({})", list);

                if (!result)
                    continue;

                do
                {
                    Field* fields = result->Fetch();
                    uint32 const entry = fields[11].Get<uint32>();
                    uint32 const itemGuid = fields[12].Get<uint32>();

                    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
                    if (!proto)
                    {
                        LOG_ERROR("module.hcvault",
                            "[HCVault] Mailed item {} is entry {}, which has no template.", itemGuid, entry);
                        continue;
                    }

                    // Owner left empty: it belongs to the vault, which is not a character.
                    std::unique_ptr<Item> item(NewItemOrBag(proto));
                    if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, fields, entry))
                    {
                        LOG_ERROR("module.hcvault", "[HCVault] Mailed item {} could not be loaded.", itemGuid);
                        continue;
                    }

                    loaded.emplace(itemGuid, std::move(item));
                } while (result->NextRow());
            }

            return loaded;
        }

        /// The longest subject and body a letter is forwarded with.
        ///
        /// The game imposes no limit worth relying on: `mail.subject` and `mail.body` are LONGTEXT,
        /// and while the 3.3.5 client caps what a player can type, auction, GM and script mail are
        /// under no such restraint.
        ///
        /// Byte counts, and the same numbers the website validates against. Under MySQL's strict mode
        /// an over-long value is ERROR 1406 rather than a quiet truncation, which would fail the whole
        /// collection transaction and leave the mail in the box to be retried, identically, forever.
        constexpr std::size_t kMaxSubjectBytes = 200;
        constexpr std::size_t kMaxBodyBytes = 8000;

        /// Cuts a string to at most `maxBytes`, never through the middle of a character.
        ///
        /// Bytes rather than characters, because the three places this has to fit count differently:
        /// MySQL's VARCHAR counts characters, its TEXT counts bytes, and the website's validation
        /// counts UTF-16 code units. A UTF-8 string is never fewer bytes than it is characters, and
        /// never fewer bytes than it is UTF-16 units — the worst case is a 4-byte character worth two
        /// units — so one byte limit satisfies all three.
        ///
        /// Cutting mid-character would hand MySQL invalid UTF-8, which strict mode also refuses.
        std::string TruncateUtf8(std::string value, std::size_t maxBytes)
        {
            if (value.size() <= maxBytes)
                return value;

            // Back off to the start of whatever character straddles the cut: continuation bytes are
            // 10xxxxxx, and anything else begins a character.
            std::size_t cut = maxBytes;
            while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80)
                --cut;

            value.resize(cut);
            return value;
        }

        /// Escapes a value for a single-quoted SQL literal.
        ///
        /// Subjects and bodies are player-written text going into a statement built by hand: module
        /// code has no prepared statement of its own to bind to. Handed to the driver's own escaper
        /// rather than a table of replacements here, so it is the connection's character set that
        /// decides what needs escaping.
        std::string EscapeForSql(std::string value)
        {
            CharacterDatabase.EscapeString(value);
            return value;
        }

        std::string SenderName(MailRow const& mail)
        {
            // Only a player mail carries a character guid in `sender`; for creature or auction mail
            // the column is an entry id and naming a character from it would be a lie.
            if (mail.MessageType != MAIL_NORMAL)
                return {};

            std::string name;
            if (sCharacterCache->GetCharacterNameByGuid(ObjectGuid(HighGuid::Player, mail.Sender), name))
                return name;

            return {};
        }
    }

    ScanResult CollectDonations(uint32 vaultCharacterGuid, Stock& stock)
    {
        ScanResult result;

        uint32 const now = static_cast<uint32>(GameTime::GetGameTime().count());

        // Mail that has not been delivered yet is not in the box: the game would not show it either,
        // and collecting it early would take goods the sender can still cancel.
        // Bounded, because everything below happens on the world thread inside one tick: a transaction
        // per mail, and the items to go with them. A box that piled up while the module was off would
        // otherwise be collected in a single stall. What is left waits for the next cycle a minute
        // later, and the stock push is a full picture every time, so nothing is lost by waiting.
        QueryResult mails = CharacterDatabase.Query(
            "SELECT id, messageType, sender, subject, body, money, cod, deliver_time "
            "FROM mail WHERE receiver = {} AND deliver_time <= {} ORDER BY id LIMIT {}",
            vaultCharacterGuid, now, kMailsPerCycle);

        if (!mails)
            return result;

        std::vector<MailRow> rows;
        do
        {
            Field* fields = mails->Fetch();

            MailRow row;
            row.Id = fields[0].Get<uint32>();
            row.MessageType = fields[1].Get<uint8>();
            row.Sender = fields[2].Get<uint32>();
            row.Subject = fields[3].Get<std::string>();
            row.Body = fields[4].Get<std::string>();
            row.Money = fields[5].Get<uint32>();
            row.Cod = fields[6].Get<uint32>();
            row.DeliverTime = fields[7].Get<uint32>();

            rows.push_back(std::move(row));
        } while (mails->NextRow());

        // Every attachment in the box in one query, rather than one query per mail.
        std::map<uint32, std::vector<uint32>> attachments;
        if (QueryResult items = CharacterDatabase.Query(
                "SELECT mail_id, item_guid FROM mail_items WHERE receiver = {}", vaultCharacterGuid))
        {
            do
            {
                Field* fields = items->Fetch();
                attachments[fields[0].Get<uint32>()].push_back(fields[1].Get<uint32>());
            } while (items->NextRow());
        }

        // Every item this cycle will actually touch, in one go. COD mails are left alone, so there is
        // no reason to load what is attached to them.
        std::vector<uint32> wanted;
        for (MailRow const& mail : rows)
        {
            if (mail.Cod > 0)
                continue;

            if (auto const itr = attachments.find(mail.Id); itr != attachments.end())
                wanted.insert(wanted.end(), itr->second.begin(), itr->second.end());
        }

        std::unordered_map<uint32, std::unique_ptr<Item>> loadedItems = LoadMailedItems(wanted);

        for (MailRow const& mail : rows)
        {
            // Cash on delivery would have to be paid to get at the attachment, and paying on the
            // vault's behalf is not a decision this module gets to make. Left in the box, where it
            // stays visible and eventually expires back to its sender.
            if (mail.Cod > 0)
            {
                ++result.MailsSkipped;
                LOG_INFO("module.hcvault", "[HCVault] Mail {} is COD ({} copper); leaving it in the mailbox.",
                    mail.Id, mail.Cod);
                continue;
            }

            static std::vector<uint32> const noAttachments;
            auto const itr = attachments.find(mail.Id);
            std::vector<uint32> const& itemGuids = itr != attachments.end() ? itr->second : noAttachments;

            // Ownership moves out of the batch and into this mail, so the items are freed when it is
            // done with them whether it succeeds or not.
            std::vector<std::unique_ptr<Item>> collected;
            bool failed = false;

            for (uint32 itemGuid : itemGuids)
            {
                auto const found = loadedItems.find(itemGuid);
                if (found == loadedItems.end())
                {
                    failed = true;
                    break;
                }

                collected.push_back(std::move(found->second));
                loadedItems.erase(found);
            }

            if (failed)
            {
                // The mail is left exactly as it was. Deleting it would destroy whatever else was
                // attached, and half-collecting it would lose the rest silently. Whatever was taken
                // from the batch is freed here and loaded again next cycle.
                ++result.MailsSkipped;
                LOG_ERROR("module.hcvault",
                    "[HCVault] Mail {} could not be collected because one of its items failed to load; left in the mailbox.",
                    mail.Id);
                continue;
            }

            // One transaction per mail: the items landing in the vault, the money moving, and the mail
            // going away are one event, and a crash between them would either duplicate the goods or
            // lose them.
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

            // First, before anything touches the items themselves. Taking an item into the vault can
            // merge it into a stack already there and delete the row it arrived as, and a row in
            // mail_items pointing at an item that no longer exists — even for the rest of one
            // transaction — is not a state worth creating.
            trans->Append("DELETE FROM mail_items WHERE mail_id = {}", mail.Id);

            // Noted before the items are destroyed, and kept whether or not there turns out to be a
            // letter to attach it to — the cost is a few structs, and reaching back for it afterwards
            // would mean reading rows that have just been rewritten.
            std::vector<LetterAttachment> attached;
            attached.reserve(collected.size());

            for (std::unique_ptr<Item> const& item : collected)
            {
                stock.Add(item.get(), trans);
                result.ItemsCollected += item->GetCount();

                attached.push_back({ item->GetEntry(), item->GetItemRandomPropertyId(), item->GetCount() });
            }

            if (mail.Money > 0)
            {
                trans->Append("UPDATE characters SET money = money + {} WHERE guid = {}",
                    mail.Money, vaultCharacterGuid);
                result.CopperCollected += mail.Money;
            }

            if (!mail.Body.empty())
            {
                CollectedLetter letter;
                letter.MailId = mail.Id;
                letter.Sender = SenderName(mail);
                letter.Subject = TruncateUtf8(mail.Subject, kMaxSubjectBytes);
                letter.Body = TruncateUtf8(mail.Body, kMaxBodyBytes);

                if (letter.Subject.size() < mail.Subject.size() || letter.Body.size() < mail.Body.size())
                {
                    // Rare enough to be worth a line, and it used to be the thing that wedged the
                    // whole letter queue without saying anything at all.
                    LOG_INFO("module.hcvault",
                        "[HCVault] Mail {} has an over-long subject or body; forwarding it cut to {} and {} bytes.",
                        mail.Id, letter.Subject.size(), letter.Body.size());
                }

                letter.SentAt = mail.DeliverTime;
                letter.Copper = mail.Money;
                letter.Attachments = std::move(attached);

                // Buffered in the same transaction that deletes the mail. The letter has to outlive
                // the mail it came in, and a push that fails must not be the thing that loses it.
                trans->Append(
                    "REPLACE INTO mod_hcvault_letter (mail_id, sender, subject, body, money, sent_at) "
                    "VALUES ({}, '{}', '{}', '{}', {}, {})",
                    letter.MailId,
                    EscapeForSql(letter.Sender),
                    EscapeForSql(letter.Subject),
                    EscapeForSql(letter.Body),
                    letter.Copper,
                    letter.SentAt);

                // Summed by stack rather than one row per attachment: a mail can carry the same thing
                // twice, and the primary key would reject the second. "10 Linen Cloth" is what the
                // reader wants anyway.
                std::map<std::pair<uint32, int32>, uint32> byStack;
                for (LetterAttachment const& attachment : letter.Attachments)
                    byStack[{ attachment.Entry, attachment.SuffixId }] += attachment.Count;

                for (auto const& [stack, count] : byStack)
                {
                    trans->Append(
                        "REPLACE INTO mod_hcvault_letter_item (mail_id, item_entry, suffix_id, count) "
                        "VALUES ({}, {}, {}, {})",
                        letter.MailId, stack.first, stack.second, count);
                }

                result.Letters.push_back(std::move(letter));
            }

            trans->Append("DELETE FROM mail WHERE id = {}", mail.Id);

            // Queued, not committed inline. Item::SaveToDB and the mail statements are registered
            // CONNECTION_ASYNC, so they are only ever prepared on the async connections; committing
            // directly grabs a synchronous one, finds the statement unprepared and asserts.
            //
            // Nothing here reads back what it just wrote, so the queue costs nothing: the vault is
            // tracked in memory, and the purse is carried through the cycle by the caller rather than
            // re-read from the table.
            CharacterDatabase.CommitTransaction(trans);

            // The cached per-character mail count is what the client is told at login, and nothing
            // recounts it until the next restart. Every path that deletes a mail row has to say so.
            sMailMgr->OnMailDeleted(vaultCharacterGuid);

            ++result.MailsCollected;
        }

        // A full page means there is more waiting. Said plainly, because a backlog draining at a
        // hundred a minute is worth knowing about rather than inferring from the numbers.
        if (rows.size() >= kMailsPerCycle)
        {
            LOG_INFO("module.hcvault",
                "[HCVault] Took the first {} mail(s) this cycle; more are waiting and will follow.",
                kMailsPerCycle);
        }

        if (result.MailsCollected > 0 || result.MailsSkipped > 0)
        {
            LOG_INFO("module.hcvault",
                "[HCVault] Collected {} mail(s): {} item(s) and {} copper, {} letter(s) forwarded, {} left behind.",
                result.MailsCollected, result.ItemsCollected, result.CopperCollected,
                result.Letters.size(), result.MailsSkipped);
        }

        return result;
    }
}
