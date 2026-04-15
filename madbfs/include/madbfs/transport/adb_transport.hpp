#pragma once

#include "madbfs/transport/transport.hpp"

namespace madbfs::transport
{
    class AdbTransport final : public Transport
    {
    public:
        /**
         * @brief Create a new adb transport.
         *
         * @param exec Async io excecutor.
         */
        AdbTransport(net::any_io_executor exec)
            : m_in_channel{ exec }
            , m_out_channel{ exec }
            , m_pool{ 1 }
        {
        }

        AdbTransport(AdbTransport&&)            = delete;
        AdbTransport& operator=(AdbTransport&&) = delete;

        AdbTransport(const AdbTransport&)            = delete;
        AdbTransport& operator=(const AdbTransport&) = delete;

        ~AdbTransport();

        // overrides
        // ---------
        Str  name() const override { return "adb"; }
        bool running() const override { return m_running; }

        void stop(rpc::Status status) override;

        Await<void>            start() override;
        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req) override;
        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req, Milliseconds timeout) override;
        // ---------

    private:
        struct Promise
        {
            Vec<u8>&                            buffer;
            rpc::Procedure                      proc;
            saf::promise<Expect<rpc::Response>> result;
        };

        using Inflight   = std::unordered_map<rpc::Id, Promise, rpc::Id::Hash>;
        using InChannel  = async::Channel<Tup<rpc::Id, rpc::Request>>;
        using OutChannel = async::Channel<Tup<rpc::Id, Expect<rpc::Response>>>;

        /**
         * @brief Generate next id.
         */
        rpc::Id next_id() { return ++m_counter; }    // starts from 1

        /**
         * @brief Detached coroutine for dispatching requests.
         */
        AExpect<void> request_dispatch();

        /**
         * @brief Detached coroutine for receiving responses.
         */
        AExpect<void> response_receive();

        InChannel  m_in_channel;
        OutChannel m_out_channel;
        Inflight   m_requests;

        net::thread_pool m_pool;

        rpc::Id::Inner m_counter = 0;
        bool           m_running = false;
    };

}
