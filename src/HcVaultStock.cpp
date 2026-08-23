#include "HcVaultStock.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "QueryResult.h"

#include <algorithm>

namespace HcVault
{
    namespace
    {
        /// Smallest rows first, so odd leftovers are used up before a full stack is broken into. Over
        /// a few hundred orders that is the difference between a tidy vault and one full of ones and
        /// twos — and it costs the recipient nothing, because whatever leaves is merged back into full
        /// stacks before it is mailed.
        std::vector<StoredItem> SmallestFirst(std::vector<StoredItem> rows)
        {
            std::sort(rows.begin(), rows.end(),
                [](StoredItem const& left, StoredItem const& right) { return left.Count < right.Count; });

            return rows;
        }
    }

    void Stock::Load()
    {
        _stacks.clear();

        // The item_instance join is what makes the module's own table a list of references rather
        // than a second copy of the truth: counts and rolls are read from the item itself, so the two
        // can never drift apart.
        QueryResult result = CharacterDatabase.Query(
            "SELECT s.item_guid, ii.itemEntry, ii.count, ii.randomPropertyId "
            "FROM mod_hcvault_stock s "
            "INNER JOIN item_instance ii ON ii.guid = s.item_guid");

        uint32 loaded = 0;
        uint64 quantity = 0;

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();

                StackKey key;
                uint32 const itemGuid = fields[0].Get<uint32>();
                key.Entry = fields[1].Get<uint32>();
                uint32 const count = fields[2].Get<uint32>();
                key.SuffixId = fields[3].Get<int16>();

                if (count == 0)
                {
                    LOG_WARN("module.hcvault", "[HCVault] Vault item {} has a count of 0; skipping it.", itemGuid);
                    continue;
                }

                _stacks[key].push_back({ itemGuid, count });
                ++loaded;
                quantity += count;
            } while (result->NextRow());
        }

        // A row whose item is gone points at nothing. It cannot be delivered, cannot be counted, and
        // would be re-reported to the website every cycle, so it is cleared out here rather than
        // carried forever.
        CharacterDatabase.Execute(
            "DELETE s FROM mod_hcvault_stock s "
            "LEFT JOIN item_instance ii ON ii.guid = s.item_guid "
            "WHERE ii.guid IS NULL");

        LOG_INFO("module.hcvault", "[HCVault] >> Loaded {} vault item(s) in {} stack(s), {} item(s) in total.",
            loaded, _stacks.size(), quantity);

        // Every collection since packs the stack it lands in, so in steady state this finds nothing.
        // It earns its place on the first run after the merging was added, and after anything that put
        // rows in the table without going through Add.
        //
        // Over the keys rather than the entries, because Repack rewrites the vector each one holds.
        std::vector<StackKey> keys;
        keys.reserve(_stacks.size());
        for (auto const& [key, items] : _stacks)
            keys.push_back(key);

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        std::size_t merged = 0;
        for (StackKey const& key : keys)
            merged += Repack(key, trans);

        if (merged > 0)
        {
            CharacterDatabase.CommitTransaction(trans);
            LOG_INFO("module.hcvault", "[HCVault] >> Merged {} fragmented row(s) away; {} row(s) left.",
                merged, loaded - merged);
        }
    }

    void Stock::Add(Item* item, CharacterDatabaseTransaction trans)
    {
        if (!item)
            return;

        // Owned by nobody. A row still owned by the vault character would be deleted along with it
        // and, worse, could be handed back by a character load that finds no inventory row for it.
        item->SetOwnerGUID(ObjectGuid::Empty);
        item->FSetState(ITEM_CHANGED);
        item->SaveToDB(trans);

        StackKey key;
        key.Entry = item->GetEntry();
        key.SuffixId = item->GetItemRandomPropertyId();

        uint32 const itemGuid = item->GetGUID().GetCounter();
        _stacks[key].push_back({ itemGuid, item->GetCount() });

        trans->Append("REPLACE INTO mod_hcvault_stock (item_guid, stored_at) VALUES ({}, {})",
            itemGuid, static_cast<uint32>(GameTime::GetGameTime().count()));

        // Listed first and merged second, even though the row just listed may be one of the ones the
        // merge removes. The alternative is deciding where the goods go before they are in the vault,
        // and one wasted pair of statements on a donation is a poor trade for that.
        Repack(key, trans);
    }

    std::size_t Stock::Repack(StackKey const& key, CharacterDatabaseTransaction trans)
    {
        auto const itr = _stacks.find(key);
        if (itr == _stacks.end())
            return 0;

        std::vector<StoredItem>& rows = itr->second;
        if (rows.size() < 2)
            return 0;

        uint32 const maxStack = MergeableStackSize(key.Entry);
        if (maxStack == 0)
            return 0;

        uint64 total = 0;
        for (StoredItem const& row : rows)
            total += row.Count;

        std::size_t const needed = static_cast<std::size_t>((total + maxStack - 1) / maxStack);
        if (needed >= rows.size())
            return 0;

        // Fullest first, so rows already holding a whole stack keep the count they have and cost no
        // statement at all. Only what actually changes is written.
        std::sort(rows.begin(), rows.end(),
            [](StoredItem const& left, StoredItem const& right) { return left.Count > right.Count; });

        uint64 remaining = total;
        for (std::size_t index = 0; index < rows.size(); ++index)
        {
            uint32 const want = index < needed
                ? static_cast<uint32>(std::min<uint64>(maxStack, remaining))
                : 0;

            remaining -= want;

            StoredItem& row = rows[index];
            if (row.Count == want)
                continue;

            if (want == 0)
            {
                // Deleted by hand rather than through Item::SaveToDB(ITEM_REMOVED), which would mean a
                // SELECT per row to build an Item this function has no other use for — the whole point
                // of holding the vault in memory.
                //
                // What that path does beyond deleting the row is clear `character_gifts` for a wrapped
                // item. It cannot apply here: MergeableStackSize admits only items the game stacks,
                // and wrapping rewrites an item's entry to one of the six gift entries, every one of
                // which is stackable = 1. The other tables keyed on an item guid — the soulbound trade
                // window, refunds — belong to equippable loot, which it also refuses, and to items
                // that cannot be mailed at all and so can never reach the vault.
                //
                // Anything that loosens MergeableStackSize has to answer this again.
                trans->Append("DELETE FROM mod_hcvault_stock WHERE item_guid = {}", row.Guid);
                trans->Append("DELETE FROM item_instance WHERE guid = {}", row.Guid);
                continue;
            }

            trans->Append("UPDATE item_instance SET count = {} WHERE guid = {}", want, row.Guid);
            row.Count = want;
        }

        std::size_t const removed = rows.size() - needed;
        rows.resize(needed);
        return removed;
    }

    std::vector<StackTotal> Stock::Snapshot() const
    {
        std::vector<StackTotal> totals;
        totals.reserve(_stacks.size());

        for (auto const& [key, items] : _stacks)
        {
            uint32 quantity = 0;
            for (auto const& item : items)
                quantity += item.Count;

            if (quantity > 0)
                totals.push_back({ key.Entry, key.SuffixId, quantity });
        }

        return totals;
    }

    uint32 Stock::Available(StackKey const& key) const
    {
        auto const itr = _stacks.find(key);
        if (itr == _stacks.end())
            return 0;

        uint32 quantity = 0;
        for (auto const& item : itr->second)
            quantity += item.Count;

        return quantity;
    }

    uint32 Stock::MergeableStackSize(uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
            return 0;

        uint32 const maxStack = proto->GetMaxStackSize();
        if (maxStack <= 1)
            return 0;

        // Merging two rows keeps one and destroys the other, so anything an item remembers for itself
        // has to be the same on both or it is silently picked from the survivor.
        //
        // The suffix is half the stack key, so two rolls of the same item are already different stacks
        // and can never meet here. What is left is what an equippable item carries — enchantments,
        // durability, its maker — and a timed item's remaining life. Neither is something to guess at,
        // and both belong to items the game does not stack anyway; the handful of oddities that are
        // both stackable and equippable (joke items, a couple of test wands) are simply left alone.
        //
        // Ammo is the exception: stackable, equippable, and with nothing of its own to lose.
        //
        // This predicate is also what lets Repack delete a row with a plain DELETE instead of going
        // through Item::SaveToDB — read the note there before widening it.
        if (proto->InventoryType != INVTYPE_NON_EQUIP && proto->InventoryType != INVTYPE_AMMO)
            return 0;

        if (proto->Duration != 0)
            return 0;

        return maxStack;
    }

    std::size_t Stock::SlotsRequired(StackKey const& key, uint32 quantity) const
    {
        if (quantity == 0 || Available(key) < quantity)
            return 0;

        // In 64 bits because an unlimited stack reports itself as 0x7FFFFFFE and rounding up would
        // overflow a uint32 on the way.
        if (uint64 const maxStack = MergeableStackSize(key.Entry); maxStack != 0)
        {
            // However fragmented the vault's rows are, a claim merges what it takes, so the answer
            // depends only on the item's stack size.
            return static_cast<std::size_t>((quantity + maxStack - 1) / maxStack);
        }

        // Nothing will be merged, so the answer is however many rows the claim walks through. Counted
        // the same way Claim consumes them, so the two cannot disagree about what a claim would do.
        auto const itr = _stacks.find(key);
        if (itr == _stacks.end())
            return 0;

        std::size_t slots = 0;
        uint32 outstanding = quantity;

        for (StoredItem const& row : SmallestFirst(itr->second))
        {
            if (outstanding == 0)
                break;

            ++slots;

            // A row bigger than what is left is split, and the piece that leaves is the one slot just
            // counted. Everything smaller goes whole.
            outstanding -= std::min(row.Count, outstanding);
        }

        return slots;
    }

    uint64 Stock::TotalQuantity() const
    {
        uint64 quantity = 0;
        for (auto const& [key, items] : _stacks)
            for (auto const& item : items)
                quantity += item.Count;

        return quantity;
    }

    Item* Stock::LoadItem(uint32 itemGuid)
    {
        // Column order is what Item::LoadFromDB expects; it reads by index.
        QueryResult result = CharacterDatabase.Query(
            "SELECT creatorGuid, giftCreatorGuid, count, duration, charges, flags, enchantments, "
            "randomPropertyId, durability, playedTime, text, itemEntry "
            "FROM item_instance WHERE guid = {}", itemGuid);

        if (!result)
        {
            LOG_ERROR("module.hcvault", "[HCVault] Vault item {} has no item_instance row.", itemGuid);
            return nullptr;
        }

        Field* fields = result->Fetch();
        uint32 const entry = fields[11].Get<uint32>();

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto)
        {
            LOG_ERROR("module.hcvault", "[HCVault] Vault item {} is entry {}, which has no template.", itemGuid, entry);
            return nullptr;
        }

        Item* item = NewItemOrBag(proto);

        // Empty owner: the item belongs to the vault, which is not a character. Item::LoadFromDB
        // leaves the owner alone when it is empty, which is the same path the auction house uses.
        if (!item->LoadFromDB(itemGuid, ObjectGuid::Empty, fields, entry))
        {
            delete item;
            LOG_ERROR("module.hcvault", "[HCVault] Vault item {} could not be loaded.", itemGuid);
            return nullptr;
        }

        return item;
    }

    void Stock::Consolidate(std::vector<Item*>& items, CharacterDatabaseTransaction trans)
    {
        if (items.size() < 2)
            return;

        // Every item here came from one stack key, so one template answers for all of them.
        uint32 const maxStack = MergeableStackSize(items.front()->GetEntry());
        if (maxStack == 0)
            return;

        uint64 total = 0;
        for (Item const* item : items)
            total += item->GetCount();

        std::size_t const needed = static_cast<std::size_t>((total + maxStack - 1) / maxStack);
        if (needed >= items.size())
            return;

        // The first `needed` rows are filled to the brim and the rest emptied out. Which rows survive
        // does not matter — the goods are identical, and they are all about to change owner anyway.
        //
        // Both halves of that go in the claim's own transaction, not the mail's. The two commit
        // separately, and a crash in between with the merged-away rows deleted but the survivors still
        // at their old counts would take the difference with it.
        uint64 remaining = total;
        for (std::size_t index = 0; index < items.size(); ++index)
        {
            uint32 const want = index < needed
                ? static_cast<uint32>(std::min<uint64>(maxStack, remaining))
                : 0;

            remaining -= want;

            Item* item = items[index];
            if (item->GetCount() == want)
                continue;

            if (want == 0)
            {
                // SaveToDB deletes the object as well as the row when the state is ITEM_REMOVED, so
                // nothing here may touch `item` again.
                item->FSetState(ITEM_REMOVED);
                item->SaveToDB(trans);
                items[index] = nullptr;
                continue;
            }

            item->SetCount(want);
            item->FSetState(ITEM_CHANGED);
            item->SaveToDB(trans);
        }

        items.erase(std::remove(items.begin(), items.end(), nullptr), items.end());
    }

    void Stock::Forget(StackKey const& key, uint32 itemGuid, CharacterDatabaseTransaction trans)
    {
        auto const itr = _stacks.find(key);
        if (itr != _stacks.end())
        {
            auto& items = itr->second;
            items.erase(std::remove_if(items.begin(), items.end(),
                [itemGuid](StoredItem const& item) { return item.Guid == itemGuid; }), items.end());

            if (items.empty())
                _stacks.erase(itr);
        }

        trans->Append("DELETE FROM mod_hcvault_stock WHERE item_guid = {}", itemGuid);
    }

    bool Stock::Claim(StackKey const& key, uint32 quantity, CharacterDatabaseTransaction trans,
                      std::vector<Item*>& out, std::string& error)
    {
        if (quantity == 0)
        {
            error = "nothing was asked for";
            return false;
        }

        uint32 const held = Available(key);
        if (held < quantity)
        {
            error = "the vault holds " + std::to_string(held) + " of this, not " + std::to_string(quantity);
            return false;
        }

        auto const itr = _stacks.find(key);
        if (itr == _stacks.end())
        {
            error = "the vault holds none of this";
            return false;
        }

        auto const rows = SmallestFirst(itr->second);

        std::vector<Item*> claimed;
        uint32 outstanding = quantity;

        for (auto const& row : rows)
        {
            if (outstanding == 0)
                break;

            Item* item = LoadItem(row.Guid);
            if (!item)
            {
                // The vault's own bookkeeping disagrees with item_instance. Nothing is taken; the
                // stale reference is dropped so the next cycle reports an honest count.
                for (Item* claimedItem : claimed)
                    delete claimedItem;

                Forget(key, row.Guid, trans);
                error = "an item the vault listed no longer exists";
                return false;
            }

            // The count comes from the vault, not from the row that was just read.
            //
            // Writes are queued on the async connections, so an earlier line in this same cycle may
            // have split this row without the UPDATE having landed yet — the SELECT above would then
            // return the count from before the split. Everything else about the item (enchantments,
            // durability, charges) only the row knows; the count only the vault knows, because it is
            // the vault that has been changing it.
            item->SetCount(row.Count);

            if (row.Count <= outstanding)
            {
                // The whole row goes.
                Forget(key, row.Guid, trans);
                outstanding -= row.Count;
                claimed.push_back(item);
                continue;
            }

            // More here than is wanted, so the row is split and the smaller half leaves — the same way
            // round as Player::SplitItem, which keeps the original row (and its history) in place.
            Item* split = item->CloneItem(outstanding, nullptr);
            if (!split)
            {
                for (Item* claimedItem : claimed)
                    delete claimedItem;

                delete item;
                error = "a stack could not be split";
                return false;
            }

            // CloneItem drops the random property when it is given no player, because setting it
            // normally touches that player's update queue. There is no player here and no queue to
            // touch, so it is safe to put back — and it must be, or a suffixed item would arrive
            // plain.
            if (key.SuffixId != 0)
                split->SetItemRandomProperties(key.SuffixId);

            item->SetCount(item->GetCount() - outstanding);
            item->FSetState(ITEM_CHANGED);
            item->SaveToDB(trans);

            split->FSetState(ITEM_NEW);
            split->SaveToDB(trans);

            // The in-memory row shrinks by what left it; it keeps its guid and stays in the vault.
            // Looked up again rather than through `itr`: Forget above erases the map entry when it
            // empties a stack, and a stale iterator here would be a use-after-free waiting for the
            // one order that happens to consume a whole stack and split the next.
            if (auto const stack = _stacks.find(key); stack != _stacks.end())
            {
                for (auto& stored : stack->second)
                    if (stored.Guid == row.Guid)
                        stored.Count -= outstanding;
            }

            outstanding = 0;
            claimed.push_back(split);

            // The source was only borrowed to split it; the row it describes stays behind.
            delete item;
        }

        if (outstanding != 0)
        {
            // Unreachable given the check at the top, but the alternative to saying so is silently
            // mailing somebody less than the order promised.
            for (Item* claimedItem : claimed)
                delete claimedItem;

            error = "only " + std::to_string(quantity - outstanding) + " of " + std::to_string(quantity) + " could be taken";
            return false;
        }

        // Merged only now that the whole line is in hand: the rows it came from are already out of the
        // vault, so this changes nothing anybody else can see, and it is what keeps a line's mail slot
        // count down to what the game itself would use.
        Consolidate(claimed, trans);

        out.insert(out.end(), claimed.begin(), claimed.end());
        return true;
    }
}
