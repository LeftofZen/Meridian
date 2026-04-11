#pragma once

#include <sol/sol.hpp>

#include <string>

namespace Meridian {

class ScriptingSystem {
public:
    ScriptingSystem() = default;
    ~ScriptingSystem();

    ScriptingSystem(const ScriptingSystem&) = delete;
    ScriptingSystem& operator=(const ScriptingSystem&) = delete;
    ScriptingSystem(ScriptingSystem&&) = delete;
    ScriptingSystem& operator=(ScriptingSystem&&) = delete;

    [[nodiscard]] bool init();
    void shutdown();

    [[nodiscard]] sol::state& getLuaState() noexcept { return m_lua; }
    [[nodiscard]] const sol::state& getLuaState() const noexcept { return m_lua; }

    // Execute an inline Lua script string; returns true on success.
    [[nodiscard]] bool execute(const std::string& script);

    // Load and execute a Lua file; returns true on success.
    [[nodiscard]] bool executeFile(const std::string& path);

private:
    sol::state m_lua;
    bool m_initialized{false};
};

} // namespace Meridian
