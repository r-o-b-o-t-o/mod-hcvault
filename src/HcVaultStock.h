#ifndef MOD_HCVAULT_STOCK_H
#define MOD_HCVAULT_STOCK_H

#include "DatabaseEnvFwd.h"
#include "Define.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

class Item;

namespace HcVault
{
    /// What makes two piles of goods the same stack: the item, and whatever was rolled onto it.
    /// Two "Bloodwood Wand"s with different suffixes are different stacks and must never merge.
    struct StackKey
    {
        uint32 Entry = 0;

        /// `item_instance.randomPropertyId` — negative for a random suffix, positive for a random
        /// property, 0 for a plain item. Signed, and the website is keyed by the same signed value.
        int32 SuffixId = 0;

        bool operator==(StackKey const& other) const
        {
            return Entry == other.Entry && SuffixId == other.SuffixId;
        }
    };

    struct StackKeyHash
    {
        std::size_t operator()(StackKey const& key) const noexcept
        {
            return (static_cast<std::size_t>(key.Entry) << 32) ^ static_cast<uint32>(key.SuffixId);
        }
    };

    /// One `item_instance` row the vault is holding.
    struct StoredItem
    {
        uint32 Guid = 0;
        uint32 Count = 0;
    };

    /// One line of the snapshot pushed to the website: a whole stack, however many rows it is made of.
    struct StackTotal
    {
        uint32 Entry = 0;
        int32 SuffixId = 0;
        uint32 Quantity = 0;
    };

    /// The vault's contents.
    ///
    /// The goods are real `item_instance` rows that belong to nobody: taken out of the mail and
    /// parked, rather than left sitting in a character's bags where a bank window would run out of
    /// room. `mod_hcvault_stock` is the list of which rows those are.
    ///
    /// Held in memory and written through on every change, so the common case — building the snapshot
    /// to push — never touches the database. Every method must be called on the world thread.
    class Stock
    {
    public:
        /// Reads the table, joining `item_instance` for what each row actually is. Rows whose item has
        /// vanished are dropped, because nothing can be done with a reference to an item that is gone.
        void Load();

        /// Takes an item into the vault. The item must already exist in `item_instance`; its owner is
        /// cleared so no character load can claim it back.
        ///
        /// The stack it joins is repacked, so a donation of five cloth lands in the partial stack
        /// already here rather than beside it. The Item object is never destroyed — `item_instance` is
        /// what changes — so the caller still owns whatever it passed in.
        void Add(Item* item, CharacterDatabaseTransaction trans);

        /// Everything the vault holds, one entry per stack.
        [[nodiscard]] std::vector<StackTotal> Snapshot() const;

        /// How many of one stack are held, across however many rows.
        [[nodiscard]] uint32 Available(StackKey const& key) const;

        /// How many stacks `quantity` of this would arrive as.
        ///
        /// For anything mergeable, the fewest the game allows — `quantity` divided by the item's
        /// maximum stack size, rounded up — because a claim merges whatever rows it takes before
        /// handing them over. What the vault happens to be holding does not come into it: 40 cloth is
        /// two stacks of 20 whether it was donated as two stacks or as eight of five.
        ///
        /// For anything that must not be merged (see MergeableStackSize) it is the number of rows a
        /// claim would walk through, counted the same way Claim consumes them.
        ///
        /// Asked before anything moves, by a caller that has to refuse a line needing more mail slots
        /// than one mail has. Returns 0 when the vault does not hold enough.
        [[nodiscard]] std::size_t SlotsRequired(StackKey const& key, uint32 quantity) const;

        /// Pulls `quantity` of a stack out of the vault, ready to be put in the mail.
        ///
        /// Rows are consumed whole where they fit and split where they do not, exactly as
        /// `Player::SplitItem` does it: the piece that leaves is a clone and the remainder keeps the
        /// original row, so a stack of 20 asked for 5 becomes a 5 in the mail and a 15 still here.
        ///
        /// What leaves is then consolidated into full stacks, so the number of items handed back is
        /// the fewest the game allows rather than however many rows the donations happened to arrive
        /// as. See SlotsRequired, and Consolidate for why that matters.
        ///
        /// On success `out` holds items the caller owns until it hands them to a MailDraft. On failure
        /// no goods have been taken and `error` says why — though a reference to an item that has
        /// vanished is dropped on the way out, since carrying it forward would misreport the stock
        /// forever.
        ///
        /// A successful claim mutates the in-memory vault as well as `trans`, and only the latter can
        /// be abandoned. Callers that might refuse a claim after making it must decide beforehand —
        /// see SlotsRequired.
        bool Claim(StackKey const& key, uint32 quantity, CharacterDatabaseTransaction trans,
                   std::vector<Item*>& out, std::string& error);

        [[nodiscard]] std::size_t StackCount() const { return _stacks.size(); }

        /// Total number of items held, for the log line after a scan.
        [[nodiscard]] uint64 TotalQuantity() const;

    private:
        /// Loads one vault row as a full Item, owned by nobody. Returns nullptr and logs when the
        /// `item_instance` row has gone or its template is unknown.
        static Item* LoadItem(uint32 itemGuid);

        /// Merges claimed items into as few stacks as the item's maximum stack size allows.
        ///
        /// The vault fragments on its own: donations arrive in whatever stacks the donor had, and a
        /// claim takes the smallest rows first. Without this, forty cloth donated as eight fives goes
        /// out as eight separate stacks — and one mail holds twelve items, so a large enough order of
        /// a cheap enough thing becomes undeliverable for no reason the person who placed it could
        /// see.
        ///
        /// Only for items MergeableStackSize allows; everything else is left exactly as it is. Emptied
        /// rows are deleted in `trans` and their Items destroyed, so `items` comes back holding only
        /// what survived.
        static void Consolidate(std::vector<Item*>& items, CharacterDatabaseTransaction trans);

        /// The stack size to merge an item up to, or 0 when it must not be merged at all.
        ///
        /// Merging keeps one row and destroys the other, so it is only safe for an item whose
        /// instances cannot differ in anything but their count. The random-property roll is already
        /// half the stack key, so what this has left to exclude is items that carry state of their own:
        /// anything equippable (enchantments, durability, its maker) except ammo, which carries none,
        /// and anything with a duration ticking down.
        ///
        /// 0 for an unknown template too — merging something the server cannot describe is not a
        /// guess worth making.
        static uint32 MergeableStackSize(uint32 entry);

        /// Merges one stack's rows into the fewest the game allows, in the vault itself.
        ///
        /// Donations arrive in whatever stacks the donor happened to have, so without this the vault
        /// accumulates fives and sevens of things that stack to twenty — which makes the stock table
        /// grow for no reason and costs a row read for every fragment a delivery has to walk through.
        ///
        /// Only `item_instance.count` is written, never a whole row: everything else about an item is
        /// the same across a stack, and a targeted update is what lets a delivery later in the cycle
        /// re-read the row without the two disagreeing about anything but the count, which the vault
        /// is the authority on anyway.
        ///
        /// Returns how many rows it removed. Cheap and safe to call when nothing needs doing.
        std::size_t Repack(StackKey const& key, CharacterDatabaseTransaction trans);

        void Forget(StackKey const& key, uint32 itemGuid, CharacterDatabaseTransaction trans);

        std::unordered_map<StackKey, std::vector<StoredItem>, StackKeyHash> _stacks;
    };
}

#endif // MOD_HCVAULT_STOCK_H
