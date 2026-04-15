#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGuide;

struct GpuDirectionalLight {
    vec4 directionAndIntensity;
    vec4 color;
};

struct GpuPointLight {
    vec4 positionAndRange;
    vec4 colorAndIntensity;
};

struct GpuAreaLight {
    vec4 centerAndIntensity;
    vec4 rightExtentAndDoubleSided;
    vec4 upExtent;
    vec4 color;
};

layout(set = 0, binding = 3, std430) readonly buffer LightBuffer {
    uvec4 counts;
    GpuDirectionalLight sun;
    GpuPointLight pointLights[8];
    GpuAreaLight areaLights[8];
} lights;

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

const float kRayEpsilon = 0.02;
const float kHitThreshold = 0.035;
const float kNormalEpsilon = 0.08;
const float kMaxTraceDistance = 192.0;
const float kInvPi = 0.31830988618;
const float kTerrainHeightOffset = 8.0;
const float kTerrainTerraceScale = 0.5;
const float kTerrainTerraceHeight = 2.0;
const float kRayMarchStepMultiplier = 0.7;
const float kTerrainDistanceClamp = -4.0;
const float kTerrainBroadFrequency = 0.015;
const float kTerrainDetailFrequency = 0.055;
const float kTerrainBroadAmplitude = 42.0;
const float kTerrainRidgeFrequency = 0.45;
const float kTerrainRidgeAmplitude = 9.0;
const float kTerrainDetailAmplitude = 10.0;
const float kTreeGridSize = 6.0;
const float kTreeChanceThreshold = 0.80;
const float kRngNormalizationFactor = 4294967296.0;
const mat2 kFbmRotation = mat2(1.6, -1.2, 1.2, 1.6);
const float kDefaultSunIntensity = 6.0;
const vec2 kRngUvSeed = vec2(13.0, 17.0);
const float kRayMarchMinStep = 0.04;
const float kRayMarchMaxStep = 1.25;
const int kBinaryRefinementSteps = 6;
const vec3 kWorldUp = vec3(0.0, 1.0, 0.0);
const uint kMaterialAir = 0u;
const uint kMaterialGrass = 1u;
const uint kMaterialStone = 2u;
const uint kMaterialDirt = 3u;
const uint kMaterialSand = 4u;
const uint kMaterialSnow = 5u;
const uint kMaterialForest = 6u;

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
    uint material;
};

struct VoxelSample {
    bool occupied;
    uint material;
};

struct DistanceSample {
    float distance;
    uint material;
};

uint hash(uint state)
{
    state ^= 2747636419u;
    state *= 2654435769u;
    state ^= state >> 16u;
    state *= 2654435769u;
    state ^= state >> 16u;
    state *= 2654435769u;
    return state;
}

float hash11(float value)
{
    return float(hash(floatBitsToUint(value))) / kRngNormalizationFactor;
}

float hash21(vec2 value)
{
    uvec2 bits = floatBitsToUint(value);
    return float(hash(bits.x ^ hash(bits.y + 0x9e3779b9u))) / kRngNormalizationFactor;
}

float hash31(vec3 value)
{
    uvec3 bits = floatBitsToUint(value);
    uint state = bits.x;
    state ^= hash(bits.y + 0x9e3779b9u);
    state ^= hash(bits.z + 0x7f4a7c15u);
    return float(hash(state)) / kRngNormalizationFactor;
}

float randomFloat(inout uint state)
{
    state = hash(state);
    return float(state) / kRngNormalizationFactor;
}

float valueNoise(vec2 position)
{
    vec2 cell = floor(position);
    vec2 fractPart = fract(position);
    vec2 smoothPart = fractPart * fractPart * (3.0 - 2.0 * fractPart);

    float a = hash21(cell);
    float b = hash21(cell + vec2(1.0, 0.0));
    float c = hash21(cell + vec2(0.0, 1.0));
    float d = hash21(cell + vec2(1.0, 1.0));

    return mix(mix(a, b, smoothPart.x), mix(c, d, smoothPart.x), smoothPart.y);
}

