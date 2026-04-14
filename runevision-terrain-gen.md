# `Image` shader:
/*
=====================================================================================

See Buffer A for information on the erosion technique.

The Image buffer handles raymarched terrain rendering based on the heightmap from
Buffer A and the noise texture from Buffer B.

Terrain rendering derived from https://www.shadertoy.com/view/7ljcRW by Fewes.

A few of the changes implemented:

 - Normals are calculated here rather than in Buffer A, so we only have to sample the
   buffer multiple times per pixel, rather than running the whole heightmap function.

 - The raymarching runs a cheaper map function which only returns the height, whereas
   the full map function is only called once, when the data for shading is needed.

 - Multiple different data layers are transferred from Buffer A, using packing.
 
 - Rendering of drainage patterns and trees, plus various material pattern tweaks.
 
 - Fix for rendering of cut-off of water at the far end.
 
 - Support for more debug buffers being displayed.

=====================================================================================
*/


// -----------------------------------------------------------------------------
// Functions for retrieving map data from Buffer A and B
// -----------------------------------------------------------------------------

// Get the map channels from Buffer A, except the packed data.
// This is cheaper for usages that don't need the packed data anyway.
vec3 GetChannel0(vec2 uv) {
    uv *= BUFFER_SIZE / iResolution.xy;
    return texture(iChannel0, uv).xyz;
}

// Get detail texture from Buffer B.
vec4 GetChannel1(vec2 uv) {
    uv *= BUFFER_SIZE / iResolution.xy;
    return texture(iChannel1, uv);
}

// Get map channels from Buffer A, including the packed data.
vec3 GetChannel0Data(vec2 uv, out vec4 data) {
    vec2 p = uv * BUFFER_SIZE - 0.5;
    
    ivec2 i = ivec2(p);
    vec2 b = fract(p);
    vec2 a = 1.0 - b;
    
    // Get the four texels directly, rather than an interpolated result.
    vec4 AA = texelFetch(iChannel0, i, 0);
    vec4 AB = texelFetch(iChannel0, i + ivec2(0, 1), 0);
    vec4 BA = texelFetch(iChannel0, i + ivec2(1, 0), 0);
    vec4 BB = texelFetch(iChannel0, i + ivec2(1, 1), 0);
    // Use regular interpolation for the normal return value.
    // The w channel should not be used.
    vec4 ret = (AA * a.y + AB * b.y) * a.x + (BA * a.y + BB * b.y) * b.x;
    
    // Unpack each of the four samples into a vec4.
    vec4 AAdata = unpack4(AA.w);
    vec4 ABdata = unpack4(AB.w);
    vec4 BAdata = unpack4(BA.w);
    vec4 BBdata = unpack4(BB.w);
    // Interpolate the four vec4s so each piece of data is correctly interpolated.
    data = (AAdata * a.y + ABdata * b.y) * a.x + (BAdata * a.y + BBdata * b.y) * b.x;
    
    return ret.xyz;
}

vec2 GetUV(vec3 p) {
    vec2 pixel = vec2(1.0) / BUFFER_SIZE;
    vec2 uv = p.xz * (1.0 - pixel * 2.0) + vec2(0.5);
    uv = clamp(uv, pixel, vec2(1.0) - pixel);
    uv += TIME_SCROLL_OFFSET_FRAC;
    return uv;
}

// Get just the map height. This is faster for ray marching.
float mapHeight(vec2 uv) {
    return GetChannel0(uv).x;
}

// Get all map channels. This is needed for texturing and shading.
vec4 map(vec2 uv, out float erosion, out float ridgemap, out float trees, out float debug) {
    vec4 data;
    vec3 tex = GetChannel0Data(uv, data);
    
    float height = tex.x;
    
    // Calculate an accurate normal from the neighbouring points.
    vec2 uv1 = uv + vec2(1.0 / BUFFER_SIZE.x, 0.0);
    vec2 uv2 = uv + vec2(0.0, 1.0 / BUFFER_SIZE.y);
    float h1 = GetChannel0(uv1).x;
    float h2 = GetChannel0(uv2).x;
    vec3 v1 = vec3(uv1 - uv, (h1 - height));
    vec3 v2 = vec3(uv2 - uv, (h2 - height));
    vec3 normal = normalize(cross(v1, v2)).xzy;
    
    erosion = data.x * 2.0 - 1.0;
    ridgemap = data.y;
    trees = data.z;
    debug = data.w;
    
    return vec4(height, normal);
}


// -----------------------------------------------------------------------------
// Ray marching the terrain height function
// -----------------------------------------------------------------------------

// The box size the terrain is contained inside.
vec3 boxSize = vec3(0.5, 1.0, 0.5);

float march(vec3 ro, vec3 rd, out vec3 normal, out int material, out float s_t) {
    s_t = 9999.0;
    
    vec3 boxNormal;
    vec2 box = boxIntersection(ro, rd, boxSize, boxNormal);
    
    float tStart = max(0.0, box.x) + 1e-2;
    float tEnd = box.y - 1e-2;
    
    material = M_GROUND;

    float stepSize = 0.0;
    float stepScale = 1.0 / RAYMARCH_QUALITY;
    int samples = int(48.0 * RAYMARCH_QUALITY);
    float t = tStart;
    float altitude = 0.0;
    for (int i = 0; i < samples; i++) {
        vec3 pos = ro + rd * t;
        
        float h = mapHeight(GetUV(pos));
        altitude = pos.y - h;
        
        s_t = max(0.0, min(s_t, altitude / t));
        
        if (altitude < 0.0) {
            if (i < 1) { // Sides
                if (pos.y < 0.35) { // Flat bottom
                    s_t = 9999.0;
                    return -1.0;
                }
                normal = boxNormal;
                material = M_STRATA;
                break;
            }
        }
        
        if (altitude < 0.0) {
            // Step back (contact/edge refinement).
            stepScale *= 0.5;
            t -= stepSize * stepScale;
        }
        else {
            // Step forward
            // Accelerate the ray by distance to terrain.
            // This would result in horrible aliasing if we didn't do refinement above.
            //stepSize = abs(altitude) + 1e-2;
            stepSize = abs(altitude) + min(1e-2, abs(altitude) * 0.01);
            //stepSize = (tEnd - tStart) / float(stepCount);
            t += stepSize * stepScale;
        }
    }
    
    #ifdef WATER
        vec3 waterNormal;
        vec2 water = boxIntersection(ro, rd, vec3(boxSize.x, WATER_HEIGHT, boxSize.z), waterNormal);
        if ((water.y > 0.0 && (water.x < t || t < 0.0)) && material != M_STRATA) {
            t = max(0.0, water.x);
            normal = waterNormal;
            material = M_WATER;
        }
    #endif    

    if (box.y < 0.0) {
        s_t = 9999.0;
        return -1.0;
    }
    
    if (t > tEnd) {
        return -1.0;
    }

    return t;
}

