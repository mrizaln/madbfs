#pragma once

#include <madbfs-common/aliases.hpp>
#include <madbfs-common/rpc.hpp>

namespace madbfs::transport
{
    class Transport
    {
    public:
        virtual ~Transport() = default;

        /**
         * @brief Get transport name.
         */
        virtual Str name() const = 0;

        /**
         * @brief check if transport is running.
         */
        virtual bool running() const = 0;

        /**
         * @brief Cancel all operation then stop and close the client sender/receiver channel and the socket.
         *
         * @param status Status to be set on cancelled operations.
         */
        virtual void stop(rpc::Status status) = 0;

        /**
         * @brief Start the transport.
         *
         * The transport may need to initialize its own coroutine detached from subsequent caller of other
         * functions. This function provides a way to do just that.
         */
        virtual Await<void> start() = 0;

        /**
         * @brief Send request through the transport.
         *
         * @param buffer Buffer used for response.
         * @param req The requested procedure.
         *
         * @return The returned response or error.
         *
         * Since the buffer will be used for result for this coroutine, make sure it lives until the coroutine
         * complete.
         */
        virtual AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req) = 0;

        /**
         * @brief Send request through the transport.
         *
         * @param buffer Buffer used for response.
         * @param req The requested procedure.
         * @param timeout Timeout before operation cancelled.
         *
         * @return The returned response or error.
         *
         * Since the buffer will be used for result for this coroutine, make sure it lives until the coroutine
         * complete.
         */
        virtual AExpect<rpc::Response> send(Vec<u8>& buffer, rpc::Request req, Milliseconds timeout) = 0;

        /**
         * @brief Request send wrapper.
         *
         * @param buf Data buffer.
         * @param req Operation request.
         *
         * This function typecheck the returned response variant from `send()` to match the corresponding
         * request. Use this instead of `send()`
         */
        template <rpc::IsRequest Req>
        AExpect<rpc::ToResp<Req>> send_req(Vec<u8>& buf, Req req)
        {
            if (auto res = co_await send(buf, std::move(req)); not res) {
                co_return Unexpect{ res.error() };
            } else if (auto resp = std::get_if<rpc::ToResp<Req>>(&*res); resp != nullptr) {
                co_return std::move(*resp);
            }
            co_return Unexpect{ Errc::bad_message };
        }

        /**
         * @brief Request send wrapper.
         *
         * @param buf Data buffer.
         * @param req Operation request.
         * @param timeout Timeout for the operation before cancelled.
         *
         * This function typecheck the returned response variant from `send()` to match the corresponding
         * request. Use this instead of `send()`
         */
        template <rpc::IsRequest Req>
        AExpect<rpc::ToResp<Req>> send_req(Vec<u8>& buf, Req req, Milliseconds timeout)
        {
            if (auto res = co_await send(buf, std::move(req), timeout); not res) {
                co_return Unexpect{ res.error() };
            } else if (auto resp = std::get_if<rpc::ToResp<Req>>(&*res); resp != nullptr) {
                co_return std::move(*resp);
            }
            co_return Unexpect{ Errc::bad_message };
        }
    };
}
