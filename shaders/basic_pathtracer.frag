#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec2 resolution;
    uint frameIndex;
    uint samplesPerPixel;
    uint maxBounces;
} pc;

const float kRayEpsilon = 0.001;
const float kChunkSize = 32.0;
const vec3 kCameraPosition = vec3(48.0, 56.0, 112.0);
const vec3 kCameraTarget = vec3(0.0, 16.0, 0.0);
const vec3 kWorldUp = vec3(0.0, 1.0, 0.0);
const float kVerticalFovDegrees = 42.0;

const uint kMaterialAir = 0u;
const uint kMaterialGrass = 1u;
const uint kMaterialStone = 2u;

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

uint materialForChunk(ivec3 chunkCoord)
{
    if (chunkCoord.y < 0) {
        return kMaterialStone;
    }

    if (chunkCoord.y == 0) {
        return kMaterialGrass;
    }

    return kMaterialAir;
}

bool intersectAabb(Ray ray, vec3 boxMin, vec3 boxMax, out float tHit, out vec3 normal)
{
    const vec3 invDirection = 1.0 / ray.direction;
    const vec3 t0 = (boxMin - ray.origin) * invDirection;
    const vec3 t1 = (boxMax - ray.origin) * invDirection;
    const vec3 tMin3 = min(t0, t1);
    const vec3 tMax3 = max(t0, t1);

    const float tNear = max(max(tMin3.x, tMin3.y), tMin3.z);
    const float tFar = min(min(tMax3.x, tMax3.y), tMax3.z);
    if (tNear > tFar || tFar < 0.0) {
        return false;
    }

    tHit = tNear > 0.0 ? tNear : tFar;
    const vec3 hitPosition = ray.origin + ray.direction * tHit;

    if (abs(hitPosition.x - boxMin.x) < 0.01) {
        normal = vec3(-1.0, 0.0, 0.0);
    } else if (abs(hitPosition.x - boxMax.x) < 0.01) {
        normal = vec3(1.0, 0.0, 0.0);
    } else if (abs(hitPosition.y - boxMin.y) < 0.01) {
        normal = vec3(0.0, -1.0, 0.0);
    } else if (abs(hitPosition.y - boxMax.y) < 0.01) {
        normal = vec3(0.0, 1.0, 0.0);
    } else if (abs(hitPosition.z - boxMin.z) < 0.01) {
        normal = vec3(0.0, 0.0, -1.0);
    } else {
        normal = vec3(0.0, 0.0, 1.0);
    }

    return true;
}

bool sceneIntersect(Ray ray, out Hit hit)
{
    hit.found = false;
    hit.t = 1e30;

    for (int chunkY = -1; chunkY <= 1; ++chunkY) {
        for (int chunkZ = -1; chunkZ <= 1; ++chunkZ) {
            for (int chunkX = -1; chunkX <= 1; ++chunkX) {
                const ivec3 chunkCoord = ivec3(chunkX, chunkY, chunkZ);
                const uint materialId = materialForChunk(chunkCoord);
                if (materialId == kMaterialAir) {
                    continue;
                }

                const vec3 boxMin = vec3(chunkCoord) * kChunkSize;
                const vec3 boxMax = boxMin + vec3(kChunkSize);

                float tHit = 0.0;
                vec3 normal = vec3(0.0);
                if (!intersectAabb(ray, boxMin, boxMax, tHit, normal) || tHit >= hit.t) {
                    continue;
                }

                hit.found = true;
                hit.t = tHit;
                hit.position = ray.origin + ray.direction * tHit;
                hit.normal = normal;
                hit.albedo = materialAlbedo(materialId);
            }
        }
    }

    return hit.found;
}

vec3 tracePath(Ray ray, inout uint rngState)
{
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);

    for (uint bounce = 0u; bounce < max(pc.maxBounces, 1u); ++bounce) {
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
        (pc.frameIndex + 1u) * 26699u;

    const vec3 forward = normalize(kCameraTarget - kCameraPosition);
    const vec3 right = normalize(cross(forward, kWorldUp));
    const vec3 up = normalize(cross(right, forward));
    const float aspectRatio = pc.resolution.x / max(pc.resolution.y, 1.0);
    const float tanHalfFov = tan(radians(kVerticalFovDegrees) * 0.5);

    vec3 color = vec3(0.0);
    const uint sampleCount = max(pc.samplesPerPixel, 1u);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex) {
        const vec2 jitter = vec2(randomFloat(rngState), randomFloat(rngState));
        vec2 ndc = ((pixel + jitter) / pc.resolution) * 2.0 - 1.0;
        ndc.x *= aspectRatio;

        Ray ray;
        ray.origin = kCameraPosition;
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