vec3 GetReflection(vec3 p, vec3 r, vec3 sun, float smoothness) {
    vec3 refl = SkyColor(r, sun) * 4.0;
    vec3 foo;
    float r_t;
    int r_material;
    march(p, r, foo, r_material, r_t);
    return refl * (1.0 - exp(-r_t * 10.0 * sq(smoothness)));
}


// -----------------------------------------------------------------------------
// Main image output
// -----------------------------------------------------------------------------

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    #ifdef SHOW_BUFFER
        float debugCount = 4.0;
        if (SHOW_BUFFER_NR > 0)
            debugCount = 1.0;
        float debugWidth = iResolution.y / debugCount;
    #else
        float debugCount = 0.0;
        float debugWidth = 0.0;
    #endif
    
    
    // ------------------------------------------------------------------------
    // Set up camera
    // ------------------------------------------------------------------------
    
    vec3 ro;
    vec3 rd;
    GetRay(ro, rd, iTime, iMouse, iResolution, fragCoord, debugWidth);
    
    
    // ------------------------------------------------------------------------
    // Ray march
    // ------------------------------------------------------------------------
    
    vec4 foo;
    vec3 normal;
    int material;
    float t = march(ro, rd, normal, material, foo.w);
    
    
    // ------------------------------------------------------------------------
    // Coloring and shading
    // ------------------------------------------------------------------------
    
    #ifdef FIXED_SUN
        vec3 sun = normalize(vec3(-1.0, 0.4, 0.05));
    #else
        vec3 sun = rot * normalize(vec3(-1.0, 0.15, 0.25));
    #endif
    
    vec3 fogColor = 1.0 - exp(-SkyColor(rd, sun) * 2.0);
    
    vec3 color;
    
    if (t < 0.0) {
        // Sky
        color = fogColor * (1.0 + pow(fragCoord.y / iResolution.y, 3.0) * 3.0) * 0.5;
        #ifdef SHOW_NORMALS
            color = vec3(0.5, 0.5, 1.0);
        #endif
    }
    else {
        vec3 pos = ro + rd * t;
        
        float erosion;
        float ridgemap;
        float trees;
        float debug;
        vec4 mapData = map(GetUV(pos), erosion, ridgemap, trees, debug);
        float drainage = clamp01((1.0 - clamp01(ridgemap / DRAINAGE_WIDTH)) * 1.5);
        float diff = pos.y - mapData.x;
        
        float breakup = 0.0;
        #ifdef DETAIL_TEXTURE
            vec4 breakupTex = GetChannel1(GetUV(pos));
            breakup = breakupTex.x;
            if (material == M_WATER) {
                normal.xz += breakupTex.zy * 0.1;
                normal = normalize(normal);
            }            
        #endif

        vec3 f0 = vec3(0.04);
        float smoothness = 0.0;
        float reflAmount = 0.0;
        float occlusion = 1.0;
        
        vec3 r = reflect(rd, normal);
        
        // Color used when grayscale debug option is used:
        vec3 diffuseColor = vec3(0.5);
        
        if (material == M_GROUND) {
            normal = mapData.yzw;
            
            #ifndef GREYSCALE
                occlusion = clamp01(erosion + 0.5);

                // Cliffs / Dirt
                diffuseColor = CLIFF_COLOR * smoothstep(0.4, 0.52, pos.y);
                diffuseColor = mix(diffuseColor, DIRT_COLOR, smoothstep(0.6, 0.0, occlusion + breakup * 1.5));

                // Snow
                diffuseColor = mix(diffuseColor, vec3(1.0), smoothstep(0.53, 0.6, pos.y + breakup * 0.1));
                #ifdef WATER
                    // Sand (beach)
                    diffuseColor = mix(diffuseColor, SAND_COLOR, smoothstep(WATER_HEIGHT + 0.005, WATER_HEIGHT, pos.y + breakup * 0.01));
                #endif

                // Grass
                vec3 grassMix = mix(GRASS_COLOR1, GRASS_COLOR2, smoothstep(0.4, 0.6, pos.y - erosion * 0.05 + breakup * 0.3));
                diffuseColor = mix(diffuseColor, grassMix,
                    smoothstep(GRASS_HEIGHT + 0.05, GRASS_HEIGHT + 0.02, pos.y + 0.01 + (occlusion - 0.8) * 0.05 - breakup * 0.02)
                    * smoothstep(0.8, 1.0, 1.0 - (1.0 - normal.y) * (1.0 - trees) + breakup * 0.1));

                // Trees
                float tree = max(0.0, trees * 2.0 - 1.0);
                diffuseColor = mix(diffuseColor, TREE_COLOR * pow(trees, 8.0), clamp01(trees * 2.2 - 0.8) * 0.6);

                diffuseColor *= 1.0 + breakup * 0.5;
            #endif
            
            // Drainage (rivers, creeks, debris flow)
            #if defined(DRAINAGE)
                #if defined(GREYSCALE)
                    diffuseColor = mix(diffuseColor, vec3(0.0, 2.5, 2.5), drainage);
                #else
                    diffuseColor = mix(diffuseColor, vec3(1.0), drainage);
                #endif
            #endif
        }
        else if (material == M_STRATA) {
            #ifndef GREYSCALE
                vec3 strata = smoothstep(0.0, 1.0, cos(diff * vec3(130.0, 190.0, 250.0)));
                diffuseColor = vec3(0.3);
                diffuseColor = mix(diffuseColor, vec3(0.50), strata.x);
                diffuseColor = mix(diffuseColor, vec3(0.55), strata.y);
                diffuseColor = mix(diffuseColor, vec3(0.60), strata.z);

                diffuseColor *= exp(diff * 10.0) * vec3(1.0, 0.9, 0.7);
            #endif
        }
        else if (material == M_WATER) {
            float shore = normal.y > 1e-2 ? exp(-diff * 60.0) : 0.0;
            float foam = normal.y > 1e-2 ? smoothstep(0.005, 0.0, diff + breakup * 0.005) : 0.0;
        
            diffuseColor = mix(WATER_COLOR, WATER_SHORE_COLOR, shore);
            
            diffuseColor = mix(diffuseColor, vec3(1.0), foam);
            
            //f0 = vec3(0.2);
            smoothness = 0.95;
        }
        
        float shadow = 1.0;
        
        #ifdef SHADOWS
            if (material != M_STRATA) {
                // Shadow ray
                float s_t;
                int s_material;
                march(pos + vec3(0.0, 1.0, 0.0) * 1e-4, sun, foo.xyz, s_material, s_t);
                shadow = 1.0 - exp(-s_t * 20.0);
                
                // Debug shadows
                //fragColor = vec4(shadow, shadow, shadow, 1.0);
                //return;
            }
        #endif

        // Ambient
        color = diffuseColor * SkyColor(normal, sun) * Fd_Lambert();
        color *= occlusion;
        // Direct
        color += Shade(diffuseColor, f0, smoothness, normal, -rd, sun, SUN_COLOR * shadow);
        // Bounce
        color += diffuseColor * SUN_COLOR
            * (dot(normal, sun * vec3(1.0,-1.0, 1.0)) * 0.5 + 0.5)
            * Fd_Lambert() / PI;
        // Reflection
        color += GetReflection(pos, r, sun, smoothness)
            * F_Schlick(f0, dot(-rd, normal));

        #ifdef SHOW_DIFFUSE
            color = pow(diffuseColor, vec3(1.0 / 2.2));
        #elif defined(SHOW_NORMALS)
            color = normal.xzy * 0.5 + 0.5;
        #elif defined(SHOW_RIDGEMAP)
            if (material == M_GROUND) {
                color = vec3(ridgemap * ridgemap);
            }
        #endif
    }
    
    
    // ------------------------------------------------------------------------
    // Atmosphere rendering
    // ------------------------------------------------------------------------
    
    vec3 boxNormal;
    vec2 box = boxIntersection(ro, rd, boxSize, boxNormal);
    
    float costh = dot(rd, sun);
    float phaseR = PhaseRayleigh(costh);
    float phaseM = PhaseMie(costh, 0.6);
    
    vec2 od = vec2(0.0);
    vec3 tsm;
    vec3 sct = vec3(0.0);
    float rayLength = (t > 0.0 ? t : box.y) - box.x;
    float stepSize = rayLength / 16.0;
    for (float i = 0.0; i < 16.0; i++) {
        vec3 p = ro + rd * (box.x + (i + 0.5) * stepSize);
        
        float h = max(0.0, p.y - 0.35);
        float d = 1.0 - clamp01(h / 0.2);
        
        if (p.y < 0.35) {
            d = 0.0;
        }
        
        float densityR = d * 1e5;
        float densityM = d * 1e5;
        
        od += stepSize * vec2(densityR, densityM);
        
        tsm = exp(-(od.x * C_RAYLEIGH + od.y * C_MIE));
        
        sct += tsm * C_RAYLEIGH * phaseR * densityR * stepSize;
        sct += tsm * C_MIE * phaseM * densityM * stepSize;
    }
    
    color = color * tsm + sct * 10.0;
    
    
    // ------------------------------------------------------------------------
    // Tone mapping and dithering
    // ------------------------------------------------------------------------

    #if !defined(SHOW_NORMALS) && !defined(SHOW_DIFFUSE)
        color = Tonemap_ACES(color);
        color = pow(color, vec3(1.0 / 2.2));
    #endif

    // Dither
    vec2 channel2Res = iChannelResolution[2].xy;
    vec2 channel2Tiled = mod(fragCoord.xy, channel2Res) / channel2Res;
    color += texture(iChannel2, channel2Tiled).xxx / 255.0;

    
    // ------------------------------------------------------------------------
    // Debug buffer display
    // ------------------------------------------------------------------------
    
    #ifdef SHOW_BUFFER
        vec3 annotationColor = vec3(0.2,0.8,0.2);
        vec2 uv = fragCoord / debugWidth;
        uv.x = 1.0 - uv.x;
        uv.y += max(0.0, float(SHOW_BUFFER_NR - 1));
        int view = int(uv.y);
        uv.y = fract(uv.y);
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
            uv += TIME_SCROLL_OFFSET_FRAC;
            float erosion;
            float ridgemap;
            float trees;
            float debug;
            vec4 heightAndNormal = map(uv, erosion, ridgemap, trees, debug);
            
            vec4 data;
            vec3 tex = GetChannel0Data(uv, data);

            // Normal.
            if (view == 3)
                color = heightAndNormal.ywz * 0.5 + 0.5;
            // Terrain height.
            else if (view == 2)
                color = mix(heightAndNormal.xxx, vec3(0.45), 1.0 - 4.0);
            // Erosion height offset.
            else if (view == 1)
                color = vec3(erosion * 0.5 + 0.5);
            // Ridge map.
            else if (view == 0)
                color = vec3(ridgemap);
            // Debug.
            else
                color = vec3(debug);

            // Separate debug maps by thin lines.
            if (debugCount > 1.0) {
                float diff = max(abs(uv.y - 0.5), abs(uv.x - 0.5))
                    - (0.5 - debugCount / iResolution.y);
                color *= mix(0.6, 1.0, clamp01(- diff * iResolution.y * 0.5));
            }
        }
    #endif
    
    fragColor = vec4(color, 1.0);
}