float valueNoise(vec3 position)
{
    vec3 cell = floor(position);
    vec3 fractPart = fract(position);
    vec3 smoothPart = fractPart * fractPart * (3.0 - 2.0 * fractPart);

    float n000 = hash31(cell);
    float n100 = hash31(cell + vec3(1.0, 0.0, 0.0));
    float n010 = hash31(cell + vec3(0.0, 1.0, 0.0));
    float n110 = hash31(cell + vec3(1.0, 1.0, 0.0));
    float n001 = hash31(cell + vec3(0.0, 0.0, 1.0));
    float n101 = hash31(cell + vec3(1.0, 0.0, 1.0));
    float n011 = hash31(cell + vec3(0.0, 1.0, 1.0));
    float n111 = hash31(cell + vec3(1.0, 1.0, 1.0));

    float nx00 = mix(n000, n100, smoothPart.x);
    float nx10 = mix(n010, n110, smoothPart.x);
    float nx01 = mix(n001, n101, smoothPart.x);
    float nx11 = mix(n011, n111, smoothPart.x);
    float nxy0 = mix(nx00, nx10, smoothPart.y);
    float nxy1 = mix(nx01, nx11, smoothPart.y);
    return mix(nxy0, nxy1, smoothPart.z);
}

float fbm(vec2 position)
{
    float amplitude = 0.5;
    float value = 0.0;
    for (int octave = 0; octave < 5; ++octave) {
        value += amplitude * valueNoise(position);
        position = kFbmRotation * position;
        amplitude *= 0.5;
    }

    return value;
}

float fbm(vec3 position)
{
    float amplitude = 0.5;
    float value = 0.0;
    for (int octave = 0; octave < 4; ++octave) {
        value += amplitude * valueNoise(position);
        position = position * 2.03 + vec3(17.0, -11.0, 9.0);
        amplitude *= 0.5;
    }

    return value;
}

float sdBox(vec3 point, vec3 halfExtents)
{
    vec3 q = abs(point) - halfExtents;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

vec3 sampleCosineHemisphere(vec3 normal, inout uint rngState)
{
    float u1 = randomFloat(rngState);
    float u2 = randomFloat(rngState);
    float radius = sqrt(u1);
    float theta = 6.28318530718 * u2;

    vec3 tangentSeed = abs(normal.y) > 0.5 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(normal, tangentSeed));
    vec3 bitangent = cross(normal, tangent);
    vec3 localDirection = vec3(radius * cos(theta), sqrt(max(0.0, 1.0 - u1)), radius * sin(theta));
    return tangent * localDirection.x + normal * localDirection.y + bitangent * localDirection.z;
}

vec3 sunDirection()
{
    vec3 direction = lights.sun.directionAndIntensity.xyz;
    if (dot(direction, direction) < 1e-4) {
        return normalize(vec3(0.5, 0.85, 0.3));
    }

    return normalize(direction);
}

vec3 sunColor()
{
    vec3 color = lights.sun.color.xyz;
    if (dot(color, color) < 1e-4) {
        return vec3(1.0, 0.95, 0.84);
    }

    return color;
}

float sunIntensity()
{
    return max(lights.sun.directionAndIntensity.w, kDefaultSunIntensity);
}

vec3 skyBaseColor(vec3 direction)
{
    float horizon = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 zenith = vec3(0.34, 0.55, 0.88);
    vec3 horizonColor = vec3(0.82, 0.70, 0.56);
    vec3 groundBounce = vec3(0.16, 0.12, 0.10);
    vec3 sky = mix(horizonColor, zenith, horizon * horizon);
    sky += sunColor() * sunIntensity() * pow(max(dot(direction, sunDirection()), 0.0), 96.0);
    if (direction.y < 0.0) {
        sky = mix(sky, groundBounce, clamp(-direction.y * 1.5, 0.0, 1.0));
    }

    return sky;
}

