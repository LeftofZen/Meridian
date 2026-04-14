#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

struct GpuChunk {
    ivec4 coordAndResolution;
    uvec4 octreeData;
};

layout(set = 0, binding = 0, std430) readonly buffer ChunkBuffer {
    GpuChunk chunks[];
};

layout(set = 0, binding = 1, std430) readonly buffer OctreeNodeBuffer {
    uint octreeNodes[];
};

layout(set = 0, binding = 2, std430) readonly buffer ChunkLookupBuffer {
    uint chunkLookup[];
};

layout(push_constant) uniform PushConstants {
    vec4 sceneMin;
    vec4 sceneMax;
    vec4 frameData;
    vec4 cameraPosition;
    vec4 cameraForward;
    uvec4 settings;
    ivec4 chunkGridOrigin;
    uvec4 chunkGridSize;
} pc;

const float kRayEpsilon = 0.001;
const float kChunkSize = 32.0;
const vec3 kWorldUp = vec3(0.0, 1.0, 0.0);
const float kVerticalFovDegrees = 42.0;

const uint kMaterialAir = 0u;
const uint kMaterialGrass = 1u;
const uint kMaterialStone = 2u;
const uint kMaterialDirt = 3u;
const uint kMaterialSand = 4u;
const uint kMaterialSnow = 5u;
const uint kMaterialForest = 6u;
const uint kPackedNodeWordCount = 9u;
const uint kInvalidChildIndex = 0xffffffffu;
const int kMaxOctreeStackEntries = 64;

struct Ray {
    vec3 origin;
    vec3 direction;
};

struct Hit {
    bool found;
    float t;
    vec3 position;
    vec3 normal;
    vec3 albedo;
};

uint hash(uint state)
{
    state ^= 2747636419u;
    state *= 2654435769u;
    state ^= state >> 16;
    state *= 2654435769u;
    state ^= state >> 16;
    state *= 2654435769u;
    return state;
}

float randomFloat(inout uint state)
{
    state = hash(state);
    return float(state) / 4294967296.0;
}

vec3 sampleCosineHemisphere(vec3 normal, inout uint rngState)
{
    const float u1 = randomFloat(rngState);
    const float u2 = randomFloat(rngState);
    const float radius = sqrt(u1);
    const float theta = 6.28318530718 * u2;

    const vec3 tangent =
        abs(normal.y) > 0.5
            ? vec3(1.0, 0.0, 0.0)
            : vec3(0.0, 1.0, 0.0);
    const vec3 bitangent = cross(normal, tangent);

    const vec3 localDirection = vec3(
        radius * cos(theta),
        sqrt(max(0.0, 1.0 - u1)),
        radius * sin(theta));

    return
        tangent * localDirection.x +
        normal * localDirection.y +
        bitangent * localDirection.z;
}

vec3 skyColor(vec3 direction)
{
    const float horizon = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    const vec3 sky = mix(vec3(0.68, 0.77, 0.92), vec3(0.15, 0.30, 0.58), horizon);

    const vec3 sunDirection = normalize(vec3(-0.35, 0.82, -0.28));
    const float sunAmount = pow(max(dot(direction, sunDirection), 0.0), 256.0);
    return sky + vec3(5.0, 4.8, 4.2) * sunAmount;
}

vec3 materialAlbedo(uint materialId)
{
    if (materialId == kMaterialGrass) {
        return vec3(0.26, 0.62, 0.23);
    }

    if (materialId == kMaterialDirt) {
        return vec3(0.47, 0.32, 0.19);
    }

    if (materialId == kMaterialSand) {
        return vec3(0.78, 0.71, 0.52);
    }

    if (materialId == kMaterialSnow) {
        return vec3(0.90, 0.93, 0.96);
    }

    if (materialId == kMaterialForest) {
        return vec3(0.16, 0.36, 0.12);
    }

    if (materialId == kMaterialStone) {
        return vec3(0.52, 0.52, 0.56);
    }

    return vec3(0.0);
}

uint lookupChunkIndex(ivec3 chunkCoord)
{
    const ivec3 relativeChunkCoord = chunkCoord - pc.chunkGridOrigin.xyz;

    if (any(lessThan(relativeChunkCoord, ivec3(0))) ||
        any(greaterThanEqual(relativeChunkCoord, ivec3(pc.chunkGridSize.xyz)))) {
        return 0u;
    }

    const uint lookupIndex =
        uint(relativeChunkCoord.x) +
        uint(relativeChunkCoord.y) * pc.chunkGridSize.x +
        uint(relativeChunkCoord.z) * pc.chunkGridSize.x * pc.chunkGridSize.y;
    return chunkLookup[lookupIndex];
}