# Common Shader

/*
====================================================================================

See Buffer A for information on the erosion technique.

====================================================================================
*/

// Comment out to see terrain without animated parameters.
#define ANIMATE_PARAMETERS

#define PI 3.14159265358979

void AnimateTo(inout float current, float target, float time) {
    #if defined(ANIMATE_PARAMETERS)
        current = mix(current, target, smoothstep(0.0, 1.0, time));
    #endif
}

void AnimateWaveTo(inout float current, float target, float time) {
    #if defined(ANIMATE_PARAMETERS)
        time = clamp(time, 0.0, 1.0);
        current = mix(current, target, 0.5 - 0.5 * cos(3.0 * time * PI));
    #endif
}

void AnimateLoHi(inout float current, float lo, float hi, float time) {
    #if defined(ANIMATE_PARAMETERS)
        float og = current;
        current = mix(current, lo, smoothstep(0.0, 1.0, time));
        current = mix(current, hi, smoothstep(0.0, 1.0, time - 2.0));
        current = mix(current, og, smoothstep(0.0, 1.0, time - 4.0));
    #endif
}


// -----------------------------------------------------------------------------
// Debug options
// -----------------------------------------------------------------------------

//#define GREYSCALE
//#define SHOW_RIDGEMAP // Try me
//#define SHOW_DIFFUSE
//#define SHOW_NORMALS
//#define COMPARISON_SLIDER
//#define SHOW_BUFFER

// Requires SHOW_BUFFER to be defined.
// 0: All buffers (1-4)
// 1: Ridgemap
// 2: Erosion offset
// 3: Heightmap
// 4: Normals
// 5: Debug
#define SHOW_BUFFER_NR 0


// -----------------------------------------------------------------------------
// Renderer settings
// -----------------------------------------------------------------------------

#define SHADOWS
#define FIXED_SUN
#define WATER
#define WATER_HEIGHT (0.36 + 0.1 * (smoothstep(54.0, 60.0, mod(iTime, 120.0)) - smoothstep(114.0, 120.0, mod(iTime, 120.0))))
#define FOG_HEIGHT 0.465
#define GRASS_HEIGHT 0.465
#define DRAINAGE
#define DRAINAGE_WIDTH 0.3
#define TREES
#define DETAIL_TEXTURE
#define RAYMARCH_QUALITY 2.0


// -----------------------------------------------------------------------------
// Camera settings
// -----------------------------------------------------------------------------

