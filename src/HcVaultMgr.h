#ifndef MOD_HCVAULT_MGR_H
#define MOD_HCVAULT_MGR_H

#include "Define.h"
#include "HcVaultConfig.h"
#include "HcVaultStock.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace Acore::Asio
{
    class IoContext;
}

namespace HcVault::Net
{
    class HttpClient;
}

namespace HcVault
{
    /// Drives the whole module.
    ///
    /// One cycle, on a timer:
    ///   1. empty the vault character's mailbox into the vault  (world thread)
    ///   2. push the whole stock and the money to the website    (network thread)
    ///   3. forward any letters that came with donations         (network thread)
    ///   4. ask what to deliver and who to describe              (network thread)
    ///   5. do it                                                (world thread)
    ///   6. report what came of it                               (network thread)
    ///
    /// Steps that touch the game run on the world thread, driven from OnUpdate; steps that talk to
    /// the website run on the io_context. The only thing crossing between them is the answer to
    /// step 4, which is parked under a mutex and picked up on the next world tick.
    ///
    /// The whole cycle is skipped while the vault character is online: a logged-in player holds its
    /// mail and its purse in memory and writes them back on save, which would undo anything done
    /// underneath it.
    class Mgr
    {
    public:
        static Mgr* instance();

        /// Called once the worldserver's io_context exists. Networking cannot be set up before this.
        void OnNetworkStart(Acore::Asio::IoContext& ioContext);

        /// Reads the configuration. Called at startup and on `.reload config`.
        void LoadConfiguration(bool reload);

        /// Reads the vault into memory. Called once the world is up.
        void OnStartup();

        /// Drives the cycle. Called on every world tick, so it must stay cheap when there is nothing
        /// to do.
        void OnUpdate(uint32 diff);

        void OnShutdown();

        [[nodiscard]] Config const& GetConfig() const { return _config; }

        [[nodiscard]] Stock const& GetStock() const { return _stock; }

        /// Runs a cycle now rather than waiting for the timer, for the `.hcvault sync` command.
        /// Returns false with `reason` set when it cannot start.
        bool RequestSync(std::string& reason);

    private:
        Mgr() = default;

        /// True when the module is configured, enabled and has a client.
        [[nodiscard]] bool IsRunnable() const;

        /// Builds the website client if the configuration is usable and there is an io_context to
        /// build it on. Safe to call twice; does nothing when a client already exists.
        void EnsureClient();

        /// True when the vault character is logged in, which is the one thing that stops a cycle.
        [[nodiscard]] bool VaultCharacterIsOnline() const;

        void StartCycle();

        /// Forgets whatever cycle is in flight: any answer parked for the world thread, and the flag
        /// that says one is running. For when the module stops being able to act on either.
        void DiscardCycle();

        /// How long a cycle may run before it is presumed wedged. See OnUpdate.
        [[nodiscard]] uint32 CycleWatchdogMs() const;

        /// The complete stock and the vault character's purse, as the website wants it. Must be
        /// called on the world thread; the result is a finished string safe to post from anywhere.
        [[nodiscard]] std::string BuildStockPayload() const;

        /// Reads the buffered donation letters and builds the push for them.
        ///
        /// Must be called on the world thread: it reads the database, and a synchronous read from a
        /// network thread spins on GetFreeConnection before blocking on MySQL. Returns false when
        /// there is nothing to send, leaving both outputs untouched.
        [[nodiscard]] bool BuildLetterPayload(std::string& payload, std::string& mailIds) const;

        // The client is passed down the chain rather than read from `_http` at each hop. Every one of
        // these after the first runs on a network thread, while `_http` is replaced on the world
        // thread by a configuration reload — so a cycle finishes on the client it started with, and
        // no network thread ever touches the member.
        void PushStock(std::shared_ptr<Net::HttpClient> http, std::string letters, std::string letterIds);
        void PushLetters(std::shared_ptr<Net::HttpClient> http, std::string payload, std::string mailIds);
        void FetchWork(std::shared_ptr<Net::HttpClient> http);
        void HandleWork(std::string const& body);
        void PushResults(std::shared_ptr<Net::HttpClient> http, std::string body);
        void EndCycle();

        Config _config;

        /// Config::Problem() rebuilt on every world tick would be a string comparison per frame for an
        /// answer that only changes when the configuration is reloaded.
        bool _usable = false;

        Stock _stock;

        /// What the vault character is carrying, in copper, as of this cycle.
        ///
        /// Read from the table once at the start of a cycle and then kept by hand: collections add to
        /// it, deliveries take from it.
        ///
        /// It cannot simply be re-read when the push is built, because every write a cycle makes is
        /// queued on the async connections and may not have landed yet. A fresh SELECT would return
        /// the figure the cycle started with, and the push would undo the cycle's own bookkeeping.
        uint64 _vaultCopper = 0;

        /// The worldserver's io_context, kept so the client can be rebuilt after a config reload. It
        /// outlives this object; nothing here owns it.
        Acore::Asio::IoContext* _ioContext = nullptr;

        std::shared_ptr<Net::HttpClient> _http;

        /// Set while a cycle is in flight, so the timer cannot start a second one on top.
        std::atomic<bool> _busy{ false };

        /// Milliseconds since the last cycle started.
        uint32 _sinceLastCycle = 0;

        /// Milliseconds the current cycle has been running, for the watchdog in OnUpdate. Only
        /// meaningful while `_busy`.
        uint32 _cycleElapsed = 0;

        /// Set by the world tick when the vault is loaded; nothing runs before it.
        bool _ready = false;

        /// The answer to step 4, handed from the network thread to the world thread.
        std::mutex _workMutex;
        std::optional<std::string> _work;
    };
}

#define sHcVault HcVault::Mgr::instance()

#endif // MOD_HCVAULT_MGR_H
