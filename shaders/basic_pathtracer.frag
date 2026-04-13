#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

struct GpuChunk {
    ivec4 coordAndResolution;
    uvec4 offsets;
};

layout(set = 0, binding = 0, std430) readonly buffer ChunkBuffer {
    GpuChunk chunks[];
};

layout(set = 0, binding = 1, std430) readonly buffer VoxelBuffer {
    uint voxelMaterials[];
};

layout(push_constant) uniform PushConstants {
    vec4 sceneMin;
    vec4 sceneMax;
    vec4 frameData;
    vec4 cameraPosition;
    vec4 cameraForward;
    uvec4 settings;
} pc;

const float kRayEpsilon = 0.001;
const float kChunkSize = 32.0;
const vec3 kWorldUp = vec3(0.0, 1.0, 0.0);
const float kVerticalFovDegrees = 42.0;

const uint kMaterialAir = 0u;
const uint kMaterialGrass = 1u;
const uint kMaterialStone = 2u;
const int kMaxDdaSteps = 1024;

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

    const vec3 tangent = normalize(
        abs(normal.y) < 0.999
            ? cross(normal, vec3(0.0, 1.0, 0.0))
            : cross(normal, vec3(1.0, 0.0, 0.0)));
    const vec3 bitangent = cross(normal, tangent);

    const vec3 localDirection = vec3(
        radius * cos(theta),
        sqrt(max(0.0, 1.0 - u1)),
        radius * sin(theta));

    return normalize(
        tangent * localDirection.x +
        normal * localDirection.y +
        bitangent * localDirection.z);
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

    if (materialId == kMaterialStone) {
        return vec3(0.52, 0.52, 0.56);
    }

    return vec3(0.0);
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
    return pc.settings.w > 0u ? chunks[0].coordAndResolution.w : int(kChunkSize);
}

uint sampleVoxelMaterial(ivec3 worldVoxelCoord)
{
    const int resolution = chunkResolution();
    const ivec3 chunkCoord = ivec3(
        floorDivInt(worldVoxelCoord.x, resolution),
        floorDivInt(worldVoxelCoord.y, resolution),
        floorDivInt(worldVoxelCoord.z, resolution));
    const ivec3 localCoord = ivec3(
        positiveModInt(worldVoxelCoord.x, resolution),
        positiveModInt(worldVoxelCoord.y, resolution),
        positiveModInt(worldVoxelCoord.z, resolution));

    for (uint chunkIndex = 0u; chunkIndex < pc.settings.w; ++chunkIndex) {
        if (any(notEqual(chunks[chunkIndex].coordAndResolution.xyz, chunkCoord))) {
            continue;
        }

        const uint voxelOffset = chunks[chunkIndex].offsets.x;
        const uint linearIndex =
            uint(localCoord.x) +
            uint(localCoord.y) * uint(resolution) +
            uint(localCoord.z) * uint(resolution * resolution);
        return voxelMaterials[voxelOffset + linearIndex];
    }

    return kMaterialAir;
}

bool intersectSceneBounds(Ray ray, out float tEnter, out vec3 entryNormal)
{
    const vec3 boxMin = pc.sceneMin.xyz;
    const vec3 boxMax = pc.sceneMax.xyz;
    const vec3 invDirection = 1.0 / ray.direction;
    const vec3 t0 = (boxMin - ray.origin) * invDirection;
    const vec3 t1 = (boxMax - ray.origin) * invDirection;
    const vec3 tNear3 = min(t0, t1);
    const vec3 tFar3 = max(t0, t1);

    tEnter = max(max(tNear3.x, tNear3.y), tNear3.z);
    const float tExit = min(min(tFar3.x, tFar3.y), tFar3.z);
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

bool sceneIntersect(Ray ray, out Hit hit)
{
    hit.found = false;
    hit.t = 1e30;

    if (pc.settings.w == 0u) {
        return false;
    }

    float tEnter = 0.0;
    vec3 currentNormal = vec3(0.0, 1.0, 0.0);
    if (!intersectSceneBounds(ray, tEnter, currentNormal)) {
        return false;
    }

    float currentT = max(tEnter, 0.0);
    const vec3 startPosition = ray.origin + ray.direction * (currentT + kRayEpsilon);
    ivec3 voxelCoord = ivec3(floor(startPosition));
    const ivec3 stepDir = ivec3(sign(ray.direction));

    const vec3 tDelta = vec3(
        ray.direction.x == 0.0 ? 1e30 : abs(1.0 / ray.direction.x),
        ray.direction.y == 0.0 ? 1e30 : abs(1.0 / ray.direction.y),
        ray.direction.z == 0.0 ? 1e30 : abs(1.0 / ray.direction.z));

    const vec3 nextBoundary = vec3(
        stepDir.x > 0 ? float(voxelCoord.x + 1) : float(voxelCoord.x),
        stepDir.y > 0 ? float(voxelCoord.y + 1) : float(voxelCoord.y),
        stepDir.z > 0 ? float(voxelCoord.z + 1) : float(voxelCoord.z));

    vec3 tMax = vec3(
        ray.direction.x == 0.0 ? 1e30 : (nextBoundary.x - ray.origin.x) / ray.direction.x,
        ray.direction.y == 0.0 ? 1e30 : (nextBoundary.y - ray.origin.y) / ray.direction.y,
        ray.direction.z == 0.0 ? 1e30 : (nextBoundary.z - ray.origin.z) / ray.direction.z);

    const ivec3 sceneMinVoxel = ivec3(floor(pc.sceneMin.xyz));
    const ivec3 sceneMaxVoxel = ivec3(ceil(pc.sceneMax.xyz));

    for (int stepIndex = 0; stepIndex < kMaxDdaSteps; ++stepIndex) {
        const bool outsideScene =
            any(lessThan(voxelCoord, sceneMinVoxel)) ||
            any(greaterThanEqual(voxelCoord, sceneMaxVoxel));
        if (outsideScene) {
            return false;
        }

        const uint materialId = sampleVoxelMaterial(voxelCoord);
        if (materialId != kMaterialAir) {
            hit.found = true;
            hit.t = currentT;
            hit.position = ray.origin + ray.direction * currentT;
            hit.normal = currentNormal;
            hit.albedo = materialAlbedo(materialId);
            return true;
        }

        if (tMax.x < tMax.y) {
            if (tMax.x < tMax.z) {
                voxelCoord.x += stepDir.x;
                currentT = tMax.x;
                tMax.x += tDelta.x;
                currentNormal = vec3(float(-stepDir.x), 0.0, 0.0);
            } else {
                voxelCoord.z += stepDir.z;
                currentT = tMax.z;
                tMax.z += tDelta.z;
                currentNormal = vec3(0.0, 0.0, float(-stepDir.z));
            }
        } else {
            if (tMax.y < tMax.z) {
                voxelCoord.y += stepDir.y;
                currentT = tMax.y;
                tMax.y += tDelta.y;
                currentNormal = vec3(0.0, float(-stepDir.y), 0.0);
            } else {
                voxelCoord.z += stepDir.z;
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