#define CAMERA_MOUSE_CONTROL
#define LOW_ANGLE mod(iTime, 240.0) >= 120.0
#define CAMERA_DIST (LOW_ANGLE ? 1.5 : 3.25)
#define CAMERA_FOV (LOW_ANGLE ? 20.0 : 11.0)
#define CAMERA_ANGLE (LOW_ANGLE ? 0.25 : -0.45)
#define CAMERA_ELEVATION (LOW_ANGLE ? -0.35 : -0.43)
#define CAMERA_LOOKAT vec3(0.0, 0.40, 0.0)
#define TIME_SCROLL_OFFSET vec2(cos(iTime / 60.0 * 2.0 * PI) * 2.0, -sin(iTime / 60.0 * 2.0 * PI) * 0.1)
#define TIME_CAM_SPIN (LOW_ANGLE ? 0.0 : 1.0 / 60.0)
#define TIME_CAM_WOBBLE 0.0 // (1.0 / 60.0)


// -----------------------------------------------------------------------------
// Material and color values
// -----------------------------------------------------------------------------

// Materials

#define M_GROUND 0
#define M_STRATA 1
#define M_WATER  2

// Colors

#define CLIFF_COLOR    vec3(0.22, 0.2, 0.2)
#define DIRT_COLOR     vec3(0.6, 0.5, 0.4)
#define TREE_COLOR     vec3(0.12, 0.26, 0.1)
#define GRASS_COLOR1   vec3(0.15, 0.3, 0.1)
#define GRASS_COLOR2   vec3(0.4, 0.5, 0.2)
#define SAND_COLOR     vec3(0.8, 0.7, 0.6)

#define WATER_COLOR vec3(0.0, 0.05, 0.1)
#define WATER_SHORE_COLOR vec3(0.0, 0.25, 0.25)

#define SUN_COLOR (vec3(1.0, 0.98, 0.95) * 2.0)
#define AMBIENT_COLOR (vec3(0.3, 0.5, 0.7) * 0.1)


// -----------------------------------------------------------------------------
// Buffer size functionality
// -----------------------------------------------------------------------------

// Limit the work area of Buffer A/B to speed things up
#define BUFFER_SIZE vec2(min(min(iResolution.x, iResolution.y), 1080.0))
//#define BUFFER_SIZE vec2(min(min(iResolution.x, iResolution.y), 768.0))
#define DISCARD_MAP (fragCoord.x >= BUFFER_SIZE.x || fragCoord.y >= BUFFER_SIZE.y)
#define TIME_SCROLL_OFFSET_INT (round(TIME_SCROLL_OFFSET * BUFFER_SIZE) / BUFFER_SIZE)
#define TIME_SCROLL_OFFSET_FRAC (TIME_SCROLL_OFFSET - TIME_SCROLL_OFFSET_INT)


// -----------------------------------------------------------------------------
// Misc utility functions
// -----------------------------------------------------------------------------

#define DEG_TO_RAD (PI / 180.0)
#define clamp01(x) clamp(x, 0.0, 1.0)
#define sq(x) (x*x)

vec2 hash(in vec2 x) {
    const vec2 k = vec2(0.3183099, 0.3678794);
    x = x * k + k.yx;
    return -1.0 + 2.0 * fract(16.0 * k * fract(x.x * x.y * (x.x + x.y)));
}


// Returns gradient noise (in x) and its derivatives (in yz).
// From https://www.shadertoy.com/view/XdXBRH
vec3 noised(in vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);

    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0); 
    
    vec2 ga = hash(i + vec2(0.0, 0.0));
    vec2 gb = hash(i + vec2(1.0, 0.0));
    vec2 gc = hash(i + vec2(0.0, 1.0));
    vec2 gd = hash(i + vec2(1.0, 1.0));
    
    float va = dot(ga, f - vec2(0.0, 0.0));
    float vb = dot(gb, f - vec2(1.0, 0.0));
    float vc = dot(gc, f - vec2(0.0, 1.0));
    float vd = dot(gd, f - vec2(1.0, 1.0));

    return vec3(va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * (va - vb - vc + vd),
        ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd) +
        du * (u.yx * (va - vb - vc + vd) + vec2(vb, vc) - va));
}

// From https://iquilezles.org/articles/intersectors
vec2 boxIntersection(in vec3 ro, in vec3 rd, vec3 boxSize, out vec3 outNormal) {
    vec3 m = 1.0 / rd; // can precompute if traversing a set of aligned boxes
    vec3 n = m * ro;   // can precompute if traversing a set of aligned boxes
    vec3 k = abs(m) * boxSize;
    vec3 t1 = -n - k;
    vec3 t2 = -n + k;
    float tN = max(max(t1.x, t1.y), t1.z);
    float tF = min(min(t2.x, t2.y), t2.z);
    if (tN > tF || tF < 0.0)
        return vec2(-1.0); // no intersection
    outNormal = -sign(rd) * step(t1.yzx, t1.xyz) * step(t1.zxy, t1.xyz);
    return vec2(tN, tF);
}

// ==========================================================================================
// Set up camera
// ==========================================================================================

// From https://www.shadertoy.com/view/XsB3Rm
vec3 CameraRay(float fov, vec2 size, vec2 pos) {
    vec2 xy = pos - size * 0.5;
    float cot_half_fov = tan((90.0 - fov * 0.5) * DEG_TO_RAD);    
    float z = size.y * 0.5 * cot_half_fov;
    return normalize(vec3(xy, -z));
}
mat3 CameraRotation(vec2 angle) {
    vec2 c = cos(angle);
    vec2 s = sin(angle);
    return mat3(
        c.y      ,  0.0, -s.y,
        s.y * s.x,  c.x,  c.y * s.x,
        s.y * c.x, -s.x,  c.y * c.x
    );
}

void GetRay(out vec3 ro, out vec3 rd, float iTime, vec4 iMouse, vec3 iResolution, vec2 fragCoord, float debugWidth) {
    float iRevolution = iTime * 2.0 * PI;
    vec2 cameraAngle = vec2(
        iRevolution * TIME_CAM_SPIN
            + sin(iRevolution / 6.0) * TIME_CAM_WOBBLE * 6.0
            + CAMERA_ANGLE * 2.0 * PI,
        CAMERA_ELEVATION);
    float cameraDistance = CAMERA_DIST;
    
    // Intro animation
    //cameraAngle.x -= exp(-iTime * 5.0) * 4.0;
    //cameraDistance += exp(-iTime * 5.0) * 5.0;
    
    #if defined(CAMERA_MOUSE_CONTROL)
        // Control camera orbit position with mouse when held down.
        if (iMouse.z > 0.5) {
            vec2 mouse = iMouse.xy / iResolution.xy;
            mouse.y = clamp01(mix(mouse.y, 0.5, -1.0));
            cameraAngle = (mouse - vec2(0.5, 1.0)) * vec2(-PI * 2.0, PI * 0.5);
        }
    #endif

    mat3 rot = CameraRotation(cameraAngle.yx);
    
    // Ray direction.
    rd = CameraRay(CAMERA_FOV, iResolution.xy, fragCoord.xy - vec2(debugWidth / 2.0, 0.0));
    rd = rot * rd;
    
    // Ray origin.
    ro = CAMERA_LOOKAT + rot * vec3(0, 0, cameraDistance);
}

