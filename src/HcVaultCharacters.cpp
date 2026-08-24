#include "HcVaultCharacters.h"

#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "HcVaultConfig.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "QueryResult.h"

#include <algorithm>
#include <cctype>

namespace HcVault
{
    namespace
    {
        /// Guids per batched query. The same figure the mailbox uses to read attachments, and for the
        /// same reason: far below anything MySQL minds, far above any mailbox this will meet.
        constexpr std::size_t kGuidQueryChunk = 500;

        /// The columns every challenge-modes read selects, in the order ApplyChallengeFields expects.
        ///
        /// In one place because that function reads them by index. A batched read appends `guid`
        /// after them, which disturbs nothing.
        constexpr char const* kChallengeColumns = "name, class, level, challenge, dead";

        /// Merges one challenge-modes row into a character, filling only what is not already known.
        ///
        /// The cache is the better authority on name, class and level for a character the realm still
        /// has; this row is the only authority on the challenge and how it ended.
        void ApplyChallengeFields(ResolvedCharacter& character, Field* fields)
        {
            if (character.Name.empty())
                character.Name = fields[0].Get<std::string>();

            if (!character.HasClass)
            {
                character.Class = fields[1].Get<uint8>();
                character.HasClass = character.Class != 0;
            }

            if (!character.HasLevel)
            {
                character.Level = fields[2].Get<uint8>();
                character.HasLevel = character.Level != 0;
            }

            character.HasChallenge = true;
            character.Challenge = fields[3].Get<int8>();
            character.Dead = fields[4].Get<uint8>() != 0;
        }

        /// Fills in whatever the character cache holds for a guid, which may be nothing.
        void ApplyCacheEntry(ResolvedCharacter& character, uint32 guid)
        {
            CharacterCacheEntry const* cached =
                sCharacterCache->GetCharacterCacheByGuid(ObjectGuid(HighGuid::Player, guid));

            if (!cached)
                return;

            // A soft-deleted character stays in the cache with its name blanked, which is exactly how
            // to tell one apart from a character the realm still has.
            character.Found = true;
            character.Live = !cached->Name.empty();
            character.Name = cached->Name;
            character.Class = cached->Class;
            character.HasClass = cached->Class != 0;
            character.Level = cached->Level;
            character.HasLevel = cached->Level != 0;
        }

        /// True when anything at all was found out about a character.
        bool WasFound(ResolvedCharacter const& character)
        {
            return character.Found || character.HasChallenge || !character.Name.empty();
        }
    }

    bool IsPlausibleCharacterName(std::string const& name)
    {
        // Twelve characters is the game's limit and utf8mb4 spends up to four bytes on each, so the
        // bound is 48 bytes. Counted in bytes deliberately: the job here is to refuse something
        // absurd, not to count characters, and a byte bound cannot be fooled by an encoding.
        //
        // 12 would have been the obvious number and the wrong one — a name of twelve accented letters
        // is 24 bytes, and rejecting it means reporting a living character as one that never existed.
        if (name.empty() || name.size() > 48)
            return false;

        // Letters, plus every byte a multi-byte character is made of.
        //
        // The bytes matter: an ASCII-only test throws out accented and Cyrillic names, which are not
        // exotic — a realm this was measured against has some three thousand of them, and rejecting
        // one means reporting a living character as one that never existed.
        //
        // What this excludes is quotes, backslashes, wildcards, digits, whitespace and control
        // characters: everything the game will not put in a name and a query would rather not see.
        return std::all_of(name.begin(), name.end(), [](unsigned char c)
        {
            return c >= 0x80 || std::isalpha(c) != 0;
        });
    }

    ResolvedCharacter ResolveByGuid(Config const& config, uint32 guid)
    {
        ResolvedCharacter character;
        if (guid == 0)
            return character;

        character.Guid = guid;
        ApplyCacheEntry(character, guid);

        // Read whether or not the cache answered: the challenge state is only ever here, and for a
        // character the realm has purged this is the only record of any kind.
        //
        // Database and table are identifiers and cannot be bound; they are validated when the
        // configuration loads and the module refuses to run otherwise. See Config::Problem.
        if (QueryResult result = CharacterDatabase.Query(
                "SELECT {} FROM `{}`.`{}` WHERE guid = {}",
                kChallengeColumns, config.ChallengeDatabase, config.ChallengeTable, guid))
        {
            ApplyChallengeFields(character, result->Fetch());
        }

        character.Found = WasFound(character);
        return character;
    }