vec3 materialAlbedo(uint materialId, vec3 position)
{
    float variation = 0.88 + 0.12 * valueNoise(position * 0.15);

    if (materialId == kMaterialGrass) {
        return vec3(0.30, 0.58, 0.21) * variation;
    }
    if (materialId == kMaterialDirt) {
        return vec3(0.48, 0.34, 0.20) * variation;
    }
    if (materialId == kMaterialSand) {
        return vec3(0.80, 0.72, 0.50) * variation;
    }
    if (materialId == kMaterialSnow) {
        return vec3(0.92, 0.94, 0.98) * variation;
    }
    if (materialId == kMaterialForest) {
        return vec3(0.14, 0.34, 0.12) * variation;
    }

    return vec3(0.52, 0.52, 0.56) * variation;
}

float terrainHeight(vec2 xz)
{
    vec2 broadCoord = xz * kTerrainBroadFrequency;
    vec2 detailCoord = xz * kTerrainDetailFrequency;
    float broad = fbm(broadCoord) * kTerrainBroadAmplitude;
    float ridge = (1.0 - abs(fbm(detailCoord * kTerrainRidgeFrequency + vec2(9.0, -4.0)) * 2.0 - 1.0)) *
        kTerrainRidgeAmplitude;
    float detail = fbm(detailCoord + vec2(4.0, 11.0)) * kTerrainDetailAmplitude;
    float terraces = floor((broad + ridge + detail - kTerrainHeightOffset) *
        kTerrainTerraceScale) * kTerrainTerraceHeight;
    return terraces;
}

bool carveCave(ivec3 cell)
{
    if (cell.y > 18 || cell.y < -36) {
        return false;
    }

    float density = fbm(vec3(cell) * vec3(0.09, 0.12, 0.09) + vec3(17.0, 3.0, -11.0));
    density += 0.45 * valueNoise(vec3(cell) * vec3(0.18, 0.09, 0.18) + vec3(-9.0, 5.0, 2.0));
    return density > 0.88;
}

VoxelSample sampleVoxel(ivec3 cell)
{
    float height = terrainHeight(vec2(cell.x, cell.z));
    if (float(cell.y) <= height) {
        if (!carveCave(cell)) {
            uint materialId = kMaterialStone;
            float depth = height - float(cell.y);
            if (height < -2.0 && depth < 2.5) {
                materialId = kMaterialSand;
            } else if (height > 26.0 && depth < 2.0) {
                materialId = kMaterialSnow;
            } else if (depth < 1.5) {
                materialId = kMaterialGrass;
            } else if (depth < 4.5) {
                materialId = kMaterialDirt;
            }
            return VoxelSample(true, materialId);
        }
    }

    vec2 groveCell = floor(vec2(cell.x, cell.z) / kTreeGridSize);
    float treeChance = hash21(groveCell + vec2(0.73, 4.12));
    if (treeChance > kTreeChanceThreshold) {
        vec2 treeOrigin = floor(groveCell * kTreeGridSize + vec2(
            floor(hash21(groveCell + vec2(2.1, 7.4)) * 4.0) + 1.0,
            floor(hash21(groveCell + vec2(8.3, 1.7)) * 4.0) + 1.0));
        int treeBase = int(floor(terrainHeight(treeOrigin)));
        int trunkHeight = 3 + int(floor(hash21(groveCell + vec2(5.8, 9.2)) * 4.0));
        ivec2 localXZ = cell.xz - ivec2(treeOrigin);
        if (all(equal(localXZ, ivec2(0))) && cell.y > treeBase && cell.y <= treeBase + trunkHeight) {
            return VoxelSample(true, kMaterialDirt);
        }

        ivec3 crownCenter = ivec3(int(treeOrigin.x), treeBase + trunkHeight + 1, int(treeOrigin.y));
        ivec3 crownDelta = abs(cell - crownCenter);
        if (crownDelta.y <= 1 && crownDelta.x <= 2 && crownDelta.z <= 2 &&
            crownDelta.x + crownDelta.y + crownDelta.z <= 4) {
            return VoxelSample(true, kMaterialForest);
        }
    }

    vec2 rockCell = floor(vec2(cell.x, cell.z) / 10.0);
    float rockChance = hash21(rockCell + vec2(3.8, 1.9));
    if (rockChance > 0.88) {
        vec2 rockOrigin = floor(rockCell * 10.0 + vec2(
            floor(hash21(rockCell + vec2(6.3, 2.5)) * 6.0) + 2.0,
            floor(hash21(rockCell + vec2(1.1, 8.6)) * 6.0) + 2.0));
        int rockBase = int(floor(terrainHeight(rockOrigin)));
        int rockHeight = 2 + int(floor(hash21(rockCell + vec2(7.6, 4.4)) * 4.0));
        ivec2 rockDelta = abs(cell.xz - ivec2(rockOrigin));
        if (rockDelta.x <= 1 && rockDelta.y <= 1 && cell.y > rockBase && cell.y <= rockBase + rockHeight) {
            return VoxelSample(true, kMaterialStone);
        }
    }

    return VoxelSample(false, kMaterialAir);
}

