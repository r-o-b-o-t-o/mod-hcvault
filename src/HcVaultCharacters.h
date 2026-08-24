#ifndef MOD_HCVAULT_CHARACTERS_H
#define MOD_HCVAULT_CHARACTERS_H

#include "Define.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace HcVault
{
    struct Config;

    /// What the realm knows about a character, whether or not it still has one.
    ///
    /// A hardcore death deletes the character, so the realm forgets it and the character cache
    /// stops answering to its name. The challenge-modes table does not: it keeps a row per character
    /// with the name, class, level and how it ended, long after the `characters` row has been purged.
    /// That table is therefore the memory this reads when the realm has none.
    struct ResolvedCharacter
    {
        /// False only when no character by this name has ever existed — a typo, in other words.
        bool Found = false;

        uint32 Guid = 0;

        /// The realm still has this character, so mail can reach it.
        ///
        /// False for one only the challenge table remembers. Nothing may be delivered to such a
        /// character however healthy its record looks: the mail would be written to a mailbox nobody
        /// can open.
        bool Live = false;

        std::string Name;

        bool HasLevel = false;
        uint8 Level = 0;

        bool HasClass = false;
        uint8 Class = 0;

        /// False when the character runs no challenge at all, which is not the same as running the
        /// wrong one.
        bool HasChallenge = false;
        int32 Challenge = 0;

        bool Dead = false;
    };

    /// Resolves the character that currently bears a name, or last bore it.
    ///
    /// The living character wins, always. Names are reused heavily — one on this realm has been worn
    /// by over a hundred characters — so a name whose previous holder died and whose current holder
    /// is alive and well must resolve to the living one. Asking the character cache first is what
    /// guarantees that; only when the realm has no such character does the challenge table's memory
    /// come into it, and then the most recent record wins.
    ///
    /// Must run on the world thread.
    ResolvedCharacter ResolveByName(Config const& config, std::string const& name);

    /// The same, for a character already identified by guid — a letter's sender, say, where there is
    /// no name to disambiguate and none needed.
    ResolvedCharacter ResolveByGuid(Config const& config, uint32 guid);

    /// The same again, for a whole batch, in one query rather than one per guid.
    ///
    /// A mailbox holding a hundred letters is a hundred synchronous round trips on the world thread
    /// done the obvious way, which is why the attachment reads next door are batched too. Duplicate
    /// and zero guids are dropped; the result is keyed by guid and holds an entry for every distinct
    /// one asked for, whether or not anything was found for it.
    ///
    /// Must run on the world thread.
    std::unordered_map<uint32, ResolvedCharacter> ResolveByGuids(
        Config const& config, std::vector<uint32> const& guids);

    /// True when a string is safe to compare against a character name in SQL: letters only, and no
    /// longer than the game allows.
    ///
    /// This guards the hand-built query and nothing else. The character cache is asked first and
    /// without it, because the cache takes a name as a value: whatever the realm holds, it answers
    /// for, and a character that exists must never be reported as one that does not.
    bool IsPlausibleCharacterName(std::string const& name);
}

#endif // MOD_HCVAULT_CHARACTERS_H
