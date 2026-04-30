#pragma once

#include "core/ISystem.hpp"

#include <taskflow/taskflow.hpp>

#include <tracy/Tracy.hpp>

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <stdexcept>
#include <type_traits>

namespace Meridian {

class TaskSystem final : public ISystem {
public:
    TaskSystem() = default;
    ~TaskSystem() override { shutdown(); }

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;
    TaskSystem(TaskSystem&&) = delete;
    TaskSystem& operator=(TaskSystem&&) = delete;

    [[nodiscard]] bool init()
    {
        ZoneScopedN("TaskSystem::init");
        m_executor = std::make_unique<tf::Executor>();
        m_initialized = (m_executor != nullptr);
        return m_initialized;
    }

    void shutdown()
    {
        ZoneScopedN("TaskSystem::shutdown");
        if (m_executor) {
            m_executor->wait_for_all();
            m_executor.reset();
        }
        m_initialized = false;
    }

    void update(float /*deltaTimeSeconds*/) override {}

    [[nodiscard]] tf::Executor& getExecutor()
    {
        ensureInitialised();
        return *m_executor;
    }
    [[nodiscard]] const tf::Executor& getExecutor() const
    {
        ensureInitialised();
        return *m_executor;
    }

    void run(tf::Taskflow& flow)
    {
        ensureInitialised();
        ZoneScopedN("TaskSystem::run");
        m_executor->run(flow).wait();
    }

    template <typename F>
    [[nodiscard]] auto async(F&& task)
        -> std::future<std::invoke_result_t<std::decay_t<F>>>
    {
        ensureInitialised();

        using Task = std::decay_t<F>;
        using Result = std::invoke_result_t<Task>;

        return m_executor->async([task = Task(std::forward<F>(task))]() mutable -> Result {
            thread_local std::string workerThreadName;
            if (workerThreadName.empty()) {
                static std::atomic<std::uint32_t> nextWorkerIndex{0};
                workerThreadName = "Task Worker " +
                    std::to_string(nextWorkerIndex.fetch_add(1, std::memory_order_relaxed) + 1U);
                tracy::SetThreadName(workerThreadName.c_str());
            }

            ZoneScopedN("TaskSystem::async");
            if constexpr (std::is_void_v<Result>) {
                std::invoke(task);
                return;
            } else {
                return std::invoke(task);
            }
        });
    }

    [[nodiscard]] bool isInitialised() const noexcept { return m_initialized; }

private:
    void ensureInitialised() const
    {
        if (!m_initialized || !m_executor) {
            throw std::logic_error("TaskSystem is not initialised");
        }
    }

    std::unique_ptr<tf::Executor> m_executor;
    bool m_initialized{false};
};

} // namespace Meridian
