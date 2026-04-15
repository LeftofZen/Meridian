#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D rawPathTraceInput;
layout(set = 0, binding = 1) uniform sampler2D filteredPathTraceInput;

layout(push_constant) uniform PushConstants {
    uvec4 toggles;
    vec4 filterParameters0;
} pc;

vec3 toneMapAces(vec3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

vec3 presentColor(vec3 hdrColor)
{
    vec3 color = toneMapAces(hdrColor);
    return pow(color, vec3(1.0 / 2.2));
}

vec3 debugDifference(vec3 rawHdr, vec3 filteredHdr, float differenceGain)
{
    vec3 hdrDifference = abs(rawHdr - filteredHdr) * differenceGain;
    return clamp(hdrDifference, vec3(0.0), vec3(1.0));
}

void main()
{
    vec3 rawColor = texture(rawPathTraceInput, inUv).rgb;
    vec3 filteredColor = texture(filteredPathTraceInput, inUv).rgb;
    uint debugView = pc.toggles.y;

    if (pc.toggles.x == 0u) {
        outColor = vec4(presentColor(rawColor), 1.0);
        return;
    }

    if (debugView == 1u) {
        outColor = vec4(presentColor(rawColor), 1.0);
        return;
    }

    if (debugView == 2u) {
        outColor = vec4(debugDifference(rawColor, filteredColor, pc.filterParameters0.x), 1.0);
        return;
    }

    if (debugView == 3u) {
        bool showRaw = inUv.x < 0.5;
        outColor = vec4(showRaw ? presentColor(rawColor) : presentColor(filteredColor), 1.0);
        return;
    }

    outColor = vec4(presentColor(filteredColor), 1.0);
}