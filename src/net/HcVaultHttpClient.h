#ifndef MOD_HCVAULT_NET_HTTP_CLIENT_H
#define MOD_HCVAULT_NET_HTTP_CLIENT_H

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/beast/http/verb.hpp>
#include <functional>
#include <memory>
#include <string>

namespace HcVault
{
    struct Config;
}

namespace HcVault::Net
{
    /// The outcome of one request. `Ok` means the website answered with a 2xx; anything else — a
    /// refused connection, a failed handshake, a 500 — arrives with `Error` set and is treated the
    /// same way by the caller, which simply tries again next cycle.
    struct Response
    {
        bool Ok = false;
        unsigned Status = 0;
        std::string Body;
        std::string Error;
    };

    using ResponseHandler = std::function<void(Response)>;

    /// A small HTTP/HTTPS client for talking JSON to the vault website.
    ///
    /// One connection per request, deliberately: the module makes a handful of calls a minute, and a
    /// pooled connection to a server that may restart under it is more failure modes than the
    /// handshake costs. Every request is authenticated with the configured passkey and bounded by a
    /// timeout, so nothing can wedge the cycle.
    ///
    /// TLS is decided by the configured URL's scheme. Plain http exists for a local debug setup and
    /// has to be asked for twice — see Config::AllowInsecureHttp.
    ///
    /// Request() is safe to call from any thread; the handler runs on the executor, which is expected
    /// to be a strand.
    class HttpClient : public std::enable_shared_from_this<HttpClient>
    {
    public:
        static std::shared_ptr<HttpClient> Create(boost::asio::any_io_executor executor, Config const& config);

        HttpClient(HttpClient const&) = delete;
        HttpClient& operator=(HttpClient const&) = delete;

        /// Sends one request. `path` is appended to the configured base path. `body` is sent as
        /// application/json and may be empty for a GET. The handler is invoked exactly once.
        void Request(boost::beast::http::verb method, std::string path, std::string body, ResponseHandler handler);

        void Get(std::string path, ResponseHandler handler);
        void Post(std::string path, std::string body, ResponseHandler handler);

        /// Stops accepting new requests. In-flight ones still complete, so their handlers are not
        /// left dangling half way through a cycle.
        void Stop();

    private:
        HttpClient(boost::asio::any_io_executor executor, Config const& config);

        class Session;

        boost::asio::any_io_executor _executor;

        /// Null when the website is plain http, which is also how a session knows not to handshake.
        std::shared_ptr<boost::asio::ssl::context> _sslContext;
        std::string _host;
        std::string _port;
        std::string _basePath;
        std::string _passkey;
        unsigned _timeoutSeconds;

        /// Whether request bodies are gzipped. See Config::CompressRequests.
        bool _compress;

        std::atomic<bool> _stopped;
    };
}

#endif // MOD_HCVAULT_NET_HTTP_CLIENT_H