vec3 GetMouseWorldPos(vec3 ro, vec3 rd, float planeHeight) {
    return ro + rd * (planeHeight - ro.y) / rd.y;
}

vec3 SkyColor(vec3 rd, vec3 sun) {
    float costh = dot(rd, sun);
    return AMBIENT_COLOR * PI * (1.0 - abs(costh) * 0.8);
}

vec3 Tonemap_ACES(vec3 x) {
    // Narkowicz 2015, "ACES Filmic Tone Mapping Curve"
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return (x * (a * x + b)) / (x * (c * x + d) + e);
}


// -----------------------------------------------------------------------------
// BRDF functionality from https://www.shadertoy.com/view/XlKSDR
// -----------------------------------------------------------------------------

float pow5(float x) {
    float x2 = x * x;
    return x2 * x2 * x;
}

float D_GGX(float linearRoughness, float NoH, const vec3 h) {
    // Walter et al. 2007, "Microfacet Models for Refraction through Rough Surfaces"
    float oneMinusNoHSquared = 1.0 - NoH * NoH;
    float a = NoH * linearRoughness;
    float k = linearRoughness / (oneMinusNoHSquared + a * a);
    float d = k * k * (1.0 / PI);
    return d;
}

float V_SmithGGXCorrelated(float linearRoughness, float NoV, float NoL) {
    // Heitz 2014, "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs"
    float a2 = linearRoughness * linearRoughness;
    float GGXV = NoL * sqrt((NoV - a2 * NoV) * NoV + a2);
    float GGXL = NoV * sqrt((NoL - a2 * NoL) * NoL + a2);
    return 0.5 / (GGXV + GGXL);
}

vec3 F_Schlick(const vec3 f0, float VoH) {
    // Schlick 1994, "An Inexpensive BRDF Model for Physically-Based Rendering"
    return f0 + (vec3(1.0) - f0) * pow5(1.0 - VoH);
}

float F_Schlick(float f0, float f90, float VoH) {
    return f0 + (f90 - f0) * pow5(1.0 - VoH);
}

float Fd_Burley(float linearRoughness, float NoV, float NoL, float LoH) {
    // Burley 2012, "Physically-Based Shading at Disney"
    float f90 = 0.5 + 2.0 * linearRoughness * LoH * LoH;
    float lightScatter = F_Schlick(1.0, f90, NoL);
    float viewScatter  = F_Schlick(1.0, f90, NoV);
    return lightScatter * viewScatter * (1.0 / PI);
}

float Fd_Lambert() {
    return 1.0 / PI;
}

vec3 Shade(vec3 diffuse, vec3 f0, float smoothness, vec3 n, vec3 v, vec3 l, vec3 lc) {
    vec3 h = normalize(v + l);

    float NoV = abs(dot(n, v)) + 1e-5;
    float NoL = clamp01(dot(n, l));
    float NoH = clamp01(dot(n, h));
    float LoH = clamp01(dot(l, h));

    float roughness = 1.0 - smoothness;
    float linearRoughness = roughness * roughness;
    float D = D_GGX(linearRoughness, NoH, h);
    float V = V_SmithGGXCorrelated(linearRoughness, NoV, NoL);
    vec3 F = F_Schlick(f0, LoH);
    vec3 Fr = (D * V) * F;

    vec3 Fd = diffuse * Fd_Burley(linearRoughness, NoV, NoL, LoH);

    return (Fd + Fr) * lc * NoL;
}

// ------------------------------------------------------------------------------
// Atmosphere
// ------------------------------------------------------------------------------

#define C_RAYLEIGH (vec3(5.802, 13.558, 33.100) * 1e-6)
#define C_MIE (vec3(3.996,  3.996,  3.996) * 1e-6)

float PhaseRayleigh(float costh) {
	return 3.0 * (1.0 + costh * costh) / (16.0 * PI);
}

float PhaseMie(float costh, float g) {
	g = min(g, 0.9381);
	float k = 1.55 * g - 0.55 * g * g * g;
	float kcosth = k * costh;
	return (1.0 - k * k) / ((4.0 * PI) * (1.0 - kcosth) * (1.0 - kcosth));
}


// ------------------------------------------------------------------------------
// Packing
// ------------------------------------------------------------------------------

// Some methods to package colours from a vec4 (0-1) into a single 32-bit float.
float pack4(in vec4 rgba) {
    lowp int red = clamp(int(rgba.r * 255.0), 0, 255);
    lowp int green = clamp(int(rgba.g * 255.0), 0, 255);
    lowp int blue = clamp(int(rgba.b * 255.0), 0, 255);
    lowp int alpha = clamp(int(rgba.a * 255.0), 0, 255);

    return intBitsToFloat((red << 24) | (green << 16) | (blue << 8) | alpha);
}

vec4 unpack4(in float col) {
    highp int val = floatBitsToInt(col);

    return vec4(
        float((val >> 24) & 255) / 255.0,
        float((val >> 16) & 255) / 255.0,
        float((val >> 8) & 255) / 255.0,
        float(val & 255) / 255.0
    );
}


# Buffer A
/*
=====================================================================================

Advanced terrain erosion filter based on stacked faded gullies,
with controls for erosion strength, detail, ridge and crease rounding,
and producing a ridge map output useful for e.g. water drainage.

For more on the technique, see:
https://www.youtube.com/watch?v=gsJHzBTPG0Y
https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html

This buffer has three parts:

 - Phacelle Nose function (used by the erosion function)
 - Erosion function
 - Demonstration

For explanations of the erosion parameters, see the demonstration section.

This erosion technique was originally derived from versions by
Clay John (https://www.shadertoy.com/view/MtGcWh)
and Fewes (https://www.shadertoy.com/view/7ljcRW)
and my own cleaned up version (https://www.shadertoy.com/view/33cXW8),
but at this point has little in common with them, apart from the high level concept.

Also see "Mouse-Paint Eroded Mountains" variation with interactive heightmap.
https://www.shadertoy.com/view/sf23W1

The raymarched terrain rendering is largely based on Fewes' Shadertoy;
see the Image buffer for more info on that.

=====================================================================================
*/


// -----------------------------------------------------------------------------
// PHACELLE NOISE FUNCTION
// -----------------------------------------------------------------------------

// NOTE: Phacelle Noise depends on the 'hash' function defined in the Common tab.

#define TAU 6.28318530717959

