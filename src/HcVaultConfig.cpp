#include "HcVaultConfig.h"

#include "Config.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace HcVault
{
    bool IsSafeSqlIdentifier(std::string const& identifier)
    {
        if (identifier.empty() || identifier.size() > 64)
            return false;

        return std::all_of(identifier.begin(), identifier.end(), [](unsigned char c)
        {
            return std::isalnum(c) || c == '_';
        });
    }

    std::string UnescapeConfigText(std::string const& value)
    {
        std::string out;
        out.reserve(value.size());

        for (std::size_t index = 0; index < value.size(); ++index)
        {
            char const current = value[index];

            // A trailing backslash stands for itself; there is nothing after it to escape.
            if (current != '\\' || index + 1 == value.size())
            {
                out += current;
                continue;
            }

            switch (value[index + 1])
            {
                case 'n':  out += '\n'; ++index; break;
                case 'r':  out += '\r'; ++index; break;
                case 't':  out += '\t'; ++index; break;
                case '\\': out += '\\'; ++index; break;

                // Not a sequence this knows, so both characters stay exactly as they were written.
                default: out += current; break;
            }
        }

        return out;
    }

    std::string MailSubjectFor(std::string const& reference)
    {
        // The mail's subject is the only place the reference appears in game, and it is what somebody
        // asking "did my order arrive?" will read out. Kept identical to what the website shows so the
        // two can be compared character for character.
        if (reference.empty())
            return "Hardcore Vault Order";

        return "Hardcore Vault Order #" + reference;
    }

    bool ParseWebsiteUrl(std::string const& url, bool& useTls, std::string& host, std::string& port,
                         std::string& basePath)
    {
        std::string rest = url;

        // The scheme decides TLS and the default port; whether plain http is *permitted* is decided by
        // Config::AllowInsecureHttp, so that a URL and a decision to run without TLS stay two separate
        // things rather than one typed slip.
        bool parsedTls;
        if (rest.rfind("https://", 0) == 0)
        {
            parsedTls = true;
            rest.erase(0, 8);
        }
        else if (rest.rfind("http://", 0) == 0)
        {
            parsedTls = false;
            rest.erase(0, 7);
        }
        else
        {
            return false;
        }

        if (rest.empty())
            return false;

        std::string parsedPath;
        if (auto const slash = rest.find('/'); slash != std::string::npos)
        {
            parsedPath = rest.substr(slash);
            rest.erase(slash);
        }

        // Trailing slashes would produce "//api/module/stock" once a path is appended.
        while (!parsedPath.empty() && parsedPath.back() == '/')
            parsedPath.pop_back();

        std::string parsedPort = parsedTls ? "443" : "80";
        if (auto const colon = rest.rfind(':'); colon != std::string::npos)
        {
            parsedPort = rest.substr(colon + 1);
            rest.erase(colon);

            if (parsedPort.empty() || !std::all_of(parsedPort.begin(), parsedPort.end(),
                [](unsigned char c) { return std::isdigit(c); }))
                return false;
        }

        if (rest.empty())
            return false;

        useTls = parsedTls;
        host = rest;
        port = parsedPort;
        basePath = parsedPath;
        return true;
    }

    bool Config::IsUsable() const
    {
        return Problem().empty();
    }

    std::string Config::Problem() const
    {
        if (VaultCharacterGuid == 0)
            return "HcVault.VaultCharacterGuid is 0; set it to the low GUID of the vault character";

        if (Host.empty())
            return "HcVault.Website.Url is missing or is not an http:// or https:// URL";

        if (!UseTls && !AllowInsecureHttp)
        {
            return "HcVault.Website.Url is http://, which sends the passkey in clear text. Set "
                   "HcVault.Website.AllowInsecureHttp = 1 if that is what you meant (local debugging), "
                   "or use https://";
        }

        if (Passkey.empty())
            return "HcVault.Website.Passkey is empty; the website refuses every unauthenticated call";

        if (!IsSafeSqlIdentifier(ChallengeDatabase))
            return "HcVault.ChallengeDatabase is not a plain identifier (letters, digits, underscores)";

        if (!IsSafeSqlIdentifier(ChallengeTable))
            return "HcVault.ChallengeTable is not a plain identifier (letters, digits, underscores)";

        if (AllowedChallengeMask == 0)
            return "HcVault.AllowedChallengeMask is 0, which no character can ever satisfy";

        return {};
    }

    Config LoadConfig()
    {
        Config config;

        config.Enabled = sConfigMgr->GetOption<bool>("HcVault.Enabled", false);
        config.VaultCharacterGuid = sConfigMgr->GetOption<uint32>("HcVault.VaultCharacterGuid", 0);
        config.Passkey = sConfigMgr->GetOption<std::string>("HcVault.Website.Passkey", "");
        config.VerifyCertificate = sConfigMgr->GetOption<bool>("HcVault.Website.VerifyCertificate", true);
        config.AllowInsecureHttp = sConfigMgr->GetOption<bool>("HcVault.Website.AllowInsecureHttp", false);

        // A minute is the shortest cycle worth running: every one of them reads the mailbox and talks
        // to the website, and a donation is not more urgent than that.
        config.PollInterval = std::max<uint32>(30, sConfigMgr->GetOption<uint32>("HcVault.PollInterval", 60));
        config.HttpTimeout = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("HcVault.HttpTimeout", 15), 3, 120);
        config.CompressRequests = sConfigMgr->GetOption<bool>("HcVault.Website.Compress", true);

        config.ChallengeDatabase = sConfigMgr->GetOption<std::string>("HcVault.ChallengeDatabase", "ac_eluna");
        config.ChallengeTable = sConfigMgr->GetOption<std::string>("HcVault.ChallengeTable", "challenge_modes_character");
        config.AllowedChallengeMask = sConfigMgr->GetOption<uint32>("HcVault.AllowedChallengeMask", 1);
        config.ChallengeMatchExact = sConfigMgr->GetOption<bool>("HcVault.ChallengeMatchExact", true);
        config.RefuseDeadRecipients = sConfigMgr->GetOption<bool>("HcVault.RefuseDeadRecipients", true);

        // Unescaped, so a body written across several lines in the config arrives as several lines in
        // the mailbox. Nothing else read here is prose, and nothing else is unescaped.
        config.MailBody = UnescapeConfigText(
            sConfigMgr->GetOption<std::string>("HcVault.Mail.Body", kDefaultMailBody));

        auto const url = sConfigMgr->GetOption<std::string>("HcVault.Website.Url", "");
        if (!url.empty())
            ParseWebsiteUrl(url, config.UseTls, config.Host, config.Port, config.BasePath);

        return config;
    }
}
