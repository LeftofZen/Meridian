#pragma once

#include <taskflow/taskflow.hpp>

#include <memory>

namespace Meridian {

class TaskSystem {
public:
    TaskSystem() = default;
    ~TaskSystem() { shutdown(); }

    TaskSystem(const TaskSystem&) = delete;
    TaskSystem& operator=(const TaskSystem&) = delete;
    TaskSystem(TaskSystem&&) = delete;
    TaskSystem& operator=(TaskSystem&&) = delete;

    [[nodiscard]] bool init()
    {
        m_executor = std::make_unique<tf::Executor>();
        m_initialized = (m_executor != nullptr);
        return m_initialized;
    }

    void shutdown()
    {
        if (m_executor) {
            m_executor->wait_for_all();
            m_executor.reset();
        }
        m_initialized = false;
    }

    [[nodiscard]] tf::Executor& getExecutor() noexcept { return *m_executor; }
    [[nodiscard]] const tf::Executor& getExecutor() const noexcept { return *m_executor; }

    void run(tf::Taskflow& flow) { m_executor->run(flow).wait(); }

    [[nodiscard]] bool isInitialised() const noexcept { return m_initialized; }

private:
    std::unique_ptr<tf::Executor> m_executor;
    bool m_initialized{false};
};

} // namespace Meridian
