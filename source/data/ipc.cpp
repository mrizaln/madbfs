#include "adbfsm/data/ipc.hpp"
#include "adbfsm/log.hpp"

#include <simdjson.h>

namespace
{

}

namespace adbfsm::data
{
    Expect<Ipc> Ipc::create()
    {
        auto socket_path = [] -> String {
            const auto* res = std::getenv("XDG_RUNTIME_DIR");
            return res ? res : "/tmp";
        }();

        const auto* serial = std::getenv("ANDROID_SERIAL");
        if (serial == nullptr) {
            return Unexpect{ Errc::no_such_device };
        }

        socket_path += '/';
        socket_path += serial;

        auto acc = std::make_unique<sockpp::unix_acceptor>();
        if (auto res = acc->open(sockpp::unix_address{ socket_path })) {
            return Unexpect{ static_cast<Errc>(res.error().value()) };
        }

        auto path = path::create_buf(std::move(socket_path));
        if (not path.has_value()) {
            return Unexpect{ Errc::bad_address };
        }

        return Ipc{ std::move(*path), std::move(acc) };
    }

    void Ipc::run(std::stop_token st, std::move_only_function<void(ipc::Op op)>)
    {
        m_threadpool->run();

        while (not st.stop_requested()) {
            if (auto res = m_socket->accept(); !res) {
                log_e({ "{}: socket accept failed {}" }, __func__, res.error_message());
                continue;
            }

            m_threadpool->enqueue_detached([=] {

            });
        }

        m_threadpool->stop(true);
    }
}
