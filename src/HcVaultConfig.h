#ifndef MOD_HCVAULT_CONFIG_H
#define MOD_HCVAULT_CONFIG_H

#include "Define.h"

#include <string>

namespace HcVault
{
    /// The mail body used when nothing is configured.
    ///
    /// Named rather than written out twice, so the struct's default and LoadConfig's fallback cannot
    /// drift apart. Real newlines here: what the configuration file spells with a backslash-n arrives
    /// as one of these by way of UnescapeConfigText, which leaves an actual newline alone.
    inline constexpr char const* kDefaultMailBody =
        "Someone made it far enough to give these away. Now they're yours.\n\n"
        "Your bank is somebody else's lifeline. Mail what you don't need to Hcvault, and it'll be "
        "on the site for the next poor soul at 12% health.\n\n"
        "Spread the word and stay alive out there!";

    /// Everything the module reads out of worldserver.conf, resolved once at startup and
    /// re-resolved on `.reload config`.
    struct Config
    {
        bool Enabled = false;

        /// Low GUID of the one character donations are mailed to and orders are sent from.
        /// Zero disables the module: there is no sensible default and guessing one would have it
        /// emptying a stranger's mailbox.
        uint32 VaultCharacterGuid = 0;

        /// Host part of the website, without scheme or path (e.g. "vault.example.com").
        std::string Host;

        /// Port as a string, because that is what the resolver takes.
        std::string Port = "443";

        /// Whether to speak TLS, taken from the scheme of the configured URL. See AllowInsecureHttp.
        bool UseTls = true;

        /// Path the API is mounted under, always starting with '/' and never ending with one.
        /// "" when the API sits at the root.
        std::string BasePath;

        /// Shared key presented as `Authorization: Bearer …`.
        std::string Passkey;

        /// Whether the server's certificate is verified against the system trust store. Leaving this
        /// off makes the connection encrypted but not authenticated — anyone able to answer for the
        /// host can read the passkey and hand back orders of their own choosing. Irrelevant when the
        /// URL is plain http, where there is no certificate to check.
        bool VerifyCertificate = true;

        /// Permits an `http://` website URL.
        ///
        /// Off by default and separate from the URL itself, so plain HTTP is always a deliberate
        /// second step. The passkey travels on every request and authenticates calls that can empty
        /// the vault and mail its contents anywhere: over http it is readable by anything on the path,
        /// and replayable by anything that reads it.
        ///
        /// It exists for a local debug setup, where the whole conversation stays on one machine.
        bool AllowInsecureHttp = false;

        /// Seconds between cycles. One cycle is: read the mailbox, push the stock, ask for work, do
        /// it, report back.
        uint32 PollInterval = 60;

        /// Seconds before an HTTP request is abandoned.
        uint32 HttpTimeout = 15;

        /// Whether request bodies are gzipped before they are sent.
        ///
        /// The stock push is the only payload big enough to matter and it is repetitive JSON, so it
        /// goes out at about a ninth of its size. The cost is a fraction of a millisecond on a network
        /// thread — never on the world thread — so the only reason to turn this off is something
        /// between the module and the website that mishandles `Content-Encoding` on a request.
        bool CompressRequests = true;

        /// Database and table holding the challenge-mode rows. Both are identifiers, not values, so
        /// they cannot be bound as parameters — they are validated instead. See Delivery.cpp.
        std::string ChallengeDatabase = "ac_eluna";
        std::string ChallengeTable = "challenge_modes_character";

        /// The `challenge` bitmask an order's recipient must be running.
        uint32 AllowedChallengeMask = 1;

        /// When true the recipient's mask must equal AllowedChallengeMask exactly; when false it need
        /// only contain those bits. Exact by default, because the server only lets characters trade
        /// with others running the identical set of challenges — a superset cannot receive the mail.
        bool ChallengeMatchExact = true;

        /// Refuse to deliver to a character the challenge table marks as dead.
        bool RefuseDeadRecipients = true;

        /// Body of the mail an order is delivered in.
        ///
        /// The subject is not configurable: it carries the order reference the requester was shown
        /// when they placed it, which is the one thing that lets a parcel in game and a card in the
        /// backoffice be matched up. See HcVault::MailSubjectFor.
        std::string MailBody = kDefaultMailBody;

        /// True once everything the module cannot run without is present.
        [[nodiscard]] bool IsUsable() const;

        /// Why it is not usable, for a single clear line in the log. Empty when it is.
        [[nodiscard]] std::string Problem() const;
    };

    /// Reads the configuration. Never throws; anything malformed falls back to the default and is
    /// reported by Problem().
    Config LoadConfig();

    /// Splits "https://host:8443/vault" into host, port and base path, and says whether the scheme
    /// asked for TLS. Both http and https parse; whether http is *allowed* is a separate question,
    /// answered by Config::Problem.
    ///
    /// Returns false when the URL is not usable, leaving the outputs untouched.
    bool ParseWebsiteUrl(std::string const& url, bool& useTls, std::string& host, std::string& port,
                         std::string& basePath);

    /// Turns the escape sequences a configuration file can carry into the characters they stand for.
    ///
    /// The core's config reader hands back whatever is between the quotes, byte for byte, so a body
    /// written with a backslash-n arrives as those two characters and is mailed out as those two
    /// characters. Anyone typing one plainly means a line break.
    ///
    /// Understands backslash n, r and t, and a doubled backslash for a literal one. Anything else is
    /// left exactly as written — a backslash that starts no sequence this knows is far more likely to
    /// be part of the text than a mistake worth swallowing.
    std::string UnescapeConfigText(std::string const& value);

    /// The subject an order's mail is sent under: "Hardcore Vault Order #237b142d".
    ///
    /// `reference` is the short order reference the website hands out — the first eight characters of
    /// the order's public id, exactly as the cart prints it back to whoever placed the order. An
    /// empty one drops the number rather than mailing a stray '#', which is what an older website
    /// that does not send the field would produce.
    std::string MailSubjectFor(std::string const& reference);

    /// True when the string is safe to paste into SQL as an identifier: letters, digits and
    /// underscores only. Database and table names cannot be bound as parameters, so this is what
    /// stands between a typo in the config and a syntax error — or worse — at query time.
    bool IsSafeSqlIdentifier(std::string const& identifier);
}

#endif // MOD_HCVAULT_CONFIG_H
