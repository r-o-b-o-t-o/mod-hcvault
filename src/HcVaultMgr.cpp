#include "HcVaultMgr.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "GameTime.h"
#include "HcVaultDelivery.h"
#include "HcVaultMailbox.h"
#include "IoContext.h"
#include "Log.h"
#include "net/HcVaultHttpClient.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"

#include <boost/asio/strand.hpp>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <ctime>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace HcVault
{
    namespace
    {
        constexpr char const* kStockPath = "/api/module/stock";
        constexpr char const* kMessagesPath = "/api/module/messages";
        constexpr char const* kWorkPath = "/api/module/work";
        constexpr char const* kResultsPath = "/api/module/results";

        /// The first cycle waits this long after startup, so the character cache and the rest of the
        /// world are settled before anything is mailed.
        constexpr uint32 kStartupDelayMs = 15 * 1000;

        /// The website reads timestamps as ISO 8601; the game counts Unix seconds.
        std::string IsoTime(uint32 unixSeconds)
        {
            std::time_t const seconds = static_cast<std::time_t>(unixSeconds);
            std::tm parts{};

#ifdef _WIN32
            gmtime_s(&parts, &seconds);
#else
            gmtime_r(&seconds, &parts);
#endif

            char buffer[32];
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
            return buffer;
        }

        std::string IsoNow()
        {
            return IsoTime(static_cast<uint32>(GameTime::GetGameTime().count()));
        }

        /// Trims a response body to something a log line can carry.
        std::string Excerpt(std::string const& body, std::size_t limit = 200)
        {
            if (body.size() <= limit)
                return body;

            return body.substr(0, limit) + "...";
        }
    }

    Mgr* Mgr::instance()
    {
        static Mgr instance;
        return &instance;
    }

    void Mgr::LoadConfiguration(bool reload)
    {
        _config = LoadConfig();
        _usable = false;

        // A reload can change the host, the passkey or the timeouts, none of which an existing client
        // would pick up. Dropped here and rebuilt below; requests already in flight hold their own
        // reference and finish on the old one.
        if (reload && _http)
        {
            _http->Stop();
            _http.reset();
        }

        if (!_config.Enabled)
        {
            LOG_INFO("module.hcvault", "[HCVault] Disabled (HcVault.Enabled = 0).");
            DiscardCycle();
            return;
        }

        if (std::string const problem = _config.Problem(); !problem.empty())
        {
            LOG_ERROR("module.hcvault", "[HCVault] Enabled but not usable: {}. The module will do nothing.", problem);
            DiscardCycle();
            return;
        }

        _usable = true;

        LOG_INFO("module.hcvault",
            "[HCVault] Enabled. Vault character {}, website {}://{}:{}{}, every {}s, serving challenge {} ({}).",
            _config.VaultCharacterGuid, _config.UseTls ? "https" : "http", _config.Host, _config.Port,
            _config.BasePath, _config.PollInterval,
            _config.AllowedChallengeMask, _config.ChallengeMatchExact ? "exact" : "at least");

        // Null at startup, because the configuration is read before networking starts; OnNetworkStart
        // builds the first client. On a reload it is set, and this is what puts the client back.
        EnsureClient();
    }

    void Mgr::EnsureClient()
    {
        if (_http || !_usable || !_ioContext)
            return;

        // Its own strand: the worldserver runs the io_context on several threads, and every handler
        // in a cycle has to be serialised against the others.
        _http = Net::HttpClient::Create(boost::asio::make_strand(_ioContext->get_executor()), _config);

        LOG_INFO("module.hcvault", "[HCVault] Website client ready.");
    }

    void Mgr::OnNetworkStart(Acore::Asio::IoContext& ioContext)
    {
        _ioContext = &ioContext;
        EnsureClient();
    }

    void Mgr::OnStartup()
    {
        if (!_usable)
            return;

        _stock.Load();
        _ready = true;

        // Counted down from the startup delay rather than from zero, so the first cycle runs shortly
        // after the world opens instead of a full interval later.
        uint32 const interval = _config.PollInterval * 1000;
        _sinceLastCycle = interval > kStartupDelayMs ? interval - kStartupDelayMs : 0;
    }

    void Mgr::OnShutdown()
    {
        if (_http)
            _http->Stop();

        _ready = false;
        DiscardCycle();
    }

    bool Mgr::IsRunnable() const
    {
        return _usable && _ready && _http != nullptr;
    }

    bool Mgr::VaultCharacterIsOnline() const
    {
        return ObjectAccessor::FindPlayerByLowGUID(_config.VaultCharacterGuid) != nullptr;
    }

    bool Mgr::RequestSync(std::string& reason)
    {
        if (!IsRunnable())
        {
            reason = _config.Enabled ? _config.Problem() : "the module is disabled";
            if (reason.empty())
                reason = "the module is not ready yet";

            return false;
        }

        if (_busy)
        {
            reason = "a cycle is already running";
            return false;
        }

        if (VaultCharacterIsOnline())
        {
            reason = "the vault character is online";
            return false;
        }

        StartCycle();
        return true;
    }

    void Mgr::OnUpdate(uint32 diff)
    {
        if (!IsRunnable())
            return;

        // Whatever the website asked for last cycle, acted on here because it moves items and mail.
        std::optional<std::string> work;
        {
            std::lock_guard lock(_workMutex);
            work.swap(_work);
        }

        if (work)
            HandleWork(*work);

        if (_busy)
        {
            // A cycle that never ends is worse than one that fails: `_busy` gates everything, so the
            // module would go quiet — no mail collected, no orders delivered — and say nothing but
            // "a cycle is already running" to anybody who asked, until the next restart.
            //
            // Every request is bounded and every completion path reports, so reaching this means a
            // handler was lost rather than merely slow. The cycle is abandoned; anything it did that
            // matters is already recorded, and the delivery log makes a repeat harmless.
            _cycleElapsed += diff;
            if (_cycleElapsed >= CycleWatchdogMs())
            {
                LOG_ERROR("module.hcvault",
                    "[HCVault] A cycle has been running for {}s with no answer; abandoning it. The next "
                    "one starts on schedule.", _cycleElapsed / 1000);

                EndCycle();
            }

            return;
        }

        _sinceLastCycle += diff;
        if (_sinceLastCycle < _config.PollInterval * 1000)
            return;

        _sinceLastCycle = 0;

        // Nothing is safe to touch while the character is logged in: it holds its mail and its purse
        // in memory and writes both back when it saves. The cycle is simply skipped, and the next one
        // catches up — the stock push is a full picture every time, so nothing accumulates.
        if (VaultCharacterIsOnline())
        {
            LOG_DEBUG("module.hcvault", "[HCVault] Vault character is online; skipping this cycle.");
            return;
        }

        StartCycle();
    }

    void Mgr::StartCycle()
    {
        _busy = true;
        _sinceLastCycle = 0;
        _cycleElapsed = 0;

        // Read before anything this cycle writes, which is the only moment the table and this figure
        // are guaranteed to agree: from here on the cycle keeps its own count. The vault character is
        // offline — that is checked before a cycle starts — so nothing else moves its money.
        _vaultCopper = ReadVaultMoney(_config.VaultCharacterGuid);

        // Read before the scan writes anything, so this cycle forwards what earlier ones collected and
        // nothing half-written. A letter collected below therefore goes out on the next cycle — which
        // is what the buffer table is for, and a minute on somebody's thank-you note.
        std::string letters;
        std::string letterIds;
        bool const haveLetters = BuildLetterPayload(letters, letterIds);

        ScanResult const scan = CollectDonations(_config.VaultCharacterGuid, _stock);
        _vaultCopper += scan.CopperCollected;

        // Copied once, here on the world thread, and carried through the chain. `_http` itself is
        // replaced by a configuration reload — also on the world thread — and every hop after this one
        // runs on a network thread, where reading the member would be a race and finding it null a
        // crash.
        PushStock(_http, haveLetters ? std::move(letters) : std::string(),
                  haveLetters ? std::move(letterIds) : std::string());
    }

    uint32 Mgr::CycleWatchdogMs() const
    {
        // A cycle makes at most five requests one after another, so the bound has to clear that with
        // room to spare or a merely slow website would trip it. Well short of the poll interval's
        // usefulness, and far beyond anything healthy.
        return (_config.HttpTimeout * 6 + 30) * 1000;
    }

    void Mgr::EndCycle()
    {
        _busy = false;
    }

    void Mgr::DiscardCycle()
    {
        // Work parked for a world thread that will now refuse to run it. OnUpdate returns before it
        // reads this when the module is not runnable, so without clearing it here the answer — and the
        // `_busy` that goes with it — would sit there until the configuration was fixed, and then be
        // acted on as though it had just arrived.
        {
            std::lock_guard lock(_workMutex);
            _work.reset();
        }

        EndCycle();
    }

    std::string Mgr::BuildStockPayload() const
    {
        json payload;
        payload["capturedAt"] = IsoNow();
        payload["copper"] = _vaultCopper;

        json entries = json::array();
        for (StackTotal const& total : _stock.Snapshot())
        {
            entries.push_back({
                { "itemId", total.Entry },
                { "suffixId", total.SuffixId },
                { "quantity", total.Quantity },
            });
        }

        payload["entries"] = std::move(entries);
        return payload.dump();
    }

    void Mgr::PushStock(std::shared_ptr<Net::HttpClient> http, std::string letters, std::string letterIds)
    {
        LOG_DEBUG("module.hcvault", "[HCVault] Pushing {} stack(s).", _stock.StackCount());

        http->Post(kStockPath, BuildStockPayload(),
            [this, http, letters = std::move(letters), letterIds = std::move(letterIds)]
            (Net::Response response) mutable
        {
            if (!response.Ok)
            {
                // The push is a complete picture every cycle, so a failed one costs nothing but
                // freshness. The rest of the cycle carries on: approved orders are still worth
                // delivering even when the site's view of the shelves is a minute old.
                LOG_WARN("module.hcvault", "[HCVault] Stock push failed: {}", response.Error);
            }

            PushLetters(std::move(http), std::move(letters), std::move(letterIds));
        });
    }

    bool Mgr::BuildLetterPayload(std::string& payload, std::string& mailIds) const
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT mail_id, sender, subject, body, money, sent_at FROM mod_hcvault_letter "
            "ORDER BY mail_id LIMIT 100");

        if (!result)
            return false;

        json messages = json::array();
        std::string ids;

        // Indexed by mail id so the attachments below can be dropped in without a query each.
        std::unordered_map<uint32, std::size_t> position;

        do
        {
            Field* fields = result->Fetch();
            uint32 const mailId = fields[0].Get<uint32>();

            position[mailId] = messages.size();
            messages.push_back({
                { "mailId", mailId },
                { "sender", fields[1].Get<std::string>() },
                { "subject", fields[2].Get<std::string>() },
                { "body", fields[3].Get<std::string>() },
                { "copper", fields[4].Get<uint32>() },
                { "sentAt", IsoTime(fields[5].Get<uint32>()) },
                { "items", json::array() },
            });

            if (!ids.empty())
                ids += ',';

            ids += std::to_string(mailId);
        } while (result->NextRow());

        // What came in the same mail, so the note can be read against what it is about. One query for
        // the whole batch, restricted to the letters actually going out — the table can hold more than
        // the hundred this cycle takes.
        if (QueryResult items = CharacterDatabase.Query(
                "SELECT mail_id, item_entry, suffix_id, count FROM mod_hcvault_letter_item "
                "WHERE mail_id IN ({})", ids))
        {
            do
            {
                Field* fields = items->Fetch();

                auto const itr = position.find(fields[0].Get<uint32>());
                if (itr == position.end())
                    continue;

                messages[itr->second]["items"].push_back({
                    { "itemId", fields[1].Get<uint32>() },
                    { "suffixId", fields[2].Get<int32>() },
                    { "quantity", fields[3].Get<uint32>() },
                });
            } while (items->NextRow());
        }

        json document;
        document["messages"] = std::move(messages);

        payload = document.dump();
        mailIds = std::move(ids);
        return true;
    }

    void Mgr::PushLetters(std::shared_ptr<Net::HttpClient> http, std::string payload, std::string mailIds)
    {
        // Nothing buffered when the cycle started. Read there rather than here because this runs on a
        // network thread, and CharacterDatabase.Query spins looking for a free synchronous connection
        // before it blocks on MySQL — neither of which belongs on an io_context thread.
        if (mailIds.empty())
        {
            FetchWork(std::move(http));
            return;
        }

        http->Post(kMessagesPath, std::move(payload),
            [this, http, mailIds = std::move(mailIds)](Net::Response response) mutable
        {
            if (response.Ok)
            {
                // Cleared only once the website has them. The mail they came from is long gone, so
                // these rows are the only copy until this succeeds.
                //
                // Execute rather than Query: it hands the statement to the async worker and returns,
                // so it costs this thread nothing.
                //
                // The attachment rows go with them: the foreign key cascades, so the two can never be
                // left disagreeing about what arrived.
                CharacterDatabase.Execute("DELETE FROM mod_hcvault_letter WHERE mail_id IN ({})", mailIds);
            }
            else
            {
                LOG_WARN("module.hcvault", "[HCVault] Letter push failed, keeping them for the next cycle: {}",
                    response.Error);
            }

            FetchWork(std::move(http));
        });
    }

    void Mgr::FetchWork(std::shared_ptr<Net::HttpClient> http)
    {
        http->Get(kWorkPath, [this](Net::Response response)
        {
            if (!response.Ok)
            {
                LOG_WARN("module.hcvault", "[HCVault] Could not fetch work: {}", response.Error);
                EndCycle();
                return;
            }

            // Parked for the world thread. Everything it asks for moves items or mail, and neither is
            // safe to touch from here.
            {
                std::lock_guard lock(_workMutex);
                _work = std::move(response.Body);
            }
        });
    }

    void Mgr::HandleWork(std::string const& body)
    {
        json work;
        try
        {
            work = json::parse(body);
        }
        catch (json::exception const& e)
        {
            LOG_ERROR("module.hcvault", "[HCVault] The website's work list did not parse: {} — {}",
                e.what(), Excerpt(body));
            EndCycle();
            return;
        }

        // The vault character can log in between the cycle starting and the answer arriving, which is
        // the moment it becomes unsafe to mail anything out of its purse.
        if (VaultCharacterIsOnline())
        {
            LOG_INFO("module.hcvault", "[HCVault] Vault character came online mid-cycle; the work waits for the next one.");
            EndCycle();
            return;
        }

        // Taken once, on this thread, and used for both posts below — the same reason StartCycle takes
        // one. Non-null because OnUpdate checked IsRunnable before handing the work over, and nothing
        // between there and here runs on the world thread.
        std::shared_ptr<Net::HttpClient> http = _http;

        json deliveryResults = json::array();
        json recipientResults = json::array();

        // The cycle's own running figure, decremented as money goes out, so two orders in one cycle
        // cannot each see the full purse and together overdraw it.
        uint64& availableCopper = _vaultCopper;

        for (json const& entry : work.value("deliveries", json::array()))
        {
            Delivery delivery;
            delivery.OrderId = entry.value("orderId", 0);
            delivery.Reference = entry.value("reference", std::string());
            delivery.Recipient = entry.value("recipient", std::string());
            delivery.Copper = entry.value("copper", 0ULL);

            for (json const& line : entry.value("items", json::array()))
            {
                DeliveryLine item;
                item.LineId = line.value("lineId", 0);
                item.ItemId = line.value("itemId", 0U);
                item.SuffixId = line.value("suffixId", 0);
                item.Quantity = line.value("quantity", 0U);
                delivery.Items.push_back(item);
            }

            if (delivery.OrderId == 0 || delivery.Recipient.empty())
            {
                LOG_WARN("module.hcvault", "[HCVault] Skipping a delivery with no order id or recipient.");
                continue;
            }

            for (DeliveryOutcome const& outcome : DeliverOrder(_config, _stock, delivery, availableCopper))
            {
                json result;
                // Null rather than 0 for the money: the website's line ids start at 1, and a nullable
                // field is how it tells the two apart.
                result["lineId"] = outcome.IsMoney ? json(nullptr) : json(outcome.LineId);
                result["orderId"] = outcome.OrderId;
                result["delivered"] = outcome.Delivered;

                if (!outcome.Reason.empty())
                    result["reason"] = outcome.Reason;

                deliveryResults.push_back(std::move(result));
            }
        }

        for (json const& entry : work.value("recipients", json::array()))
        {
            RecipientRequest request;
            request.OrderId = entry.value("orderId", 0);
            request.Recipient = entry.value("recipient", std::string());

            if (request.OrderId == 0 || request.Recipient.empty())
                continue;

            RecipientInfo const info = DescribeRecipient(_config, request);

            recipientResults.push_back({
                { "orderId", info.OrderId },
                { "exists", info.Exists },
                { "level", info.HasLevel ? json(info.Level) : json(nullptr) },
                { "challenge", info.HasChallenge ? json(info.Challenge) : json(nullptr) },
                { "eligible", info.Eligible },
                { "dead", info.Dead },
            });
        }

        if (deliveryResults.empty() && recipientResults.empty())
        {
            EndCycle();
            return;
        }

        // The vault changed, so the website's mirror is now a cycle behind. Pushed again rather than
        // left until the next cycle, so what the public page offers matches what is on the shelves.
        bool const stockChanged = !deliveryResults.empty();

        json payload;
        payload["deliveries"] = std::move(deliveryResults);
        payload["recipients"] = std::move(recipientResults);

        PushResults(http, payload.dump());

        if (stockChanged)
        {
            // Built here on the world thread, where the stock may be read, and posted from the
            // callback thread as a finished string.
            http->Post(kStockPath, BuildStockPayload(), [](Net::Response response)
            {
                if (!response.Ok)
                    LOG_WARN("module.hcvault", "[HCVault] Post-delivery stock push failed: {}", response.Error);
            });
        }
    }

    void Mgr::PushResults(std::shared_ptr<Net::HttpClient> http, std::string body)
    {
        http->Post(kResultsPath, std::move(body), [this](Net::Response response)
        {
            if (!response.Ok)
            {
                // The lines that were mailed are recorded in mod_hcvault_delivery, so the website will
                // offer them again and they will be reported delivered without being resent.
                LOG_WARN("module.hcvault", "[HCVault] Could not report results, they will be repeated next cycle: {}",
                    response.Error);
            }
            else
            {
                LOG_DEBUG("module.hcvault", "[HCVault] Results accepted: {}", Excerpt(response.Body));
            }

            EndCycle();
        });
    }
}