DistanceSample sceneDistance(vec3 position)
{
    ivec3 cell = ivec3(floor(position));
    float bestDistance = 1e6;
    uint bestMaterialId = kMaterialAir;

    float localHeight = terrainHeight(floor(position.xz));
    bestDistance = min(bestDistance, max(position.y - (localHeight + 1.0), kTerrainDistanceClamp));

    for (int offsetZ = -1; offsetZ <= 1; ++offsetZ) {
        for (int offsetY = -2; offsetY <= 2; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                ivec3 neighbor = cell + ivec3(offsetX, offsetY, offsetZ);
                VoxelSample voxel = sampleVoxel(neighbor);
                if (!voxel.occupied) {
                    continue;
                }

                float distanceToVoxel = sdBox(position - (vec3(neighbor) + vec3(0.5)), vec3(0.5));
                if (distanceToVoxel < bestDistance) {
                    bestDistance = distanceToVoxel;
                    bestMaterialId = voxel.material;
                }
            }
        }
    }

    return DistanceSample(bestDistance, bestMaterialId);
}

float sceneDistanceValue(vec3 position)
{
    return sceneDistance(position).distance;
}

vec3 estimateNormal(vec3 position)
{
    vec2 offset = vec2(1.0, -1.0) * kNormalEpsilon;
    return normalize(
        offset.xyy * sceneDistanceValue(position + offset.xyy) +
        offset.yyx * sceneDistanceValue(position + offset.yyx) +
        offset.yxy * sceneDistanceValue(position + offset.yxy) +
        offset.xxx * sceneDistanceValue(position + offset.xxx));
}

bool sceneIntersect(Ray ray, float maxDistance, out Hit hit)
{
    hit.found = false;
    hit.t = maxDistance;

    uint maxRaySteps = max(pc.settings.w, 32u);
    float t = 0.0;
    float previousT = 0.0;
    DistanceSample marchSample = DistanceSample(1e6, kMaterialAir);

    for (uint stepIndex = 0u; stepIndex < maxRaySteps && t < maxDistance; ++stepIndex) {
        vec3 position = ray.origin + ray.direction * t;
        marchSample = sceneDistance(position);
        if (marchSample.distance < kHitThreshold) {
            float lower = previousT;
            float upper = t;
            for (int refineStep = 0; refineStep < kBinaryRefinementSteps; ++refineStep) {
                float mid = 0.5 * (lower + upper);
                if (sceneDistanceValue(ray.origin + ray.direction * mid) < kHitThreshold) {
                    upper = mid;
                } else {
                    lower = mid;
                }
            }

            hit.found = true;
            hit.t = upper;
            hit.position = ray.origin + ray.direction * hit.t;
            hit.normal = estimateNormal(hit.position);
            hit.material = marchSample.material;
            hit.albedo = materialAlbedo(hit.material, hit.position);
            return true;
        }

        previousT = t;
        t += clamp(
            marchSample.distance * kRayMarchStepMultiplier,
            kRayMarchMinStep,
            kRayMarchMaxStep);
    }

    return false;
}

