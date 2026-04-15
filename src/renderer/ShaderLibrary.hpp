#pragma once

#include <volk.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Meridian {

class ShaderLibrary final {
public:
    explicit ShaderLibrary(VkDevice device) noexcept;
    ~ShaderLibrary();

    ShaderLibrary(const ShaderLibrary&) = delete;
    ShaderLibrary& operator=(const ShaderLibrary&) = delete;
    ShaderLibrary(ShaderLibrary&&) = delete;
    ShaderLibrary& operator=(ShaderLibrary&&) = delete;

    [[nodiscard]] VkShaderModule loadModule(
        std::string_view name,
        const std::filesystem::path& path);
    [[nodiscard]] VkShaderModule loadBuiltInModule(std::string_view name);
    void clear() noexcept;

private:
    [[nodiscard]] static bool isBuiltInShader(std::string_view name) noexcept;
    [[nodiscard]] static std::filesystem::path builtInShaderPath(std::string_view name);
    [[nodiscard]] static std::vector<std::uint32_t> readSpirvFile(
        const std::filesystem::path& path);

    VkDevice m_device{VK_NULL_HANDLE};
    std::unordered_map<std::string, VkShaderModule> m_modules;
};

} // namespace Meridian