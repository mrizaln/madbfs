#pragma once

#include "madbfs/transport/transport.hpp"

namespace madbfs::transport
{
    /**
     * @class NullTransport
     * @brief Dummy tranport that does nothing.
     */
    class NullTransport final : public Transport
    {
    public:
        /**
         * @brief Construct a new dummy transport.
         *
         * @param errc Error code to return on send operation.
         */
        NullTransport(Errc errc)
            : m_errc{ errc }
        {
        }

        Str  name() const override { return "null"; }
        bool running() const override { return true; }

        void        stop(rpc::Status) override { }
        Await<void> start() override { co_return; }

        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req) override
        {
            co_return Unexpect{ m_errc };
        }

        AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req, Milliseconds timeout) override
        {
            co_return Unexpect{ m_errc };
        }

    private:
        Errc m_errc;
    };
}
