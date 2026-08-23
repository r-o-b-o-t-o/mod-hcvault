#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "HcVaultMgr.h"
#include "IoContext.h"
#include "Log.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "WorldScript.h"

using namespace Acore::ChatCommands;

namespace HcVault::Scripts
{
    /// The io_context only exists once networking starts, so this is the earliest the website client
    /// can be built.
    class HcVaultServerScript : public ServerScript
    {
    public:
        HcVaultServerScript() : ServerScript("HcVaultServerScript", { SERVERHOOK_ON_NETWORK_START }) { }

        void OnNetworkStart(Acore::Asio::IoContext& ioContext) override
        {
            sHcVault->OnNetworkStart(ioContext);
        }
    };

    class HcVaultWorldScript : public WorldScript
    {
    public:
        HcVaultWorldScript() : WorldScript("HcVaultWorldScript",
            {
                WORLDHOOK_ON_AFTER_CONFIG_LOAD,
                WORLDHOOK_ON_STARTUP,
                WORLDHOOK_ON_UPDATE,
                WORLDHOOK_ON_SHUTDOWN,
            })
        {
        }

        void OnAfterConfigLoad(bool reload) override
        {
            sHcVault->LoadConfiguration(reload);
        }

        void OnStartup() override
        {
            sHcVault->OnStartup();
        }

        void OnUpdate(uint32 diff) override
        {
            sHcVault->OnUpdate(diff);
        }

        void OnShutdown() override
        {
            sHcVault->OnShutdown();
        }
    };

    class HcVaultCommandScript : public CommandScript
    {
    public:
        HcVaultCommandScript() : CommandScript("HcVaultCommandScript") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable vaultCommandTable =
            {
                { "status", HandleStatusCommand, SEC_ADMINISTRATOR, Console::Yes },
                { "sync",   HandleSyncCommand,   SEC_ADMINISTRATOR, Console::Yes },
            };

            static ChatCommandTable commandTable =
            {
                { "hcvault", vaultCommandTable }
            };

            return commandTable;
        }

        static bool HandleStatusCommand(ChatHandler* handler)
        {
            Config const& config = sHcVault->GetConfig();

            if (!config.Enabled)
            {
                handler->SendSysMessage("Hardcore Vault: disabled.");
                return true;
            }

            if (std::string const problem = config.Problem(); !problem.empty())
            {
                handler->PSendSysMessage("Hardcore Vault: misconfigured - {}.", problem);
                return true;
            }

            Stock const& stock = sHcVault->GetStock();

            handler->PSendSysMessage("Hardcore Vault: character {}, {}://{}:{}{}, every {}s.",
                config.VaultCharacterGuid, config.UseTls ? "https" : "http", config.Host, config.Port,
                config.BasePath, config.PollInterval);

            if (!config.UseTls)
                handler->SendSysMessage("Plain http: the passkey is sent in clear text on every request.");
            handler->PSendSysMessage("Holding {} item(s) across {} stack(s).",
                stock.TotalQuantity(), stock.StackCount());
            handler->PSendSysMessage("Serving challenge {} ({} match), dead recipients {}.",
                config.AllowedChallengeMask,
                config.ChallengeMatchExact ? "exact" : "at least",
                config.RefuseDeadRecipients ? "refused" : "allowed");

            return true;
        }

        static bool HandleSyncCommand(ChatHandler* handler)
        {
            std::string reason;
            if (!sHcVault->RequestSync(reason))
            {
                handler->PSendSysMessage("Hardcore Vault: cannot sync now - {}.", reason);
                return true;
            }

            handler->SendSysMessage("Hardcore Vault: sync started.");
            return true;
        }
    };

    void AddScripts()
    {
        new HcVaultServerScript();
        new HcVaultWorldScript();
        new HcVaultCommandScript();
    }
}