    std::unordered_map<uint32, ResolvedCharacter> ResolveByGuids(
        Config const& config, std::vector<uint32> const& guids)
    {
        std::unordered_map<uint32, ResolvedCharacter> resolved;
        if (guids.empty())
            return resolved;

        resolved.reserve(guids.size());

        // The cache first, and for every guid before a single row is read: it is a map in memory, so
        // asking it costs nothing and it answers for most characters outright.
        for (uint32 const guid : guids)
        {
            if (guid == 0)
                continue;

            auto const [itr, inserted] = resolved.try_emplace(guid);
            if (!inserted)
                continue;

            itr->second.Guid = guid;
            ApplyCacheEntry(itr->second, guid);
        }

        std::vector<uint32> distinct;
        distinct.reserve(resolved.size());
        for (auto const& entry : resolved)
            distinct.push_back(entry.first);

        for (std::size_t start = 0; start < distinct.size(); start += kGuidQueryChunk)
        {
            std::size_t const stop = std::min(distinct.size(), start + kGuidQueryChunk);

            std::string list;
            for (std::size_t index = start; index < stop; ++index)
            {
                if (index != start)
                    list += ',';

                list += std::to_string(distinct[index]);
            }

            // guid last, so ApplyChallengeFields keeps reading fields 0 to 4 by index.
            QueryResult result = CharacterDatabase.Query(
                "SELECT {}, guid FROM `{}`.`{}` WHERE guid IN ({})",
                kChallengeColumns, config.ChallengeDatabase, config.ChallengeTable, list);

            if (!result)
                continue;

            do
            {
                Field* fields = result->Fetch();
                if (auto const itr = resolved.find(fields[5].Get<uint32>()); itr != resolved.end())
                    ApplyChallengeFields(itr->second, fields);
            } while (result->NextRow());
        }

        for (auto& entry : resolved)
            entry.second.Found = WasFound(entry.second);

        return resolved;
    }

    ResolvedCharacter ResolveByName(Config const& config, std::string const& name)
    {
        ResolvedCharacter character;
        if (name.empty())
            return character;

        // The living character first, so a name whose previous holder died resolves to whoever wears
        // it now rather than to the grave.
        //
        // Asked before the name is judged, and deliberately: the cache takes a name as a value and
        // will answer for whatever the realm actually holds, however it came to be called that. A
        // character the realm has is a character this must find.
        if (ObjectGuid const guid = sCharacterCache->GetCharacterGuidByName(name))
        {
            character = ResolveByGuid(config, guid.GetCounter());
            character.Live = true;
            if (character.Name.empty())
                character.Name = name;

            return character;
        }

        // Only now does the shape of the name matter, because only from here does it go into a
        // statement built by hand. Refusing an implausible one costs nothing — the realm has already
        // said it has no such character — and it keeps anything the game would not have created out of
        // a query, whatever the escaping would have done with it.
        if (!IsPlausibleCharacterName(name))
            return character;

        // The only thing left that might answer is the challenge table's memory. Highest guid wins:
        // guids are handed out in order, so that is the most recent character to have worn the name,
        // and a name reused ten times must not resolve to the first.
        //
        // The whole row, not just the guid: reading the guid and then fetching the rest by it was two
        // round trips for one row, on the one path already slow enough to warrant an index.
        std::string escaped = name;
        CharacterDatabase.EscapeString(escaped);

        QueryResult result = CharacterDatabase.Query(
            "SELECT {}, guid FROM `{}`.`{}` WHERE name = '{}' ORDER BY guid DESC LIMIT 1",
            kChallengeColumns, config.ChallengeDatabase, config.ChallengeTable, escaped);

        if (!result)
            return character;

        Field* fields = result->Fetch();

        // Cache before row, the order ResolveByGuid uses: for a character that was merely renamed the
        // realm still has it, and what it says about class and level is the fresher answer.
        character.Guid = fields[5].Get<uint32>();
        ApplyCacheEntry(character, character.Guid);
        ApplyChallengeFields(character, fields);

        character.Found = WasFound(character);

        // Whatever the cache had to say about a guid the realm no longer lists, it is not live.
        character.Live = false;
        if (character.Name.empty())
            character.Name = name;

        LOG_DEBUG("module.hcvault",
            "[HCVault] {} is not on the realm; the challenge table remembers character {} (dead: {}).",
            name, character.Guid, character.Dead);

        return character;
    }
}
