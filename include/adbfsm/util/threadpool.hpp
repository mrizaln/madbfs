#pragma once

#include <cassert>
#include <deque>
#include <thread>
#include <version>

#if not defined(__cpp_lib_move_only_function)
#    error "This library requires std::move_only_function implementation"
#endif

#include "adbfsm/common.hpp"

#include <functional>
#include <future>

namespace adbfsm::util
{
    class Threadpool
    {
    public:
        using Task = std::move_only_function<void()>;

        template <typename Fn, typename... Args>
        using Fut = std::future<std::invoke_result_t<Fn, Args...>>;

        enum class Status
        {
            Running,       // running
            Stopped,       // run() called then stop() called
            Terminated,    // stop() called without run()
        };

        Threadpool(usize num_threads, bool immediately_run)
        {
            for (auto _ : sv::iota(0uz, num_threads)) {
                m_threads.emplace_back([this] { thread_function(); });
            }

            if (immediately_run) {
                run();
            }
        }

        ~Threadpool()
        {
            auto remainder = stop(false);
            assert(remainder == 0);
        }

        template <typename... Args, std::invocable<Args...> Fn>
        [[nodiscard]] auto enqueue(Fn&& func, Args&&... args) -> Fut<Fn, Args...>
        {
            if (status() == Status::Terminated or status() == Status::Stopped) {
                throw std::runtime_error{ "Threadpool is terminated or stopped, recreate a new one." };
            }

            auto packaged_task = std::packaged_task<std::invoke_result_t<Fn, Args...>()>{
                [func = std::forward<Fn>(func), ... args = std::forward<Args>(args)]() mutable {
                    return func(std::forward<Args>(args)...);
                }
            };

            auto fut = packaged_task.get_future();
            {
                auto lock = std::unique_lock{ m_mutex };
                m_tasks.emplace_back([task = std::move(packaged_task)]() mutable { task(); });
            }

            m_condition.notify_one();
            return fut;
        }

        template <typename... Args, std::invocable<Args...> Func>
            requires std::same_as<std::invoke_result_t<Func, Args...>, void>
        void enqueue_detached(Func&& func, Args&&... args)
        {
            if (status() == Status::Terminated or status() == Status::Stopped) {
                throw std::runtime_error{ "Threadpool is terminated or stopped, recreate a new one." };
            }

            {
                auto lock = std::unique_lock{ m_mutex };
                auto fn   = [func = std::forward<Func>(func), ... args = std::forward<Args>(args)]() mutable {
                    func(std::forward<Args>(args)...);
                };
                m_tasks.emplace_back(std::move(fn));
            }
            m_condition.notify_one();
        }

        usize queued_tasks()
        {
            auto lock = std::unique_lock{ m_mutex };
            return m_tasks.size();
        }

        void run()
        {
            m_status = Status::Running;
            m_status.notify_all();
        }

        Status status() { return m_status; }

        // after this call, the instance will effectively become unusable.
        // create a new instance if you want to use Threadpool again.
        // returns number of ignored tasks if ignore_enqueued_tasks is true otherwise returns 0
        usize stop(bool ignore_enqueued_tasks)
        {
            switch (m_status) {
            case Status::Running: {
                m_status = Status::Stopped;
                if (auto lock = std::unique_lock{ m_mutex }; ignore_enqueued_tasks) {
                    m_tasks.clear();
                }
                m_condition.notify_all();

                for (auto& thread : m_threads) {
                    if (thread.joinable()) {
                        thread.join();
                    }
                }

                return m_tasks.size();
            }
            case Status::Stopped: {
                m_status = Status::Terminated;
                m_status.notify_all();
                m_condition.notify_all();
                return 0;
            }
            case Status::Terminated:
                m_condition.notify_all();    //
                return 0;
            default:    //
                return 0;
            }
        }

    private:
        void thread_function()
        {
            auto get_task_or_wait = [&]() -> Opt<Task> {
                auto lock = std::unique_lock{ m_mutex };
                m_condition.wait(lock, [&] { return not m_tasks.empty() or m_status != Status::Running; });

                if (m_status != Status::Running and m_tasks.empty()) {
                    return std::nullopt;
                }

                auto task = std::move(m_tasks.front());
                m_tasks.pop_front();

                return task;
            };

            m_status.wait(Status::Stopped);

            while (m_status == Status::Running) {
                if (auto task = get_task_or_wait(); task.has_value()) {
                    (*task)();
                } else {
                    break;
                }
            }
        }

        std::vector<std::jthread> m_threads;
        std::deque<Task>          m_tasks;
        std::mutex                m_mutex;
        std::condition_variable   m_condition;
        std::atomic<Status>       m_status = Status::Stopped;
    };
}
