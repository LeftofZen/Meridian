#include "world/WorldChunkDatabase.hpp"

#include "core/Logger.hpp"

#include <lmdb.h>
#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include <tracy/Tracy.hpp>

namespace Meridian {

namespace {

constexpr std::uint32_t kChunkBlobMagic{0x4D524443U};
constexpr std::uint32_t kChunkBlobVersion{1U};
constexpr int kChunkCompressionLevel{1};
constexpr std::size_t kChunkDatabaseMapSizeBytes{2ULL * 1024ULL * 1024ULL * 1024ULL};

struct ChunkBlobHeader {
    std::uint32_t magic{kChunkBlobMagic};
    std::uint32_t version{kChunkBlobVersion};
    std::uint64_t terrainSettingsSignature{0};
    std::int32_t chunkX{0};
    std::int32_t chunkY{0};
    std::int32_t chunkZ{0};
    std::uint32_t voxelResolution{0};
    std::uint32_t voxelCount{0};
};

static_assert(std::is_trivially_copyable_v<ChunkBlobHeader>);

template <typename T>
void appendValue(std::vector<std::uint8_t>& buffer, const T& value)
{
    static_assert(std::is_trivially_copyable_v<T>);

    const std::size_t offset = buffer.size();
    buffer.resize(offset + sizeof(T));
    std::memcpy(buffer.data() + offset, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] bool readValue(
    const std::vector<std::uint8_t>& buffer,
    std::size_t& readOffset,
    T& value) noexcept
{
    static_assert(std::is_trivially_copyable_v<T>);

    if (readOffset + sizeof(T) > buffer.size()) {
        return false;
    }

    std::memcpy(&value, buffer.data() + readOffset, sizeof(T));
    readOffset += sizeof(T);
    return true;
}

[[nodiscard]] std::vector<std::uint8_t> serialiseChunk(
    const WorldChunkStorage& chunkStorage,
    std::uint64_t terrainSettingsSignature)
{
    ZoneScopedN("WorldChunkDatabase::serialiseChunk");
    const auto& voxels = chunkStorage.voxels();
    const ChunkBlobHeader header{
        .magic = kChunkBlobMagic,
        .version = kChunkBlobVersion,
        .terrainSettingsSignature = terrainSettingsSignature,
        .chunkX = chunkStorage.coord().x,
        .chunkY = chunkStorage.coord().y,
        .chunkZ = chunkStorage.coord().z,
        .voxelResolution = chunkStorage.voxelResolution(),
        .voxelCount = static_cast<std::uint32_t>(voxels.size()),
    };

    std::vector<std::uint8_t> buffer;
    buffer.reserve(
        sizeof(ChunkBlobHeader) +
        voxels.size() * sizeof(float) +
        voxels.size() * sizeof(std::uint8_t));
    appendValue(buffer, header);

    for (const VoxelSample& voxel : voxels) {
        appendValue(buffer, voxel.density);
    }

    for (const VoxelSample& voxel : voxels) {
        appendValue(buffer, voxel.materialId);
    }

    return buffer;
}

[[nodiscard]] std::optional<WorldChunkStorage> deserialiseChunk(
    const std::vector<std::uint8_t>& buffer,
    ChunkCoord coord,
    ChunkKey key,
    std::uint64_t terrainSettingsSignature)
{
    ZoneScopedN("WorldChunkDatabase::deserialiseChunk");
    std::size_t readOffset = 0;
    ChunkBlobHeader header{};
    if (!readValue(buffer, readOffset, header)) {
        return std::nullopt;
    }

    if (header.magic != kChunkBlobMagic || header.version != kChunkBlobVersion) {
        return std::nullopt;
    }

    if (header.terrainSettingsSignature != terrainSettingsSignature) {
        return std::nullopt;
    }

    if (header.chunkX != coord.x || header.chunkY != coord.y || header.chunkZ != coord.z) {
        return std::nullopt;
    }

    const std::uint64_t expectedVoxelCount =
        static_cast<std::uint64_t>(header.voxelResolution) *
        static_cast<std::uint64_t>(header.voxelResolution) *
        static_cast<std::uint64_t>(header.voxelResolution);
    if (expectedVoxelCount != header.voxelCount) {
        return std::nullopt;
    }

    std::vector<VoxelSample> voxels(header.voxelCount);
    for (VoxelSample& voxel : voxels) {
        if (!readValue(buffer, readOffset, voxel.density)) {
            return std::nullopt;
        }
    }

    for (VoxelSample& voxel : voxels) {
        if (!readValue(buffer, readOffset, voxel.materialId)) {
            return std::nullopt;
        }
    }

    SparseVoxelOctree octree;
    {
        ZoneScopedN("SparseVoxelOctree::build");
        octree = SparseVoxelOctree::build(voxels, header.voxelResolution);
    }
    return WorldChunkStorage(
        coord,
        key,
        header.voxelResolution,
        std::move(voxels),
        std::move(octree));
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> compressChunkBlob(
    const std::vector<std::uint8_t>& serialisedChunk)
{
    ZoneScopedN("WorldChunkDatabase::compressChunkBlob");
    std::vector<std::uint8_t> compressedBuffer(ZSTD_compressBound(serialisedChunk.size()));
    const std::size_t compressedSize = ZSTD_compress(
        compressedBuffer.data(),
        compressedBuffer.size(),
        serialisedChunk.data(),
        serialisedChunk.size(),
        kChunkCompressionLevel);
    if (ZSTD_isError(compressedSize) != 0U) {
        return std::nullopt;
    }

    compressedBuffer.resize(compressedSize);
    return compressedBuffer;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> decompressChunkBlob(
    std::span<const std::uint8_t> compressedBytes)
{
    ZoneScopedN("WorldChunkDatabase::decompressChunkBlob");
    const auto* compressedData = compressedBytes.data();
    const std::size_t compressedSize = compressedBytes.size();
    const unsigned long long uncompressedSize =
        ZSTD_getFrameContentSize(compressedData, compressedSize);
    if (uncompressedSize == ZSTD_CONTENTSIZE_ERROR ||
        uncompressedSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> decompressedBuffer(static_cast<std::size_t>(uncompressedSize));
    const std::size_t actualSize = ZSTD_decompress(
        decompressedBuffer.data(),
        decompressedBuffer.size(),
        compressedData,
        compressedSize);
    if (ZSTD_isError(actualSize) != 0U || actualSize != decompressedBuffer.size()) {
        return std::nullopt;
    }

    return decompressedBuffer;
}

} // namespace

WorldChunkDatabase::~WorldChunkDatabase()
{
    shutdown();
}

bool WorldChunkDatabase::init(const std::filesystem::path& databasePath)
{
    std::scoped_lock lock(m_mutex);
    if (m_initialised) {
        return true;
    }

    std::error_code directoryError;
    std::filesystem::create_directories(databasePath, directoryError);
    if (directoryError) {
        MRD_ERROR(
            "Failed to create chunk database directory '{}': {}",
            databasePath.string(),
            directoryError.message());
        return false;
    }

    if (mdb_env_create(&m_env) != MDB_SUCCESS) {
        m_env = nullptr;
        MRD_ERROR("Failed to create LMDB environment for world chunks");
        return false;
    }

    if (mdb_env_set_maxdbs(m_env, 1) != MDB_SUCCESS ||
        mdb_env_set_mapsize(m_env, kChunkDatabaseMapSizeBytes) != MDB_SUCCESS) {
        shutdown();
        MRD_ERROR("Failed to configure LMDB environment for world chunks");
        return false;
    }

    const std::string databasePathString = databasePath.string();
    if (mdb_env_open(m_env, databasePathString.c_str(), 0, 0664) != MDB_SUCCESS) {
        shutdown();
        MRD_ERROR("Failed to open LMDB environment for world chunks at '{}'", databasePathString);
        return false;
    }

    MDB_txn* transaction = nullptr;
    if (mdb_txn_begin(m_env, nullptr, 0, &transaction) != MDB_SUCCESS) {
        shutdown();
        MRD_ERROR("Failed to begin LMDB transaction for world chunks");
        return false;
    }

    const int openResult = mdb_dbi_open(transaction, "chunks", MDB_CREATE, &m_chunkDatabaseHandle);
    if (openResult != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        shutdown();
        MRD_ERROR("Failed to open chunk database handle: {}", mdb_strerror(openResult));
        return false;
    }

    const int commitResult = mdb_txn_commit(transaction);
    if (commitResult != MDB_SUCCESS) {
        shutdown();
        MRD_ERROR("Failed to commit chunk database init transaction: {}", mdb_strerror(commitResult));
        return false;
    }

    m_initialised = true;
    MRD_INFO("World chunk database initialised at '{}'", databasePathString);
    return true;
}

void WorldChunkDatabase::shutdown() noexcept
{
    std::scoped_lock lock(m_mutex);
    if (m_env != nullptr) {
        if (m_initialised) {
            mdb_dbi_close(m_env, m_chunkDatabaseHandle);
        }
        mdb_env_close(m_env);
        m_env = nullptr;
    }

    m_chunkDatabaseHandle = 0;
    m_initialised = false;
}

std::optional<WorldChunkStorage> WorldChunkDatabase::loadChunk(
    ChunkCoord coord,
    ChunkKey key,
    std::uint64_t terrainSettingsSignature) const
{
    ZoneScopedN("WorldChunkDatabase::loadChunk");

    // Copy the raw compressed bytes while holding the lock and LMDB transaction open.
    std::vector<std::uint8_t> rawBytes;
    {
        std::scoped_lock lock(m_mutex);
        if (!isInitialised()) {
            return std::nullopt;
        }

        MDB_txn* transaction = nullptr;
        const int beginResult = mdb_txn_begin(m_env, nullptr, MDB_RDONLY, &transaction);
        if (beginResult != MDB_SUCCESS) {
            MRD_WARN("Failed to begin LMDB read transaction for chunk load: {}", mdb_strerror(beginResult));
            return std::nullopt;
        }

        MDB_val databaseKey{
            .mv_size = sizeof(key),
            .mv_data = &key,
        };
        MDB_val databaseValue{};
        const int getResult = mdb_get(transaction, m_chunkDatabaseHandle, &databaseKey, &databaseValue);
        if (getResult == MDB_NOTFOUND) {
            mdb_txn_abort(transaction);
            return std::nullopt;
        }

        if (getResult != MDB_SUCCESS) {
            mdb_txn_abort(transaction);
            MRD_WARN("Failed to load chunk {} from LMDB: {}", key, mdb_strerror(getResult));
            return std::nullopt;
        }

        rawBytes.resize(databaseValue.mv_size);
        std::memcpy(rawBytes.data(), databaseValue.mv_data, databaseValue.mv_size);
        mdb_txn_abort(transaction);
    }

    // Decompress and deserialise outside the mutex to avoid blocking other threads.
    const std::optional<std::vector<std::uint8_t>> decompressedBlob = decompressChunkBlob(rawBytes);
    if (!decompressedBlob.has_value()) {
        MRD_WARN("Failed to decompress cached world chunk {}", key);
        return std::nullopt;
    }

    return deserialiseChunk(*decompressedBlob, coord, key, terrainSettingsSignature);
}

bool WorldChunkDatabase::storeChunk(
    const WorldChunkStorage& chunkStorage,
    std::uint64_t terrainSettingsSignature)
{
    ZoneScopedN("WorldChunkDatabase::storeChunk");

    // Serialise and compress outside the mutex — these can be expensive.
    const std::vector<std::uint8_t> serialisedChunk =
        serialiseChunk(chunkStorage, terrainSettingsSignature);
    const std::optional<std::vector<std::uint8_t>> compressedChunk =
        compressChunkBlob(serialisedChunk);
    if (!compressedChunk.has_value()) {
        MRD_WARN("Failed to compress world chunk {} for persistence", chunkStorage.key());
        return false;
    }

    std::scoped_lock lock(m_mutex);
    if (!isInitialised()) {
        return false;
    }

    MDB_txn* transaction = nullptr;
    const int beginResult = mdb_txn_begin(m_env, nullptr, 0, &transaction);
    if (beginResult != MDB_SUCCESS) {
        MRD_WARN("Failed to begin LMDB write transaction for chunk store: {}", mdb_strerror(beginResult));
        return false;
    }

    ChunkKey key = chunkStorage.key();
    MDB_val databaseKey{
        .mv_size = sizeof(key),
        .mv_data = &key,
    };
    MDB_val databaseValue{
        .mv_size = compressedChunk->size(),
        .mv_data = const_cast<std::uint8_t*>(compressedChunk->data()),
    };
    const int putResult = mdb_put(transaction, m_chunkDatabaseHandle, &databaseKey, &databaseValue, 0);
    if (putResult != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        MRD_WARN("Failed to persist world chunk {}: {}", key, mdb_strerror(putResult));
        return false;
    }

    const int commitResult = mdb_txn_commit(transaction);
    if (commitResult != MDB_SUCCESS) {
        MRD_WARN("Failed to commit world chunk {}: {}", key, mdb_strerror(commitResult));
        return false;
    }

    return true;
}

bool WorldChunkDatabase::isInitialised() const noexcept
{
    return m_initialised && m_env != nullptr;
}

} // namespace Meridian