#version 460

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D currentRadianceInput;
layout(set = 0, binding = 1) uniform sampler2D currentGuideInput;
layout(set = 0, binding = 2) uniform sampler2D historyRadianceInput;
layout(set = 0, binding = 3) uniform sampler2D historyGuideInput;

layout(push_constant) uniform PushConstants {
    uvec4 toggles;
    vec4 temporalParameters0;
    vec4 currentFrameData;
    vec4 currentCameraPosition;
    vec4 currentCameraForward;
    vec4 previousFrameData;
    vec4 previousCameraPosition;
    vec4 previousCameraForward;
} pc;

const vec3 kWorldUp = vec3(0.0, 1.0, 0.0);
const float kEpsilon = 0.0001;

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

mat3 cameraBasis(vec3 forward)
{
    vec3 basisForward = normalize(forward);
    vec3 worldUp = abs(basisForward.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : kWorldUp;
    vec3 right = normalize(cross(basisForward, worldUp));
    vec3 up = normalize(cross(right, basisForward));
    return mat3(right, up, basisForward);
}

vec3 rayDirection(vec2 uv, vec4 frameData, vec3 forward)
{
    vec2 ndc = uv * 2.0 - 1.0;
    ndc.x *= max(frameData.w, 0.01);
    ndc.y = -ndc.y;

    float tanHalfFov = tan(radians(max(frameData.z, 1.0)) * 0.5);
    mat3 basis = cameraBasis(forward);
    return normalize(
        basis[2] +
        basis[0] * (ndc.x * tanHalfFov) +
        basis[1] * (ndc.y * tanHalfFov));
}

bool projectWorldPosition(vec3 worldPosition, vec4 frameData, vec3 cameraPosition, vec3 cameraForward, out vec2 uv)
{
    mat3 basis = cameraBasis(cameraForward);
    vec3 relative = worldPosition - cameraPosition;
    float forwardDistance = dot(relative, basis[2]);
    if (forwardDistance <= kEpsilon) {
        uv = vec2(0.0);
        return false;
    }

    float tanHalfFov = tan(radians(max(frameData.z, 1.0)) * 0.5);
    float aspectRatio = max(frameData.w, 0.01);
    vec2 ndc;
    ndc.x = dot(relative, basis[0]) / (forwardDistance * tanHalfFov * aspectRatio);
    ndc.y = -dot(relative, basis[1]) / (forwardDistance * tanHalfFov);
    uv = ndc * 0.5 + 0.5;
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

vec3 worldPositionFromGuide(vec2 uv, vec4 frameData, vec3 cameraPosition, vec3 cameraForward, float hitDistance)
{
    vec3 direction = rayDirection(uv, frameData, cameraForward);
    return cameraPosition + direction * hitDistance;
}

vec3 unpackNormal(vec4 guide)
{
    return normalize(guide.xyz * 2.0 - 1.0);
}

void main()
{
    vec4 currentRadiance = texture(currentRadianceInput, inUv);
    vec4 currentGuide = texture(currentGuideInput, inUv);
    float currentDepth = currentGuide.w;

    vec3 resolvedRadiance = currentRadiance.rgb;
    float historyAge = 1.0;

    if (pc.toggles.x != 0u && currentDepth > 0.0) {
        vec3 currentWorldPosition = worldPositionFromGuide(
            inUv,
            pc.currentFrameData,
            pc.currentCameraPosition.xyz,
            pc.currentCameraForward.xyz,
            currentDepth);

        vec2 previousUv;
        if (projectWorldPosition(
                currentWorldPosition,
                pc.previousFrameData,
                pc.previousCameraPosition.xyz,
                pc.previousCameraForward.xyz,
                previousUv)) {
            vec4 previousGuide = texture(historyGuideInput, previousUv);
            if (previousGuide.w > 0.0) {
                vec3 previousWorldPosition = worldPositionFromGuide(
                    previousUv,
                    pc.previousFrameData,
                    pc.previousCameraPosition.xyz,
                    pc.previousCameraForward.xyz,
                    previousGuide.w);
                float depthDelta = length(previousWorldPosition - currentWorldPosition);
                float normalAgreement = dot(unpackNormal(currentGuide), unpackNormal(previousGuide));

                if (normalAgreement > 0.85 &&
                    depthDelta < max(pc.temporalParameters0.z, currentDepth * 0.05)) {
                    vec4 historyRadiance = texture(historyRadianceInput, previousUv);
                    float clampedHistoryAge = clamp(historyRadiance.a, 1.0, pc.temporalParameters0.y);
                    float currentWeight = max(pc.temporalParameters0.x, 1.0 / (clampedHistoryAge + 1.0));
                    float varianceClamp = sqrt(max(currentRadiance.a, 0.0)) * 3.0 + 0.25;
                    vec3 clampedHistory = clamp(
                        historyRadiance.rgb,
                        currentRadiance.rgb - vec3(varianceClamp),
                        currentRadiance.rgb + vec3(varianceClamp));
                    resolvedRadiance = mix(clampedHistory, currentRadiance.rgb, currentWeight);
                    historyAge = min(clampedHistoryAge + 1.0, pc.temporalParameters0.y);
                }
            }
        }
    }

    outColor = vec4(resolvedRadiance, historyAge);
}