bool isShadowed(vec3 origin, vec3 direction, float maxDistance)
{
    Ray shadowRay;
    shadowRay.origin = origin;
    shadowRay.direction = direction;

    Hit shadowHit;
    if (!sceneIntersect(shadowRay, maxDistance, shadowHit)) {
        return false;
    }

    return shadowHit.t < maxDistance - kRayEpsilon * 2.0;
}

vec3 evaluateSunLight(Hit hit)
{
    float ndotl = max(dot(hit.normal, sunDirection()), 0.0);
    if (ndotl <= 0.0) {
        return vec3(0.0);
    }

    if (isShadowed(hit.position + hit.normal * kRayEpsilon, sunDirection(), kMaxTraceDistance)) {
        return vec3(0.0);
    }

    return sunColor() * sunIntensity() * ndotl;
}

vec3 evaluatePointLight(Hit hit, GpuPointLight light)
{
    vec3 toLight = light.positionAndRange.xyz - hit.position;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1e-4) {
        return vec3(0.0);
    }

    float distanceToLight = sqrt(distanceSquared);
    float lightRange = light.positionAndRange.w;
    if (distanceToLight >= lightRange) {
        return vec3(0.0);
    }

    vec3 lightDirection = toLight / distanceToLight;
    float ndotl = max(dot(hit.normal, lightDirection), 0.0);
    if (ndotl <= 0.0) {
        return vec3(0.0);
    }

    float rangeFade = pow(clamp(1.0 - distanceToLight / max(lightRange, 0.001), 0.0, 1.0), 2.0);
    if (rangeFade <= 0.0) {
        return vec3(0.0);
    }

    if (isShadowed(hit.position + hit.normal * kRayEpsilon, lightDirection, distanceToLight)) {
        return vec3(0.0);
    }

    return light.colorAndIntensity.xyz * light.colorAndIntensity.w * rangeFade * ndotl /
        max(distanceSquared, 1.0);
}

vec3 evaluateAreaLight(Hit hit, GpuAreaLight light, inout uint rngState)
{
    float sampleU = randomFloat(rngState) * 2.0 - 1.0;
    float sampleV = randomFloat(rngState) * 2.0 - 1.0;
    vec3 rightExtent = light.rightExtentAndDoubleSided.xyz;
    vec3 upExtent = light.upExtent.xyz;
    vec3 lightSample = light.centerAndIntensity.xyz + rightExtent * sampleU + upExtent * sampleV;
    vec3 toLight = lightSample - hit.position;
    float distanceSquared = dot(toLight, toLight);
    if (distanceSquared <= 1e-4) {
        return vec3(0.0);
    }

    float distanceToLight = sqrt(distanceSquared);
    vec3 lightDirection = toLight / distanceToLight;
    float surfaceCosine = max(dot(hit.normal, lightDirection), 0.0);
    if (surfaceCosine <= 0.0) {
        return vec3(0.0);
    }

    vec3 lightNormal = normalize(cross(rightExtent, upExtent));
    float lightCosine = dot(lightNormal, -lightDirection);
    if (light.rightExtentAndDoubleSided.w >= 0.5) {
        lightCosine = abs(lightCosine);
    }
    if (lightCosine <= 0.0) {
        return vec3(0.0);
    }

    if (isShadowed(hit.position + hit.normal * kRayEpsilon, lightDirection, distanceToLight)) {
        return vec3(0.0);
    }

    float lightArea = 4.0 * length(cross(rightExtent, upExtent));
    return light.color.xyz * light.centerAndIntensity.w * lightArea * surfaceCosine * lightCosine /
        max(distanceSquared, 1.0);
}

vec3 evaluateDirectLighting(Hit hit, inout uint rngState)
{
    vec3 directLighting = evaluateSunLight(hit);

    uint pointLightCount = min(lights.counts.x, 8u);
    for (uint lightIndex = 0u; lightIndex < pointLightCount; ++lightIndex) {
        directLighting += evaluatePointLight(hit, lights.pointLights[lightIndex]);
    }

    uint areaLightCount = min(lights.counts.y, 8u);
    for (uint lightIndex = 0u; lightIndex < areaLightCount; ++lightIndex) {
        directLighting += evaluateAreaLight(hit, lights.areaLights[lightIndex], rngState);
    }

    return directLighting;
}