uint nodeWord(uint nodeIndex, uint wordIndex)
{
    return octreeNodes[nodeIndex * kPackedNodeWordCount + wordIndex];
}

uint nodeMetadata(uint nodeIndex)
{
    return nodeWord(nodeIndex, 8u);
}

bool nodeIsLeaf(uint nodeIndex)
{
    return ((nodeMetadata(nodeIndex) >> 8u) & 1u) != 0u;
}

uint nodeChildMask(uint nodeIndex)
{
    return nodeMetadata(nodeIndex) & 0xffu;
}

uint nodeMaterialId(uint nodeIndex)
{
    return (nodeMetadata(nodeIndex) >> 16u) & 0xffu;
}

uint nodeChildIndex(uint nodeIndex, uint childIndex)
{
    return nodeWord(nodeIndex, childIndex);
}

int floorDivInt(int value, int divisor)
{
    return value >= 0 ? value / divisor : -(((-value - 1) / divisor) + 1);
}

int positiveModInt(int value, int divisor)
{
    const int remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

int chunkResolution()
{
    return pc.chunkGridSize.w > 0u ? int(pc.chunkGridSize.w) : int(kChunkSize);
}

bool hasSceneChunks()
{
    return all(greaterThan(pc.chunkGridSize.xyz, uvec3(0u)));
}

bool intersectAabb(Ray ray, vec3 boxMin, vec3 boxMax, out float tEnter, out float tExit, out vec3 entryNormal)
{
    const vec3 invDirection = 1.0 / ray.direction;
    const vec3 t0 = (boxMin - ray.origin) * invDirection;
    const vec3 t1 = (boxMax - ray.origin) * invDirection;
    const vec3 tNear3 = min(t0, t1);
    const vec3 tFar3 = max(t0, t1);

    tEnter = max(max(tNear3.x, tNear3.y), tNear3.z);
    tExit = min(min(tFar3.x, tFar3.y), tFar3.z);
    if (tEnter > tExit || tExit < 0.0) {
        return false;
    }

    if (tNear3.x > tNear3.y && tNear3.x > tNear3.z) {
        entryNormal = vec3(ray.direction.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
    } else if (tNear3.y > tNear3.z) {
        entryNormal = vec3(0.0, ray.direction.y > 0.0 ? -1.0 : 1.0, 0.0);
    } else {
        entryNormal = vec3(0.0, 0.0, ray.direction.z > 0.0 ? -1.0 : 1.0);
    }

    return true;
}

bool traverseChunkOctree(Ray ray, GpuChunk chunk, float tMin, float tMax, out Hit hit)
{
    hit.found = false;
    hit.t = 1e30;

    if (chunk.octreeData.y == 0u) {
        return false;
    }

    const float chunkExtent = float(chunk.coordAndResolution.w);
    const vec3 chunkMin = vec3(chunk.coordAndResolution.xyz) * chunkExtent;
    const vec3 chunkMax = chunkMin + vec3(chunkExtent);

    float rootEnter = 0.0;
    float rootExit = 0.0;
    vec3 rootNormal = vec3(0.0, 1.0, 0.0);
    if (!intersectAabb(ray, chunkMin, chunkMax, rootEnter, rootExit, rootNormal)) {
        return false;
    }

    rootEnter = max(rootEnter, tMin);
    rootExit = min(rootExit, tMax);
    if (rootEnter > rootExit) {
        return false;
    }

    uint nodeStack[kMaxOctreeStackEntries];
    vec3 minStack[kMaxOctreeStackEntries];
    float extentStack[kMaxOctreeStackEntries];
    float enterStack[kMaxOctreeStackEntries];
    float exitStack[kMaxOctreeStackEntries];
    vec3 normalStack[kMaxOctreeStackEntries];
    int stackSize = 0;

    nodeStack[stackSize] = chunk.octreeData.x + chunk.octreeData.y - 1u;
    minStack[stackSize] = chunkMin;
    extentStack[stackSize] = chunkExtent;
    enterStack[stackSize] = rootEnter;
    exitStack[stackSize] = rootExit;
    normalStack[stackSize] = rootNormal;
    stackSize += 1;

    while (stackSize > 0) {
        stackSize -= 1;

        const uint nodeIndex = nodeStack[stackSize];
        const vec3 nodeMin = minStack[stackSize];
        const float nodeExtent = extentStack[stackSize];
        const float nodeEnter = enterStack[stackSize];
        const float nodeExit = exitStack[stackSize];
        const vec3 nodeNormal = normalStack[stackSize];

        if (nodeIsLeaf(nodeIndex)) {
            hit.found = true;
            hit.t = nodeEnter;
            hit.position = ray.origin + ray.direction * nodeEnter;
            hit.normal = nodeNormal;
            hit.albedo = materialAlbedo(nodeMaterialId(nodeIndex));
            return true;
        }

        const float childExtent = nodeExtent * 0.5;
        uint childNodeIndices[8];
        vec3 childMins[8];
        float childEnters[8];
        float childExits[8];
        vec3 childNormals[8];
        int childCount = 0;

        const uint childMask = nodeChildMask(nodeIndex);
        for (uint childIndex = 0u; childIndex < 8u; ++childIndex) {
            if ((childMask & (1u << childIndex)) == 0u) {
                continue;
            }

            const uint childNodeIndex = nodeChildIndex(nodeIndex, childIndex);
            if (childNodeIndex == kInvalidChildIndex) {
                continue;
            }

            const vec3 childMin = nodeMin + vec3(
                (childIndex & 1u) != 0u ? childExtent : 0.0,
                (childIndex & 2u) != 0u ? childExtent : 0.0,
                (childIndex & 4u) != 0u ? childExtent : 0.0);
            const vec3 childMax = childMin + vec3(childExtent);

            float childEnter = 0.0;
            float childExit = 0.0;
            vec3 childNormal = vec3(0.0, 1.0, 0.0);
            if (!intersectAabb(ray, childMin, childMax, childEnter, childExit, childNormal)) {
                continue;
            }

            childEnter = max(childEnter, nodeEnter);
            childExit = min(childExit, nodeExit);
            if (childEnter > childExit) {
                continue;
            }

            int insertIndex = childCount;
            while (insertIndex > 0 && childEnter < childEnters[insertIndex - 1]) {
                childNodeIndices[insertIndex] = childNodeIndices[insertIndex - 1];
                childMins[insertIndex] = childMins[insertIndex - 1];
                childEnters[insertIndex] = childEnters[insertIndex - 1];
                childExits[insertIndex] = childExits[insertIndex - 1];
                childNormals[insertIndex] = childNormals[insertIndex - 1];
                insertIndex -= 1;
            }

            childNodeIndices[insertIndex] = chunk.octreeData.x + childNodeIndex;
            childMins[insertIndex] = childMin;
            childEnters[insertIndex] = childEnter;
            childExits[insertIndex] = childExit;
            childNormals[insertIndex] = childNormal;
            childCount += 1;
        }

        for (int child = childCount - 1; child >= 0; --child) {
            if (stackSize >= kMaxOctreeStackEntries) {
                break;
            }

            nodeStack[stackSize] = childNodeIndices[child];
            minStack[stackSize] = childMins[child];
            extentStack[stackSize] = childExtent;
            enterStack[stackSize] = childEnters[child];
            exitStack[stackSize] = childExits[child];
            normalStack[stackSize] = childNormals[child];
            stackSize += 1;
        }
    }

    return false;
}

bool intersectSceneBounds(Ray ray, out float tEnter, out vec3 entryNormal)
{
    const vec3 boxMin = pc.sceneMin.xyz;
    const vec3 boxMax = pc.sceneMax.xyz;
    float tExit = 0.0;
    return intersectAabb(ray, boxMin, boxMax, tEnter, tExit, entryNormal);
}

bool sceneIntersect(Ray ray, out Hit hit)
{
    hit.found = false;
    hit.t = 1e30;

    if (!hasSceneChunks()) {
        return false;
    }

    float tEnter = 0.0;
    vec3 currentNormal = vec3(0.0, 1.0, 0.0);
    if (!intersectSceneBounds(ray, tEnter, currentNormal)) {
        return false;
    }

    float currentT = max(tEnter, 0.0);
    const int chunkExtent = chunkResolution();
    const vec3 startPosition = ray.origin + ray.direction * (currentT + kRayEpsilon);
    const ivec3 startVoxelCoord = ivec3(floor(startPosition));
    ivec3 chunkCoord = ivec3(
        floorDivInt(startVoxelCoord.x, chunkExtent),
        floorDivInt(startVoxelCoord.y, chunkExtent),
        floorDivInt(startVoxelCoord.z, chunkExtent));
    const ivec3 stepDir = ivec3(sign(ray.direction));

    const vec3 chunkExtent3 = vec3(float(chunkExtent));
    const vec3 nextBoundary = vec3(
        stepDir.x > 0 ? float(chunkCoord.x + 1) * chunkExtent3.x : float(chunkCoord.x) * chunkExtent3.x,
        stepDir.y > 0 ? float(chunkCoord.y + 1) * chunkExtent3.y : float(chunkCoord.y) * chunkExtent3.y,
        stepDir.z > 0 ? float(chunkCoord.z + 1) * chunkExtent3.z : float(chunkCoord.z) * chunkExtent3.z);
    const vec3 tDelta = vec3(
        ray.direction.x == 0.0 ? 1e30 : abs(chunkExtent3.x / ray.direction.x),
        ray.direction.y == 0.0 ? 1e30 : abs(chunkExtent3.y / ray.direction.y),
        ray.direction.z == 0.0 ? 1e30 : abs(chunkExtent3.z / ray.direction.z));
    vec3 tMax = vec3(
        ray.direction.x == 0.0 ? 1e30 : (nextBoundary.x - ray.origin.x) / ray.direction.x,
        ray.direction.y == 0.0 ? 1e30 : (nextBoundary.y - ray.origin.y) / ray.direction.y,
        ray.direction.z == 0.0 ? 1e30 : (nextBoundary.z - ray.origin.z) / ray.direction.z);

    const int resolution = chunkResolution();
    const ivec3 sceneMinChunk = pc.chunkGridOrigin.xyz;
    const ivec3 sceneMaxChunk =
        sceneMinChunk + ivec3(pc.chunkGridSize.x, pc.chunkGridSize.y, pc.chunkGridSize.z);

    const int maxDdaSteps = int(max(pc.settings.w, 1u));
    for (int stepIndex = 0; stepIndex < maxDdaSteps; ++stepIndex) {
        const bool outsideScene =
            any(lessThan(chunkCoord, sceneMinChunk)) ||
            any(greaterThanEqual(chunkCoord, sceneMaxChunk));
        if (outsideScene) {
            return false;
        }

        const uint currentChunkIndexPlusOne = lookupChunkIndex(chunkCoord);
        if (currentChunkIndexPlusOne != 0u) {
            Hit chunkHit;
            const float chunkExitT = min(tMax.x, min(tMax.y, tMax.z));
            if (traverseChunkOctree(
                    ray,
                    chunks[currentChunkIndexPlusOne - 1u],
                    currentT,
                    chunkExitT,
                    chunkHit)) {
                hit = chunkHit;
                return true;
            }
        }

        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                chunkCoord.x += stepDir.x;
                currentT = tMax.x;
                tMax.x += tDelta.x;
                currentNormal = vec3(float(-stepDir.x), 0.0, 0.0);
            } else {
                chunkCoord.z += stepDir.z;
                currentT = tMax.z;
                tMax.z += tDelta.z;
                currentNormal = vec3(0.0, 0.0, float(-stepDir.z));
            }
        } else {
            if (tMax.y < tMax.z) {
                chunkCoord.y += stepDir.y;
                currentT = tMax.y;
                tMax.y += tDelta.y;
                currentNormal = vec3(0.0, float(-stepDir.y), 0.0);
            } else {
                chunkCoord.z += stepDir.z;
                currentT = tMax.z;
                tMax.z += tDelta.z;
                currentNormal = vec3(0.0, 0.0, float(-stepDir.z));
            }
        }
    }

    return false;
}

