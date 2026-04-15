#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D sourceInput;
layout(set = 0, binding = 1) uniform sampler2D guideInput;

layout(push_constant) uniform PushConstants {
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

vec3 unpackNormal(vec4 guide)
{
    return normalize(guide.xyz * 2.0 - 1.0);
}

ivec2 clampedCoord(ivec2 pixelCoord, ivec2 extent)
{
    return clamp(pixelCoord, ivec2(0), extent - ivec2(1));
}

vec4 sampleColor(ivec2 pixelCoord, ivec2 extent)
{
    ivec2 coord = clampedCoord(pixelCoord, extent);
    vec2 uv = (vec2(coord) + 0.5) / vec2(extent);
    return texture(sourceInput, uv);
}

vec4 sampleGuide(ivec2 pixelCoord, ivec2 extent)
{
    ivec2 coord = clampedCoord(pixelCoord, extent);
    vec2 uv = (vec2(coord) + 0.5) / vec2(extent);
    return texture(guideInput, uv);
}

float localVariance(ivec2 pixelCoord, ivec2 extent)
{
    float firstMoment = 0.0;
    float secondMoment = 0.0;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sampleRadiance = sampleColor(pixelCoord + ivec2(x, y), extent).rgb;
            float sampleLuma = luminance(sampleRadiance);
            firstMoment += sampleLuma;
            secondMoment += sampleLuma * sampleLuma;
        }
    }

    firstMoment /= 9.0;
    secondMoment /= 9.0;
    return max(secondMoment - firstMoment * firstMoment, 0.0);
}

void main()
{
    ivec2 extent = textureSize(sourceInput, 0);
    ivec2 pixelCoord = ivec2(gl_FragCoord.xy);
    vec4 centerSample = texture(sourceInput, inUv);
    vec4 centerGuide = sampleGuide(pixelCoord, extent);
    float centerDepth = centerGuide.w;
    float centerVariance = localVariance(pixelCoord, extent) / max(centerSample.a, 1.0);
    float colorPhi = max(pc.filterParameters0.y * sqrt(centerVariance + 0.0001), 0.0001);

    vec3 accumulatedColor = vec3(0.0);
    float accumulatedWeight = 0.0;
    vec3 centerNormal = unpackNormal(centerGuide);

    for (int index = 0; index < 25; ++index) {
        ivec2 sampleCoord = clampedCoord(
            pixelCoord + kKernelOffsets[index] * int(pc.filterParameters0.x),
            extent);
        vec4 sampleRadiance = sampleColor(sampleCoord, extent);
        vec4 sampleGuideValue = sampleGuide(sampleCoord, extent);

        float weight = kKernelWeights[index];
        float colorDelta = abs(luminance(centerSample.rgb) - luminance(sampleRadiance.rgb));
        weight *= exp(-colorDelta / colorPhi);

        float sampleDepth = sampleGuideValue.w;
        if (centerDepth > 0.0 && sampleDepth > 0.0) {
            float normalWeight = pow(max(dot(centerNormal, unpackNormal(sampleGuideValue)), 0.0), pc.filterParameters0.z);
            float depthWeight = exp(-abs(sampleDepth - centerDepth) / max(pc.filterParameters0.w, 0.0001));
            weight *= normalWeight * depthWeight;
        } else if ((centerDepth > 0.0) != (sampleDepth > 0.0)) {
            weight = 0.0;
        }

        accumulatedColor += sampleRadiance.rgb * weight;
        accumulatedWeight += weight;
    }

    vec3 filteredColor = accumulatedColor / max(accumulatedWeight, 0.0001);
    outColor = vec4(filteredColor, centerSample.a);
}
