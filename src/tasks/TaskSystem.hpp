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
        return m_executor != nullptr;
    }

    void shutdown()
    {
        if (m_executor) {
            m_executor->wait_for_all();
            m_executor.reset();
        }
    }

    [[nodiscard]] tf::Executor& getExecutor() noexcept { return *m_executor; }
    [[nodiscard]] const tf::Executor& getExecutor() const noexcept { return *m_executor; }

    // Convenience: run a taskflow graph and wait for completion
    void run(tf::Taskflow& flow) { m_executor->run(flow).wait(); }

private:
    std::unique_ptr<tf::Executor> m_executor;
};

} // namespace Meridian