vec3 tracePath(Ray ray, inout uint rngState)
{
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);

    for (uint bounce = 0u; bounce < max(pc.settings.z, 1u); ++bounce) {
        Hit hit;
        if (!sceneIntersect(ray, hit)) {
            radiance += throughput * skyColor(ray.direction);
            break;
        }

        throughput *= hit.albedo;
        if (max(throughput.r, max(throughput.g, throughput.b)) < 0.02) {
            break;
        }

        ray.origin = hit.position + hit.normal * kRayEpsilon;
        ray.direction = sampleCosineHemisphere(hit.normal, rngState);
    }

    return radiance;
}

void main()
{
    const vec2 pixel = gl_FragCoord.xy;
    uint rngState =
        uint(pixel.x) * 1973u ^
        uint(pixel.y) * 9277u ^
        (pc.settings.x + 1u) * 26699u;

    const vec3 forward = normalize(pc.cameraForward.xyz);
    const vec3 right = normalize(cross(forward, kWorldUp));
    const vec3 up = normalize(cross(right, forward));
    const float aspectRatio = pc.frameData.x / max(pc.frameData.y, 1.0);
    const float tanHalfFov = tan(radians(kVerticalFovDegrees) * 0.5);

    vec3 color = vec3(0.0);
    const uint sampleCount = max(pc.settings.y, 1u);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex) {
        const vec2 jitter = vec2(randomFloat(rngState), randomFloat(rngState));
        vec2 ndc = ((pixel + jitter) / pc.frameData.xy) * 2.0 - 1.0;
        ndc.x *= aspectRatio;
        ndc.y = -ndc.y;

        Ray ray;
        ray.origin = pc.cameraPosition.xyz;
        ray.direction = normalize(
            forward +
            right * (ndc.x * tanHalfFov) +
            up * (ndc.y * tanHalfFov));

        color += tracePath(ray, rngState);
    }

    color /= float(sampleCount);
    color = color / (vec3(1.0) + color);
    color = pow(color, vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}