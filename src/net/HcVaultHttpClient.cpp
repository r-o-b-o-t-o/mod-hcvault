#include "net/HcVaultHttpClient.h"

#include "HcVaultConfig.h"
#include "Log.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/bind_handler.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/version.hpp>

#include <chrono>
#include <climits>
#include <cstddef>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <utility>
#include <zlib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincrypt.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "crypt32.lib")
#endif
#endif

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace HcVault::Net
{
    namespace
    {
        /// zlib's default. Measured on a 13 KB stock push: level 1 takes 0.09 ms for 13.1% of the
        /// original, level 6 takes 0.15 ms for 11.1%, level 9 takes 0.32 ms for 10.3%. Level 6 is the
        /// only one of the three that is not obviously the wrong trade.
        constexpr int kCompressionLevel = 6;

        /// Below this a body is sent as it is.
        ///
        /// gzip costs 18 bytes of header and trailer before it compresses anything, and the small
        /// bodies here — a work poll, a handful of results — are not what the traffic is made of. The
        /// stock push is, and it is an order of magnitude above this.
        constexpr std::size_t kMinCompressBytes = 1024;

        /// Compresses a body to gzip. Returns false and leaves `out` untouched if it cannot, which the
        /// caller treats as a reason to send the body uncompressed rather than as an error.
        ///
        /// gzip rather than deflate: HTTP's `deflate` is famously ambiguous — some servers want the
        /// zlib wrapper around it and some want the raw stream — while gzip means one thing
        /// everywhere. That is what the 15 + 16 window asks deflateInit2 for; zlib's own compress2
        /// would produce the zlib wrapper instead, which is the thing nobody agrees about.
        bool GzipCompress(std::string const& input, std::string& out)
        {
            // zlib counts in uInt. Bodies here are kilobytes, but the cast is not one to make blind.
            if (input.size() > UINT_MAX)
                return false;

            z_stream stream{};
            if (deflateInit2(&stream, kCompressionLevel, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
                return false;

            std::string buffer;
            buffer.resize(deflateBound(&stream, static_cast<uLong>(input.size())));

            // const_cast because zlib's next_in predates const-correctness; deflate does not write
            // through it.
            stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
            stream.avail_in = static_cast<uInt>(input.size());
            stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
            stream.avail_out = static_cast<uInt>(buffer.size());

            // One shot: the whole body is in hand and the output buffer is deflateBound-sized, so
            // Z_FINISH cannot come up short.
            int const result = deflate(&stream, Z_FINISH);
            uLong const produced = stream.total_out;
            deflateEnd(&stream);

            if (result != Z_STREAM_END)
                return false;

            buffer.resize(produced);
            out = std::move(buffer);
            return true;
        }

        /// Fills the SSL context with the system's trusted roots.
        ///
        /// On Windows OpenSSL's default verify paths are usually empty, which would fail every
        /// handshake against a perfectly valid certificate, so the certificates are imported from the
        /// Windows "ROOT" store instead.
        void LoadRootCertificates(ssl::context& ctx)
        {
#ifdef _WIN32
            HCERTSTORE store = CertOpenSystemStoreA(0, "ROOT");
            if (!store)
            {
                ctx.set_default_verify_paths();
                return;
            }

            X509_STORE* sslStore = X509_STORE_new();
            PCCERT_CONTEXT cert = nullptr;
            while ((cert = CertEnumCertificatesInStore(store, cert)))
            {
                unsigned char const* encoded = cert->pbCertEncoded;
                if (X509* x509 = d2i_X509(nullptr, &encoded, static_cast<long>(cert->cbCertEncoded)))
                {
                    X509_STORE_add_cert(sslStore, x509);
                    X509_free(x509);
                }
            }
            CertCloseStore(store, 0);

            // The context takes ownership of the store.
            SSL_CTX_set_cert_store(ctx.native_handle(), sslStore);
#else
            ctx.set_default_verify_paths();
#endif
        }
    }

    /// One request, from resolve to the handler call. Keeps itself alive through the chain by
    /// capturing shared_from_this in every completion handler.
    class HttpClient::Session : public std::enable_shared_from_this<HttpClient::Session>
    {
    public:
        Session(net::any_io_executor executor,
                std::shared_ptr<ssl::context> sslContext,
                std::string host,
                std::string port,
                unsigned timeoutSeconds,
                ResponseHandler handler)
            : _resolver(executor)
            , _resolveTimer(executor)
            , _sslContext(std::move(sslContext))
            , _stream(MakeStream(executor, _sslContext))
            , _host(std::move(host))
            , _port(std::move(port))
            , _timeout(std::chrono::seconds(timeoutSeconds))
            , _handler(std::move(handler))
        {
        }

        void Run(http::request<http::string_body> request)
        {
            _request = std::move(request);

            if (TlsStream* tls = std::get_if<TlsStream>(&_stream))
            {
                // Without SNI a shared host answers with the wrong certificate and the handshake fails
                // for reasons that look nothing like the cause.
                if (!SSL_set_tlsext_host_name(tls->native_handle(), _host.c_str()))
                {
                    Finish({ false, 0, {}, "failed to set the TLS server name" });
                    return;
                }
            }

            // The one operation with no deadline of its own: beast's tcp_stream timer covers connect,
            // write and read, but the resolver is a separate object it knows nothing about. Without
            // this a name lookup that never answers leaves the handler uncalled, and the cycle that
            // is waiting on it never ends.
            _resolveTimer.expires_after(_timeout);
            _resolveTimer.async_wait([self = shared_from_this()](beast::error_code ec)
            {
                // Cancelled because the lookup answered in time, which is the ordinary case.
                if (ec == net::error::operation_aborted)
                    return;

                // Completes the resolve with operation_aborted, which lands in OnResolve as a failure
                // and takes the session down the same path as any other.
                self->_resolver.cancel();
            });

            _resolver.async_resolve(_host, _port,
                beast::bind_front_handler(&Session::OnResolve, shared_from_this()));
        }

    private:
        using PlainStream = beast::tcp_stream;
        using TlsStream = beast::ssl_stream<beast::tcp_stream>;

        /// One session type for both schemes, rather than two near-identical classes that would drift
        /// apart in exactly the parts — the timeouts, the single Finish call — that have to match.
        using Stream = std::variant<PlainStream, TlsStream>;

        static Stream MakeStream(net::any_io_executor& executor, std::shared_ptr<ssl::context> const& sslContext)
        {
            // A null context is how the client says the website is plain http.
            if (sslContext)
                return Stream(std::in_place_type<TlsStream>, executor, *sslContext);

            return Stream(std::in_place_type<PlainStream>, executor);
        }

        /// Runs `fn` against whichever stream this session holds.
        template<typename F>
        decltype(auto) Visit(F&& fn)
        {
            return std::visit(std::forward<F>(fn), _stream);
        }

        /// The TCP layer underneath, which is where connecting and the timeouts live either way.
        beast::tcp_stream& Socket()
        {
            return Visit([](auto& stream) -> beast::tcp_stream& { return beast::get_lowest_layer(stream); });
        }

        void OnResolve(beast::error_code ec, tcp::resolver::results_type results)
        {
            // Whether it answered or was cancelled, the deadline has done its job. Harmless if the
            // timer has already fired: its handler then finds a resolver with nothing outstanding.
            _resolveTimer.cancel();

            if (ec)
            {
                Fail(ec, "resolve");
                return;
            }

            Socket().expires_after(_timeout);
            Socket().async_connect(results,
                beast::bind_front_handler(&Session::OnConnect, shared_from_this()));
        }

        void OnConnect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
        {
            if (ec)
            {
                Fail(ec, "connect");
                return;
            }

            Socket().expires_after(_timeout);

            TlsStream* tls = std::get_if<TlsStream>(&_stream);
            if (!tls)
            {
                // Plain http: there is nothing to negotiate, so connecting *is* the handshake.
                OnHandshake({});
                return;
            }

            tls->async_handshake(ssl::stream_base::client,
                beast::bind_front_handler(&Session::OnHandshake, shared_from_this()));
        }

        void OnHandshake(beast::error_code ec)
        {
            if (ec)
            {
                Fail(ec, "TLS handshake");
                return;
            }

            Socket().expires_after(_timeout);
            Visit([this](auto& stream)
            {
                http::async_write(stream, _request,
                    beast::bind_front_handler(&Session::OnWrite, shared_from_this()));
            });
        }

        void OnWrite(beast::error_code ec, std::size_t)
        {
            if (ec)
            {
                Fail(ec, "write");
                return;
            }

            Visit([this](auto& stream)
            {
                http::async_read(stream, _buffer, _response,
                    beast::bind_front_handler(&Session::OnRead, shared_from_this()));
            });
        }

        void OnRead(beast::error_code ec, std::size_t)
        {
            if (ec)
            {
                Fail(ec, "read");
                return;
            }

            auto const status = static_cast<unsigned>(_response.result_int());
            Response result;
            result.Status = status;
            result.Body = _response.body();
            result.Ok = status >= 200 && status < 300;

            if (!result.Ok)
                result.Error = "the website answered " + std::to_string(status);

            // The answer is in hand; the shutdown that follows is courtesy, and its outcome must not
            // change what the caller is told.
            Finish(std::move(result));

            TlsStream* tls = std::get_if<TlsStream>(&_stream);
            if (!tls)
            {
                // Nothing to negotiate down; closing the socket is the whole of it.
                OnShutdown({});
                return;
            }

            Socket().expires_after(std::chrono::seconds(5));
            tls->async_shutdown(beast::bind_front_handler(&Session::OnShutdown, shared_from_this()));
        }

        void OnShutdown(beast::error_code)
        {
            // Servers routinely drop the connection rather than completing the TLS shutdown, and
            // there is nothing useful to do about it either way.
            beast::error_code ignored;
            Socket().socket().close(ignored);
        }

        void Fail(beast::error_code ec, char const* what)
        {
            Finish({ false, 0, {}, std::string(what) + ": " + ec.message() });

            beast::error_code ignored;
            Socket().socket().close(ignored);
        }

        /// Calls the handler exactly once, however the session ends.
        void Finish(Response response)
        {
            if (!_handler)
                return;

            auto handler = std::exchange(_handler, nullptr);
            handler(std::move(response));
        }

        tcp::resolver _resolver;

        /// Bounds the name lookup, which nothing else does. See Run.
        net::steady_timer _resolveTimer;

        // Declared before _stream: the stream is built from it in the member initialiser list, and
        // members are initialised in declaration order whatever that list says.
        std::shared_ptr<ssl::context> _sslContext;

        Stream _stream;
        beast::flat_buffer _buffer;
        http::request<http::string_body> _request;
        http::response<http::string_body> _response;
        std::string _host;
        std::string _port;
        std::chrono::seconds _timeout;
        ResponseHandler _handler;
    };

    std::shared_ptr<HttpClient> HttpClient::Create(net::any_io_executor executor, Config const& config)
    {
        return std::shared_ptr<HttpClient>(new HttpClient(std::move(executor), config));
    }

    HttpClient::HttpClient(net::any_io_executor executor, Config const& config)
        : _executor(std::move(executor))
        , _sslContext(config.UseTls
            ? std::make_shared<ssl::context>(ssl::context::tlsv12_client)
            : nullptr)
        , _host(config.Host)
        , _port(config.Port)
        , _basePath(config.BasePath)
        , _passkey(config.Passkey)
        , _timeoutSeconds(config.HttpTimeout)
        , _compress(config.CompressRequests)
        , _stopped(false)
    {
        if (!_sslContext)
        {
            // Said once a boot rather than once a request, but said every boot: this is the setting
            // most likely to be turned on for an afternoon of debugging and left on.
            LOG_WARN("module.hcvault",
                "[HCVault] Talking to {} over plain http. The passkey is sent in clear text on every request, and "
                "it is the only thing authenticating calls that can empty the vault. Local debugging "
                "only.",
                _host);
            return;
        }

        if (config.VerifyCertificate)
        {
            _sslContext->set_verify_mode(ssl::verify_peer);
            // Checking the chain without checking the name would accept any valid certificate for any
            // host, which is most of the way to no verification at all.
            _sslContext->set_verify_callback(ssl::host_name_verification(_host));
            LoadRootCertificates(*_sslContext);
        }
        else
        {
            _sslContext->set_verify_mode(ssl::verify_none);
            LOG_WARN("module.hcvault",
                "[HCVault] Certificate verification is off. The connection is encrypted but not authenticated: "
                "anything able to answer for {} can read the passkey and hand back orders of its own.",
                _host);
        }
    }

    void HttpClient::Stop()
    {
        _stopped = true;
    }

    void HttpClient::Get(std::string path, ResponseHandler handler)
    {
        Request(http::verb::get, std::move(path), {}, std::move(handler));
    }

    void HttpClient::Post(std::string path, std::string body, ResponseHandler handler)
    {
        Request(http::verb::post, std::move(path), std::move(body), std::move(handler));
    }

    void HttpClient::Request(http::verb method, std::string path, std::string body, ResponseHandler handler)
    {
        if (_stopped)
        {
            if (handler)
                handler({ false, 0, {}, "the module is shutting down" });
            return;
        }

        // Hopped onto the executor so the session is constructed and driven on the strand, whichever
        // thread asked for the request.
        net::dispatch(_executor,
            [self = shared_from_this(), method, path = std::move(path), body = std::move(body),
             handler = std::move(handler)]() mutable
            {
                http::request<http::string_body> request{ method, self->_basePath + path, 11 };
                request.set(http::field::host, self->_host);
                request.set(http::field::user_agent, "mod-hcvault/" BOOST_BEAST_VERSION_STRING);
                request.set(http::field::accept, "application/json");

                // One connection per request, and this says so. Without it the server holds the socket
                // open expecting a reuse that is never coming, because the session closes as soon as it
                // has read the response.
                request.set(http::field::connection, "close");
                request.set(http::field::authorization, "Bearer " + self->_passkey);

                if (!body.empty())
                {
                    request.set(http::field::content_type, "application/json");

                    // Compressed here rather than where the payload was built. This lambda runs on the
                    // strand — a network thread — while BuildStockPayload runs on the world thread, and
                    // the world thread has better things to do with a millisecond than deflate.
                    //
                    // A failure falls through to sending the body as it is. Compression is an
                    // optimisation, and giving up on a push because it could not be made smaller would
                    // be a poor trade.
                    std::string compressed;
                    if (self->_compress && body.size() >= kMinCompressBytes && GzipCompress(body, compressed))
                    {
                        request.set(http::field::content_encoding, "gzip");
                        request.body() = std::move(compressed);
                    }
                    else
                    {
                        request.body() = std::move(body);
                    }
                }

                request.prepare_payload();

                auto session = std::make_shared<Session>(
                    self->_executor, self->_sslContext, self->_host, self->_port,
                    self->_timeoutSeconds, std::move(handler));

                session->Run(std::move(request));
            });
    }
}