vec3 tracePath(Ray ray, inout uint rngState)
{
    vec3 radiance = vec3(0.0);
    vec3 throughput = vec3(1.0);
    uint bounceCount = max(pc.settings.z, 1u);

    for (uint bounce = 0u; bounce < bounceCount; ++bounce) {
        Hit hit;
        if (!sceneIntersect(ray, kMaxTraceDistance, hit)) {
            radiance += throughput * skyBaseColor(ray.direction);
            break;
        }

        radiance += throughput * hit.albedo * evaluateDirectLighting(hit, rngState) * kInvPi;
        throughput *= hit.albedo;

        if (bounce >= 2u) {
            float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.12, 0.95);
            if (randomFloat(rngState) > survival) {
                break;
            }
            throughput /= survival;
        }

        ray.origin = hit.position + hit.normal * kRayEpsilon;
        ray.direction = sampleCosineHemisphere(hit.normal, rngState);
    }

    return radiance;
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec4 primarySurfaceGuide(
    vec2 pixel,
    vec3 forward,
    vec3 right,
    vec3 up,
    float aspectRatio,
    float tanHalfFov)
{
    vec2 ndc = (pixel / pc.frameData.xy) * 2.0 - 1.0;
    ndc.x *= aspectRatio;
    ndc.y = -ndc.y;

    Ray ray;
    ray.origin = pc.cameraPosition.xyz;
    ray.direction = normalize(forward + right * (ndc.x * tanHalfFov) + up * (ndc.y * tanHalfFov));

    Hit hit;
    if (sceneIntersect(ray, kMaxTraceDistance, hit)) {
        return vec4(hit.normal * 0.5 + 0.5, hit.t);
    }

    return vec4(normalize(ray.direction) * 0.5 + 0.5, 0.0);
}

void main()
{
    vec2 pixel = gl_FragCoord.xy;
    uint rngState =
        uint(pixel.x) * 1973u ^
        uint(pixel.y) * 9277u ^
        (pc.settings.x + 1u) * 26699u ^
        uint(hash11(float(pc.settings.x) + dot(inUv, kRngUvSeed)) * kRngNormalizationFactor);

    vec3 forward = normalize(pc.cameraForward.xyz);
    vec3 worldUp = abs(forward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : kWorldUp;
    vec3 right = normalize(cross(forward, worldUp));
    vec3 up = normalize(cross(right, forward));
    float aspectRatio = max(pc.frameData.w, 0.01);
    float tanHalfFov = tan(radians(max(pc.frameData.z, 1.0)) * 0.5);

    outGuide = primarySurfaceGuide(pixel, forward, right, up, aspectRatio, tanHalfFov);

    vec3 color = vec3(0.0);
    float luminanceSum = 0.0;
    float luminanceSquaredSum = 0.0;
    uint sampleCount = max(pc.settings.y, 1u);
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex) {
        vec2 jitter = vec2(randomFloat(rngState), randomFloat(rngState));
        vec2 ndc = ((pixel + jitter) / pc.frameData.xy) * 2.0 - 1.0;
        ndc.x *= aspectRatio;
        ndc.y = -ndc.y;

        Ray ray;
        ray.origin = pc.cameraPosition.xyz;
        ray.direction = normalize(forward + right * (ndc.x * tanHalfFov) + up * (ndc.y * tanHalfFov));

        vec3 sampleRadiance = tracePath(ray, rngState);
        color += sampleRadiance;

        float sampleLuminance = luminance(sampleRadiance);
        luminanceSum += sampleLuminance;
        luminanceSquaredSum += sampleLuminance * sampleLuminance;
    }

    color /= float(sampleCount);
    float meanLuminance = luminanceSum / float(sampleCount);
    float variance = max(
        luminanceSquaredSum / float(sampleCount) - meanLuminance * meanLuminance,
        0.0);
    outColor = vec4(color, variance);
}
