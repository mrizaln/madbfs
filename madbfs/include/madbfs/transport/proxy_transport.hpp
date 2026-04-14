#pragma once

#include "madbfs/path.hpp"
#include "madbfs/transport/transport.hpp"

namespace madbfs::transport
{
    class ProxyTransport final : public Transport
    {
    public:
        ProxyTransport() = delete;

        ProxyTransport(ProxyTransport&&)            = delete;
        ProxyTransport& operator=(ProxyTransport&&) = delete;

        ProxyTransport(const ProxyTransport&)            = delete;
        ProxyTransport& operator=(const ProxyTransport&) = delete;

        ~ProxyTransport();

        /**
         * @brief Create a new transport using proxy.
         *
         * @param server Server path.
         * @param port The port the server will run on.
         *
         * The construction of the transport may fail like any other.
         */
        static AExpect<Uniq<ProxyTransport>> create(Opt<path::Path> server, u16 port);

        // overrides
        // ---------
        Str  name() const override { return "proxy"; }
        bool running() const override { return m_running; }

        void stop(rpc::Status status) override;

        Await<void>            start() override;
        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req) override;
        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req, Milliseconds timeout) override;

        // ---------

    private:
        struct Process;

        struct Promise
        {
            Vec<u8>&                            buffer;
            saf::promise<Expect<rpc::Response>> result;
        };

        using Inflight = std::unordered_map<rpc::Id, Promise, rpc::Id::Hash>;
        using Channel  = async::Channel<Tup<rpc::Id, rpc::Request>>;

        /**
         * @brief Create a connection using the process and socket.
         *
         * Process may be null. Use the `create()` static member function to create the instance instead.
         */
        ProxyTransport(Uniq<Process> process, rpc::Socket socket);

        /**
         * @brief Generate next id.
         */
        rpc::Id next_id() { return ++m_counter; }    // starts from 1

        /**
         * @brief Detached coroutine for sending requests.
         */
        AExpect<void> request_send();

        /**
         * @brief Detached coroutine for receiving responses.
         */
        AExpect<void> response_receive();

        Uniq<Process> m_process;    // may be null

        rpc::Socket m_socket;
        Channel     m_channel;
        Inflight    m_requests;

        rpc::Id::Inner m_counter = 0;
        bool           m_running = false;
    };
}