// The Simple Phacelle Noise function produces a stripe pattern aligned with the input vector.
// The name Phacelle is a portmanteau of phase and cell, since the function produces a phase by
// interpolating cosine and sine waves from multiple cells.
//  - p is the input point being evaluated.
//  - normDir is the direction of the stripes at this point. It must be a normalized vector.
//  - freq is the freqency of the stripes within each cell. It's best to keep it close to 1.0, as
//    high values will produce distortions and other artifacts.
//  - offset is the phase offset of the stripes, where 1.0 is a full cycle.
//  - normalization is the degree of normalization applied, between 0 and 1. With e.g. a value of
//    0.4, raw output with a magnitude below 0.6 won't get fully normalized to a magnitude of 1.0.
// Phacelle Noise function copyright (c) 2025 Rune Skovbo Johansen
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
vec4 PhacelleNoise(in vec2 p, vec2 normDir, float freq, float offset, float normalization) {
    // Get a vector orthogonal to the input direction, with a
    // magnitude proportional to the frequency of the stripes.
    vec2 sideDir = normDir.yx * vec2(-1.0, 1.0) * freq * TAU;
    offset *= TAU;

    // Iterate over 4x4 cells, calculating a stripe pattern for each and blending between them.
    // pInt is the integer part of the current coordinate p, pFrac is the remainder.
    //
    // o   o   o   o
    //
    // o   o   o   o
    //       p
    // o   i   o   o
    //
    // o   o   o   o
    //
    // p: current coordinate    i: integer part of p    o: grid points for 4x4 cells
    //
    vec2 pInt = floor(p);
    vec2 pFrac = fract(p);
    vec2 phaseDir = vec2(0.0);
    float weightSum = 0.0;
    for (int i = -1; i <= 2; i++) {
        for (int j = -1; j <= 2; j++) {
            vec2 gridOffset = vec2(i, j);

            // Calculate a cell point by starting off with a point in the integer grid.
            vec2 gridPoint = pInt + gridOffset;

            // Calculate a random offset for the cell point between -0.5 and 0.5 on each axis.
            vec2 randomOffset = hash(gridPoint) * 0.5;

            // The final cell point (we don't store it) is the gridPoint plus the randomOffset.
            // Calculate a vector representing the input point relative to this cell point:
            // p - (gridPoint + randomOffset)
            // = (pFrac + pInt) - ((pInt + gridOffset) + randomOffset)
            // = pFrac + pInt - pInt - gridOffset - randomOffset
            // = pFrac - gridOffset - randomOffset
            vec2 vectorFromCellPoint = pFrac - gridOffset - randomOffset;

            // Bell-shaped weight function which is 1 at dist 0 and nearly 0 at dist 1.5.
            // Due to the random offsets of up to 0.5, the closest a cell point not in the 4x4
            // grid can be to the current point p is 1.5 units away.
            float sqrDist = dot(vectorFromCellPoint, vectorFromCellPoint);
            float weight = exp(-sqrDist * 2.0);
            // Subtract 0.01111 to make the function actually 0 at distance 1.5, which avoids
            // some (very subtle) grid line artefacts.
            weight = max(0.0, weight - 0.01111);

            // Keep track of the total sum of weights.
            weightSum += weight;

            // The waveInput is a gradient which increases in value along sideDir. Its rate of
            // change is the freq times tau, due to the multiplier pre-applied to sideDir.
            float waveInput = dot(vectorFromCellPoint, sideDir) + offset;

            // Add this cell's cosine and sine wave contributions to the interpolated value.
            phaseDir += vec2(cos(waveInput), sin(waveInput)) * weight;
        }
    }

    // Get the raw interpolated value.
    vec2 interpolated = phaseDir / weightSum;
    // Interpret the value as a vector whose length represents the magnitude of both waves.
    float magnitude = sqrt(dot(interpolated, interpolated));
    // Apply a lower threshold to show small magnitudes we're going to fully normalize.
    magnitude = max(1.0 - normalization, magnitude);
    // Return a vector containing the normalized cosine and sine waves, as well as the direction
    // vector, which can be multiplied onto the sine to get the derivatives of the cosine.
    return vec4(interpolated / magnitude, sideDir);
}


// -----------------------------------------------------------------------------
// EROSION FUNCTION
// -----------------------------------------------------------------------------

// First a few utility functions.

float pow_inv(float t, float power) {
    // Flip, raise to the specified power, and flip back.
    return 1.0 - pow(1.0 - clamp01(t), power);
}

float ease_out(float t) {
    // Flip by subtracting from one.
    float v = 1.0 - clamp01(t);
    // Raise to a power of two and flip back.
    return 1.0 - v * v;
}

float smooth_start(float t, float smoothing) {
    if (t >= smoothing)
        return t - 0.5 * smoothing;
    return 0.5 * t * t / smoothing;
}

vec2 safe_normalize(vec2 n) {
 	// A div-by-zero-safe replacement for normalize.
    float l = length(n);
	return (abs(l) > 1e-10) ? (n / l) : n;	
}

// Advanced Terrain Erosion Filter copyright (c) 2025 Rune Skovbo Johansen
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
vec4 ErosionFilter(
    // Input parameters that vary per pixel.
    in vec2 p, vec3 heightAndSlope, float fadeTarget,
    // Stylistic parameters that may vary per pixel.
    float strength, float gullyWeight, float detail, vec4 rounding, vec4 onset, vec2 assumedSlope,
    // Scale related parameters that do not support variation per pixel.
    float scale, int octaves, float lacunarity,
    // Other parameters.
    float gain, float cellScale, float normalization,
    // Output parameters.
    out float ridgeMap, out float debug
) {
    strength *= scale;
    fadeTarget = clamp(fadeTarget, -1.0, 1.0);
    
    vec3 inputHeightAndSlope = heightAndSlope;
    float freq = 1.0 / (scale * cellScale);
    float slopeLength = max(length(heightAndSlope.yz), 1e-10);
    float magnitude = 0.0;
    float roundingMult = 1.0;
    
    float roundingForInput = mix(rounding.y, rounding.x, clamp01(fadeTarget + 0.5)) * rounding.z;
    // The combined accumulating mask, based first on initial slope, and later on slope of each octave too.
    float combiMask = ease_out(smooth_start(slopeLength * onset.x, roundingForInput * onset.x));

    // Initialize the ridgeMap fadeTarget and mask.
    float ridgeMapCombiMask = ease_out(slopeLength * onset.z);
    float ridgeMapFadeTarget = fadeTarget;
    
    // Deteriming the strength of the initial slope used for gully directions
    // based on the specified mix of the actual slope and an assumed slope.
    vec2 gullySlope = mix(heightAndSlope.yz, heightAndSlope.yz / slopeLength * assumedSlope.x, assumedSlope.y);
    
    for (int i = 0; i < octaves; i++) {
        // Calculate and add gullies to the height and slope.
        vec4 phacelle = PhacelleNoise(p * freq, safe_normalize(gullySlope), cellScale, 0.25, normalization);
        // Multiply with freq since p was multiplied with freq.
        // Negate since we use slope directions that point down.
        phacelle.zw *= -freq;
        // Amount of slope as value from 0 to 1.
        float sloping = abs(phacelle.y);
        
        // Add non-masked, normalized slope to gullySlope, for use by subsequent octaves.
        // It's normalized to use the steepest part of the sine wave everywhere.
        gullySlope += sign(phacelle.y) * phacelle.zw * strength * gullyWeight;
        
        // Handle height offset and approximate output slope.
        
        // Gullies has height offset (from -1 to 1) in x and derivative in yz.
        vec3 gullies = vec3(phacelle.x, phacelle.y * phacelle.zw);
        // Fade gullies towards fadeTarget based on combiMask.
        vec3 fadedGullies = mix(vec3(fadeTarget, 0.0, 0.0), gullies * gullyWeight, combiMask);
        // Apply height offset and derivative (slope) according to strength of current octave.
        heightAndSlope += fadedGullies * strength;
        magnitude += strength;
        
        // Update fadeTarget to include the new octave.
        fadeTarget = fadedGullies.x;
        
        // Update the mask to include the new octave.
        float roundingForOctave = mix(rounding.y, rounding.x, clamp01(phacelle.x + 0.5)) * roundingMult;
        float newMask = ease_out(smooth_start(sloping * onset.y, roundingForOctave * onset.y));
        combiMask = pow_inv(combiMask, detail) * newMask;
        
        // Update the ridgeMap fadeTarget and mask.
        ridgeMapFadeTarget = mix(ridgeMapFadeTarget, gullies.x, ridgeMapCombiMask);
        float newRidgeMapMask = ease_out(sloping * onset.w);
        ridgeMapCombiMask = ridgeMapCombiMask * newRidgeMapMask;

        // Prepare the next octave.
        strength *= gain;
        freq *= lacunarity;
        roundingMult *= rounding.w;
    }
    
    ridgeMap = ridgeMapFadeTarget * (1.0 - ridgeMapCombiMask);
    debug = fadeTarget;
    
    vec3 heightAndSlopeDelta = heightAndSlope - inputHeightAndSlope;
    return vec4(heightAndSlopeDelta, magnitude);
}


