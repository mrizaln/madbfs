#pragma once

#include <atomic>

namespace adbfsm::tree::util
{
    template <typename... Fs>
    struct Overload : Fs...
    {
        using Fs::operator()...;
    };

    class Lock
    {
    public:
        Lock(std::atomic<bool>& lock)
            : m_lock{ lock }
        {
            auto expect = false;
            while (not m_lock.compare_exchange_strong(expect, true, Ord::acquire, Ord::relaxed)) {
                m_lock.wait(true, Ord::relaxed);
                expect = false;
            }
        }

        ~Lock()
        {
            m_lock.store(false, std::memory_order::release);
            m_lock.notify_one();
        }

    private:
        using Ord = std::memory_order;

        std::atomic<bool>& m_lock;
    };
}
