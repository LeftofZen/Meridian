#include "scripting/ScriptingSystem.hpp"

#include "core/Logger.hpp"

namespace Meridian {

ScriptingSystem::~ScriptingSystem()
{
    shutdown();
}

bool ScriptingSystem::init()
{
    m_lua.open_libraries(
        sol::lib::base,
        sol::lib::package,
        sol::lib::string,
        sol::lib::table,
        sol::lib::math,
        sol::lib::io,
        sol::lib::os);

    // Register a simple engine log function accessible from Lua
    m_lua.set_function("log_info", [](const std::string& msg) { MRD_INFO("[Lua] {}", msg); });
    m_lua.set_function("log_warn", [](const std::string& msg) { MRD_WARN("[Lua] {}", msg); });

    m_initialized = true;
    MRD_INFO("Lua scripting system ready (sol2)");
    return true;
}

void ScriptingSystem::shutdown()
{
    m_initialized = false;
    // sol::state destructor handles cleanup
}

bool ScriptingSystem::execute(const std::string& script)
{
    const auto result = m_lua.safe_script(script, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        MRD_ERROR("Lua error: {}", err.what());
        return false;
    }
    return true;
}

bool ScriptingSystem::executeFile(const std::string& path)
{
    const auto result = m_lua.safe_script_file(path, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error err = result;
        MRD_ERROR("Lua error in '{}': {}", path, err.what());
        return false;
    }
    return true;
}

} // namespace Meridian
