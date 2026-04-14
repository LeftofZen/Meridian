#include "renderer/ShaderLibrary.hpp"

#include "core/Logger.hpp"

#include <cstdint>
#include <fstream>

namespace {

void setShaderModuleDebugName(VkDevice device, VkShaderModule module, std::string_view name) noexcept
{
    if (device == VK_NULL_HANDLE || module == VK_NULL_HANDLE || name.empty() ||
        vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }

    VkDebugUtilsObjectNameInfoEXT nameInfo{};
    nameInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    nameInfo.objectType = VK_OBJECT_TYPE_SHADER_MODULE;
    nameInfo.objectHandle = reinterpret_cast<std::uint64_t>(module);
    nameInfo.pObjectName = name.data();
    vkSetDebugUtilsObjectNameEXT(device, &nameInfo);
}

} // namespace

namespace Meridian {

ShaderLibrary::ShaderLibrary(VkDevice device) noexcept : m_device(device) {}

ShaderLibrary::~ShaderLibrary()
{
    clear();
}

VkShaderModule ShaderLibrary::loadModule(
    std::string_view name,
    const std::filesystem::path& path)
{
    const auto existing = m_modules.find(std::string{name});
    if (existing != m_modules.end()) {
        return existing->second;
    }

    const std::vector<std::uint32_t> spirv = readSpirvFile(path);
    if (spirv.empty()) {
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(std::uint32_t);
    createInfo.pCode = spirv.data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(m_device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        MRD_ERROR("Failed to create shader module '{}' from {}",
            name,
            path.string());
        return VK_NULL_HANDLE;
    }

    setShaderModuleDebugName(m_device, module, name);

    m_modules.emplace(std::string{name}, module);
    return module;
}

void ShaderLibrary::clear() noexcept
{
    if (m_device == VK_NULL_HANDLE) {
        m_modules.clear();
        return;
    }

    for (auto& [name, module] : m_modules) {
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(m_device, module, nullptr);
        }
    }
    m_modules.clear();
}

std::vector<std::uint32_t> ShaderLibrary::readSpirvFile(
    const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        MRD_ERROR("Failed to open shader file {}", path.string());
        return {};
    }

    const std::streamsize size = file.tellg();
    if (size <= 0 || (size % static_cast<std::streamsize>(sizeof(std::uint32_t))) != 0) {
        MRD_ERROR("Shader file {} is not valid SPIR-V", path.string());
        return {};
    }

    file.seekg(0, std::ios::beg);

    std::vector<std::uint32_t> data(static_cast<std::size_t>(size) / sizeof(std::uint32_t));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        MRD_ERROR("Failed to read shader file {}", path.string());
        return {};
    }

    return data;
}

} // namespace Meridian