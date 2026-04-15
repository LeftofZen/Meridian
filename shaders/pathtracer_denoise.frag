#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D hdrPathTrace;
layout(set = 0, binding = 1) uniform sampler2D guideBuffer;

layout(push_constant) uniform PushConstants {
    uvec4 toggles;
    vec4 filterParameters0;
} pc;

const ivec2 kKernelOffsets[25] = ivec2[](
    ivec2(-2, -2),
    ivec2(-1, -2),
    ivec2(0, -2),
    ivec2(1, -2),
    ivec2(2, -2),
    ivec2(-2, -1),
    ivec2(-1, -1),
    ivec2(0, -1),
    ivec2(1, -1),
    ivec2(2, -1),
    ivec2(-2, 0),
    ivec2(-1, 0),
    ivec2(0, 0),
    ivec2(1, 0),
    ivec2(2, 0),
    ivec2(-2, 1),
    ivec2(-1, 1),
    ivec2(0, 1),
    ivec2(1, 1),
    ivec2(2, 1),
    ivec2(-2, 2),
    ivec2(-1, 2),
    ivec2(0, 2),
    ivec2(1, 2),
    ivec2(2, 2));

const float kKernelWeights[25] = float[](
    1.0 / 256.0,
    1.0 / 64.0,
    3.0 / 128.0,
    1.0 / 64.0,
    1.0 / 256.0,
    1.0 / 64.0,
    1.0 / 16.0,
    3.0 / 32.0,
    1.0 / 16.0,
    1.0 / 64.0,
    3.0 / 128.0,
    3.0 / 32.0,
    9.0 / 64.0,
    3.0 / 32.0,
    3.0 / 128.0,
    1.0 / 64.0,
    1.0 / 16.0,
    3.0 / 32.0,
    1.0 / 16.0,
    1.0 / 64.0,
    1.0 / 256.0,
    1.0 / 64.0,
    3.0 / 128.0,
    1.0 / 64.0,
    1.0 / 256.0);

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toneMapAces(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 sampleHdr(ivec2 pixelCoord, ivec2 extent)
{
    const ivec2 clampedCoord = clamp(pixelCoord, ivec2(0), extent - ivec2(1));
    const vec2 uv = (vec2(clampedCoord) + 0.5) / vec2(extent);
    return texture(hdrPathTrace, uv).rgb;
}

vec4 sampleGuide(ivec2 pixelCoord, ivec2 extent)
{
    const ivec2 clampedCoord = clamp(pixelCoord, ivec2(0), extent - ivec2(1));
    const vec2 uv = (vec2(clampedCoord) + 0.5) / vec2(extent);
    return texture(guideBuffer, uv);
}

ivec2 steppedSampleCoord(ivec2 pixelCoord, ivec2 offset, float kernelStep, ivec2 extent)
{
    const vec2 sampleCoord = vec2(pixelCoord) + vec2(offset) * kernelStep;
    return clamp(ivec2(floor(sampleCoord + 0.5)), ivec2(0), extent - ivec2(1));
}

vec3 presentColor(vec3 hdrColor)
{
    vec3 color = toneMapAces(hdrColor);
    color = pow(color, vec3(1.0 / 2.2));
    return color;
}

vec3 debugDifference(vec3 rawHdr, vec3 denoisedHdr, float differenceGain)
{
    const vec3 hdrDifference = abs(rawHdr - denoisedHdr) * differenceGain;
    return clamp(hdrDifference, vec3(0.0), vec3(1.0));
}

void main()
{
    const ivec2 extent = textureSize(hdrPathTrace, 0);
    const ivec2 pixelCoord = ivec2(gl_FragCoord.xy);
    const vec3 center = texture(hdrPathTrace, inUv).rgb;

    if (pc.toggles.x == 0u) {
        outColor = vec4(presentColor(center), 1.0);
        return;
    }

    const float kernelStep = pc.filterParameters0.x;
    const float colorPhi = pc.filterParameters0.y;
    const float normalPhi = pc.filterParameters0.z;
    const float differenceGain = pc.filterParameters0.w;
    const uint debugView = pc.toggles.y;

    const vec4 centerGuide = sampleGuide(pixelCoord, extent);

    vec3 accumulatedColor = vec3(0.0);
    float accumulatedWeight = 0.0;
    for (int index = 0; index < 25; ++index) {
        const ivec2 sampleCoord = steppedSampleCoord(pixelCoord, kKernelOffsets[index], kernelStep, extent);
        const vec3 sampleColor = sampleHdr(sampleCoord, extent);
        const vec3 colorDelta = center - sampleColor;
        const float colorDistanceSquared = dot(colorDelta, colorDelta);
        const float colorWeight = min(exp(-colorDistanceSquared / max(colorPhi, 0.0001)), 1.0);

        const vec4 sampleGuideValue = sampleGuide(sampleCoord, extent);
        const vec4 guideDelta = centerGuide - sampleGuideValue;
        const float normalDistanceSquared = max(dot(guideDelta, guideDelta), 0.0);
        const float normalWeight = min(exp(-normalDistanceSquared / max(normalPhi, 0.0001)), 1.0);

        const float weight = colorWeight * normalWeight * kKernelWeights[index];
        accumulatedColor += sampleColor * weight;
        accumulatedWeight += weight;
    }

    const vec3 denoised = accumulatedColor / max(accumulatedWeight, 0.0001);

    if (debugView == 1u) {
        outColor = vec4(presentColor(center), 1.0);
        return;
    }

    if (debugView == 2u) {
        outColor = vec4(debugDifference(center, denoised, differenceGain), 1.0);
        return;
    }

    if (debugView == 3u) {
        const bool showRaw = inUv.x < 0.5;
        outColor = vec4(showRaw ? presentColor(center) : presentColor(denoised), 1.0);
        return;
    }

    outColor = vec4(presentColor(denoised), 1.0);
}