// -----------------------------------------------------------------------------
// DEMONSTRATION
// -----------------------------------------------------------------------------

// Used for the height map.
vec3 FractalNoise(vec2 p, float freq, int octaves, float lacunarity, float gain) {
    vec3 n = vec3(0.0);
    float nf = freq;
    float na = 1.0;
    for (int i = 0; i < octaves; i++) {
        n += noised(p * nf) * na * vec3(1.0, nf, nf);
        na *= gain;
        nf *= lacunarity;
    }
    return n;
}

// Used for tree coverage on the height map.
float GetTreesAmount(float height, float normalY, float occlusion, float ridgeMap) {
    return ((
        smoothstep(
            GRASS_HEIGHT + 0.05,
            GRASS_HEIGHT + 0.01,
            height + 0.01 + (occlusion - 0.8) * 0.05
        )
        * smoothstep(
            0.0,
            0.4,
            occlusion
        )
        * smoothstep(0.95, 1.0, normalY)
        * smoothstep(-1.4, 0.0, ridgeMap)
        #if defined(WATER)
            * smoothstep(
                WATER_HEIGHT + 0.000,
                WATER_HEIGHT + 0.007,
                height
            )
        #endif
    ) - 0.5) / 0.6;
}

vec4 Heightmap(vec2 p) {

    float time = mod(iTime * 0.5, 30.0);
    
    
    // ------------------------------------------------------------------------
    // Erosion parameters.
    // ------------------------------------------------------------------------
    
    // The scale of the erosion effect, affecting it both horizontally and vertically.
    float EROSION_SCALE = 0.15;
    AnimateLoHi(EROSION_SCALE, 0.08, 0.25, time - 7.0);
    
    // The strength of the erosion effect, affecting the magnitude of all octaves,
    // and indirectly affecting the directions of the gullies as a result.
    float EROSION_STRENGTH = 0.22;
    AnimateLoHi(EROSION_STRENGTH, 0.01, 0.10, time - 1.0);
    
    // The magnitude of the gullies as a weight value from 0 to 1.
    // A value of 0 can sharpen peaks and valleys but feature virtually no gullies.
    // A value of 1 produces full gullies but may leave peaks and valleys rounded.
    // Adjusting erosion gully weight while inversely adjusting erosion scale can be
    // used to control the sharpness of peaks and valleys while leaving gully
    // magnitudes largely untocuhed.
    float EROSION_GULLY_WEIGHT = 0.5;
    
    // The overall detail of the erosion. Lower values restrict the effect of higher
    // frequency gullies to steeper slopes.
    float EROSION_DETAIL = 1.5;
    AnimateLoHi(EROSION_DETAIL, 3.0, 0.7, time - 13.0);
    
    float ridgeRounding = 0.1;
    float creaseRounding = 0.0;
    AnimateWaveTo(creaseRounding, 1.0, time - 19.0);
    AnimateWaveTo(ridgeRounding, 1.0, time - 21.0);
    AnimateWaveTo(creaseRounding, 0.0, time - 23.0);
    AnimateWaveTo(ridgeRounding, 0.1, time - 25.0);
    // Separate rounding control of ridges and creases.
    //  x: Rounding of ridges.
    //  y: Rounding of creases.
    //  z: Multiplier applied to the initial height function.
    //     E.g. if the height function has noise of 5 times lower frequency
    //     than the largest gullies, a value of 0.2 can compensate for that.
    //  w: Multiplier applied to each subsequent gully octave after the first.
    //     Setting it to the same value as the erosion lacunarity will produce
    //     consistent rounding of all octaves.
    vec4 EROSION_ROUNDING = vec4(ridgeRounding, creaseRounding, 0.1, 2.0);
    
    // Control over how far away from ridges/creases the erosion takes effect.
    //  x: Onset used on the initial height function.
    //  y: Onset used on each gully octave.
    //  z: RidgeMap-specific onset used on the initial height function.
    //  w: RidgeMap-specific onset used on each gully octave.
    vec4 EROSION_ONSET = vec4(1.25, 1.25, 2.8, 1.5);
    
    // Control over the assumed slope of the initial height function.
    // In practise, assuming a slope can work better than using the input slope,
    // since the final terrain can be shaped quite differently than the input.
    //  x: An assumed slope value to override the actual slope.
    //  y: The amount (from 0 to 1) to override the actual slope.
    vec2 EROSION_ASSUMED_SLOPE = vec2(0.7, 1.0);
    
    // Gullies are based on stripes within Voronoi-like cells in the Phacelle noise
    // function. The cell scale parameter controls the sizes of the cells relative
    // to the overall erosion scale, while keeping the stripe widths unaffected.
    // Values close to 1 usually produce good results. Smaller values produce more
    // grainy gullies while larger values produce longer unbroken gullies, but too
    // large values produce chaotic curved gullies that are not aligned with the
    // slopes. Value changes can cause abrupt changes in output, especially far away
    // from the origin, so this parameter is not well suited for animation or for
    // modulation by other functions.
    float EROSION_CELL_SCALE = 0.7;
    // The degree of normalization applied in the Phacelle noise, between 0 and 1.
    // The erosion filter depends on a certain consistency in magnitude of the
    // Phacelle output. However, high values can create loopy results where ridges
    // and creases meet up at a point, which produces unnatural looking results.
    float EROSION_NORMALIZATION = 0.5;
    
    // Control over the erosion octaves, with each successive octave layering
    // smaller gullies onto the terrain.
    int EROSION_OCTAVES = 5;
    // The lacunarity controls the frequency (the inverse
    // horizontal scale) of each octave relative to the last.
    float EROSION_LACUNARITY = 2.0;
    // The gain controls the magnitude (the vertical
    // scale) of each octave relative to the last.
    float EROSION_GAIN = 0.5;
    
    
    // ------------------------------------------------------------------------
    // Terrain parameters not used in the erosion function itself.
    // ------------------------------------------------------------------------
    
    // Control over whether the erosion effect raises or lowers the terrain.
    //  x: An offset value between -1 and 1, where a value of -1 only lowers, while
    //     1 only raises. The offset is proportional to the erosion strength
    //     parameter, so if that parameter is the same for the entire terrain, the
    //     effect of the height offset will move the entire terrain surface up or
    //     down by the same emount.
    //  y: A value between 0 and 1 which is the degree to which the offset value is
    //     replaced by the negated erosion fade target value. This has the effect
    //     of only raising at valleys and only lowering at peaks, which, due to how
    //     the erosion filter works, has the effect of largely preserving the minima
    //     and maxima of the terrain.
    vec2 TERRAIN_HEIGHT_OFFSET = vec2(-0.65, 0.0);

    // Base height noise parameters.
    
    // The inverse horizontal scale of the terrain noise function.
    float HEIGHT_FREQUENCY = 3.0;
    // The vertical scale (amplitude) of the terrain noise function.
    float HEIGHT_AMP = 0.125;
    // Control over the noise function octaves, with each successive
    // octave layering smaller bumps onto the terrain.
    int HEIGHT_OCTAVES = 3;
    // The lacunarity controls the frequency (the inverse
    // horizontal scale) of each octave relative to the last.
    float HEIGHT_LACUNARITY = 2.0;
    // The gain controls the magnitude (the vertical scale)
    // of each octave relative to the last.
    float HEIGHT_GAIN = 0.1;
    
    
    // ------------------------------------------------------------------------
    // Logic for whether erosion is enabled or not.
    // ------------------------------------------------------------------------
    
    bool erosion = true;
    
    // Toggle erosion with Enter key toggle.
    if (texelFetch(iChannel1, ivec2(13, 2), 0).x > 0.0)
        erosion = false;
    
    #ifdef COMPARISON_SLIDER
        // Animated slider that displays terrain with/without erosion.
        if (1.0 - p.y > 0.5 - cos(iTime))
            erosion = false;
    #endif
    
    
    // ------------------------------------------------------------------------
    // Heightmap implementation.
    // ------------------------------------------------------------------------

    float heightFunctionScale = 1.0;
    vec2 pHeight = p / heightFunctionScale;
    
    // Calculate the FBM terrain height and derivatives and store them in n.
    // The heights are in the [-1, 1] range.
    vec3 n = FractalNoise(pHeight, HEIGHT_FREQUENCY, HEIGHT_OCTAVES, HEIGHT_LACUNARITY, HEIGHT_GAIN)
		* HEIGHT_AMP * vec3(heightFunctionScale, 1.0, 1.0);

    // Define the erosion fade target based on the altitude of the pre-eroded terrain.
    // The fade target should strive to be -1 at valleys and 1 at peaks, but overshooting is ok.
    float fadeTarget = clamp(n.x / (HEIGHT_AMP * 0.6), -1.0, 1.0);
    
    // Change terrain heights from [-1, 1] range to [0, 1] range.
    n = n * 0.5 + vec3(0.5, 0, 0);

    // Store erosion in h (x : height delta, yz : slope delta, w : magnitude).
    // The output ridge map is -1 on creases and 1 on ridges.
    // The output debug value can be set to various values inside the erosion function.
    float ridgeMap, debug;
    vec4 h = ErosionFilter(
        p, n, fadeTarget,
        EROSION_STRENGTH, EROSION_GULLY_WEIGHT, EROSION_DETAIL,
        EROSION_ROUNDING, EROSION_ONSET, EROSION_ASSUMED_SLOPE,
        EROSION_SCALE, EROSION_OCTAVES, EROSION_LACUNARITY,
        EROSION_GAIN, EROSION_CELL_SCALE, EROSION_NORMALIZATION,
        ridgeMap, debug);
    
    if (!erosion) {
        h = vec4(0.0);
        ridgeMap = 1.0;
    }
    
    // Offset according to the height offset parameter by multiplying it with the magnitude.
    float offset = mix(TERRAIN_HEIGHT_OFFSET.x, -fadeTarget, TERRAIN_HEIGHT_OFFSET.y) * h.w;
    float eroded = n.x + h.x + offset;
    
    // Add trees to terrain.
    float trees = -1.0;
    #if defined(TREES)
        vec2 deriv = n.yz + h.yz;
        float normalY = 1.0 / sqrt(1.0 + dot(deriv, deriv));
        float treesAmount = GetTreesAmount(eroded, normalY, h.x / h.w + 0.5, ridgeMap);
        trees = (1.0 - pow(noised((p + 0.5) * 200.0).x * 0.5 + 0.5, 2.0) - 1.0 + 1.0 * treesAmount) * 1.5;
        if (trees > 0.0) {
            eroded += trees / 300.0;
        }
    #endif

    // Pack four floats into a single channel to be able to get more data out of this buffer.
    float packed = pack4(vec4(
        clamp01(h.x / h.w * 0.5 + 0.5), // Erosion delta as [0, 1] value.
        clamp01(ridgeMap * 0.5 + 0.5),  // Ridge map as [0, 1] value.
        clamp01(trees * 0.5 + 0.5),     // Tree value as [0, 1] value.
        clamp01(debug * 0.5 + 0.5)      // Debug value.
    ));
    
    return vec4(eroded, 0.0, 0.0, packed);
    
}

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    if (DISCARD_MAP) {
        return;
    }
    vec2 uv = fragCoord / BUFFER_SIZE;    
    uv += TIME_SCROLL_OFFSET_INT;

    // Get the map data.
    fragColor = Heightmap(uv);
}

# Buffer B
/*
====================================================================================

Supplemental noise texture used to add diffuse/normal detail.

Derived from https://www.shadertoy.com/view/7ljcRW by Fewes.

====================================================================================
*/

void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    if (DISCARD_MAP) {
        return;
    }

    vec2 uv = fragCoord / BUFFER_SIZE;
    uv += TIME_SCROLL_OFFSET_INT;
    
    vec3 color = vec3(0.0);
    
    float a = 0.5;
    float f = 2.0;
    for (int i = 0; i < 8; i++) {
        color += noised(uv * f) * a;
        a *= 0.95;
        f *= 2.0;
    }

    fragColor = vec4(color, 1.0);
}
