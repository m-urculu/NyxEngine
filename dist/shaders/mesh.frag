#version 450

// PBR fragment shader (Cook-Torrance, metallic-roughness workflow).
// Shared by the static and skinned mesh pipelines.

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec3 fragDiffIBL;       // computed per-vertex, interpolated
layout(location = 5) flat in float occludable;  // 1 = planet terrain (cuttable); 0 = entities

struct GpuLightData {
    vec4 positionAndType;
    vec4 colorAndIntensity;
    vec4 params;
};

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 projection;
    vec4 ambientColor;     // xyz = color, w = intensity (legacy; IBL replaces this)
    vec4 cameraPosition;
    GpuLightData lights[8];
    ivec4 lightCountAndPad;
    vec4 skyTop;           // analytic-sky IBL: zenith / horizon / ground
    vec4 skyHorizon;
    vec4 skyGround;
    mat4 lightSpace;       // sun shadow map projection (world → light clip space)
    vec4 occluder;         // xyz = character (camera-relative), w = cos(tight cone half-angle)
} ubo;

// Sun shadow map (depth texture, written each frame from the sun's POV). Sampled
// only for directional lights. sampler2DShadow + a comparison sampler does
// hardware bilinear PCF compare in one texture() call.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

// Point-light shadow cubes. Each cube stores linear distance from the light to
// the nearest surface, normalised by the light's radius. mesh.frag picks one
// per shadowed point light via params.y; -1 means "no shadow".
#define MAX_POINT_SHADOWS 4
layout(set = 0, binding = 2) uniform samplerCube pointShadowCubes[MAX_POINT_SHADOWS];

// Manual cube shadow compare. dir = fragment - light, normalised by radius.
// Sample the cube with the world-space direction; compare the stored linear
// distance against the actual one (with a small bias). Returns 0 = in shadow,
// 1 = lit.
float samplePointShadow(int shadowIdx, vec3 fragWp, vec3 lightWp, float radius) {
    vec3  toFrag = fragWp - lightWp;
    float refDist = length(toFrag) / max(radius, 1e-4);
    float stored = 1.0;
    // GLSL requires constant indexing into sampler arrays on some hardware, so
    // dispatch by switch over the small fixed set.
    if      (shadowIdx == 0) stored = texture(pointShadowCubes[0], toFrag).r;
    else if (shadowIdx == 1) stored = texture(pointShadowCubes[1], toFrag).r;
    else if (shadowIdx == 2) stored = texture(pointShadowCubes[2], toFrag).r;
    else                     stored = texture(pointShadowCubes[3], toFrag).r;
    return refDist - 0.01 > stored ? 0.0 : 1.0;
}

// Sample the sky gradient by direction — fast variant used for IBL shading only
// (the visible skybox in sky.frag keeps the smoother smoothstep version). For the
// low-frequency irradiance used here, a branchless 3-color weighted blend is
// indistinguishable from the smooth version and ~5× cheaper per call. Called twice
// per pixel (N and R), so this is the hot path.
vec3 sampleSky(vec3 dir) {
    float u = max( dir.y, 0.0);                     // weight toward zenith
    float d = max(-dir.y, 0.0);                     // weight toward ground
    vec3 col = ubo.skyHorizon.rgb * (1.0 - u - d)
             + ubo.skyTop.rgb     * u
             + ubo.skyGround.rgb  * d;
    return col * ubo.skyHorizon.w;
}

// Sun shadow: project worldPos into light clip space, do a single hardware-PCF
// compare. The comparison sampler does bilinear-filtered depth-compare in one
// texture() call (returns 0..1 visibility). Out-of-frustum: clamp-to-border on
// FLOAT_OPAQUE_WHITE returns 1.0 (fully lit) naturally.
float sampleShadow(vec3 worldPos) {
    vec4 lp  = ubo.lightSpace * vec4(worldPos, 1.0);
    vec3 ndc = lp.xyz / lp.w;
    vec2 uv  = ndc.xy * 0.5 + 0.5;                  // [-1,1] → [0,1]
    if (ndc.z > 1.0) return 1.0;                    // past the shadow far plane
    float curDepth = ndc.z - 0.002;                 // small bias to avoid self-shadow acne
    return texture(shadowMap, vec3(uv, curDepth));  // hw PCF compare, 4-tap filtered
}

// ACES filmic tonemap (Krzysztof Narkowicz approximation). Maps linear HDR colour
// into [0,1] LDR while preserving highlights and avoiding the cream-pink clipping
// the bright sky produced before tonemapping.
vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Material maps (set 1): 0 = base color (sRGB), 2 = normal, 3 = metal-rough, 4 = occlusion.
layout(set = 1, binding = 0) uniform sampler2D baseColorMap;
layout(set = 1, binding = 2) uniform sampler2D normalMap;
layout(set = 1, binding = 3) uniform sampler2D metalRoughMap;  // G = roughness, B = metallic
layout(set = 1, binding = 4) uniform sampler2D occlusionMap;   // R = ambient occlusion

layout(set = 1, binding = 1) uniform MaterialUBO {
    vec4  baseColorFactor;
    float metallic;
    float roughness;
    float hasNormalMap;
    float hasMetalRoughMap;
    float alphaCutoff;        // >0 = alpha-masked (cutout): discard fragments below this
    float subsurface;         // 0 = off; >0 = wrap-diffuse + back-translucency (skin/wax)
} material;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// Tangent-space normal → world space without precomputed tangents, using screen-space
// derivatives of position + UV (Christian Schüler's cotangent frame).
vec3 applyNormalMap(vec3 N) {
    if (material.hasNormalMap < 0.5) return N;
    vec3 mapN = texture(normalMap, fragTexCoord).xyz * 2.0 - 1.0;

    vec3 dp1 = dFdx(fragWorldPos);
    vec3 dp2 = dFdy(fragWorldPos);
    vec2 duv1 = dFdx(fragTexCoord);
    vec2 duv2 = dFdy(fragTexCoord);

    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // Degenerate UVs (flat-shaded triangle, UV seam, collapsed island) make both T
    // and B zero. inversesqrt(0) = +Inf, and 0*Inf = NaN, which then poisons every
    // subsequent operation that touches N — was the source of size-dependent NaN
    // squares around the sandals in the gladiator after bloom landed.
    float tbMax = max(dot(T, T), dot(B, B));
    if (tbMax < 1e-20) return N;
    float invmax = inversesqrt(tbMax);
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    return normalize(TBN * mapN);
}

float distributionGGX(vec3 N, vec3 H, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float geometrySchlickGGX(float NdotV, float rough) {
    float r = rough + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float rough) {
    return geometrySchlickGGX(max(dot(N, V), 0.0), rough)
         * geometrySchlickGGX(max(dot(N, L), 0.0), rough);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    // Third-person occlusion: the occluding mountain between camera and character is
    // drawn GHOSTED (low opacity) so he shows through, while the rest of the terrain
    // stays opaque. occludable encodes the draw: 0 = entity (never affected); ~1 = the
    // OPAQUE terrain pass (skip the ghost region — it's drawn by the other pass); ~3 =
    // the GHOST terrain pass (draw ONLY the ghost region, at low opacity). Two passes
    // because the ghost pass writes no depth (so BOTH mountain slopes show, not just the
    // near one), which can't coexist with opaque depth-write in one pipeline.
    // Third-person see-through. The character is drawn a SECOND time (overlay pass,
    // occludable>2.5, depth-test GREATER → only where terrain hides him) with a screen-
    // door dither: kept pixels show the lit character, dropped pixels reveal the SOLID
    // terrain behind him. So you see him pixelated "through" the terrain, while the
    // terrain stays fully opaque — the planet never goes hollow/see-into and there are no
    // chunk seams. Terrain and the normal character pass have occludable==0 → unaffected.
    if (occludable > 2.5) {
        ivec2 px = ivec2(gl_FragCoord.xy) >> 1;          // 2×2 blocks → clearly "pixelated"
        if (((px.x + px.y) & 1) == 0) discard;           // drop ~half → solid terrain shows through
    }

    // Base color is sampled from an sRGB texture (auto-linearized by the GPU).
    vec4 baseSample = texture(baseColorMap, fragTexCoord);

    // Alpha cutout (masked transparency): drop fully-transparent fragments so hair
    // cards / foliage / fences render their shape, not their quad. Cull is disabled
    // on the cutout pipeline so both sides of thin cards show.
    float alpha = baseSample.a * material.baseColorFactor.a;
    if (material.alphaCutoff > 0.0 && alpha < material.alphaCutoff) discard;

    vec3 albedo = baseSample.rgb * fragColor * material.baseColorFactor.rgb;

    float metallic  = material.metallic;
    float roughness = material.roughness;
    if (material.hasMetalRoughMap > 0.5) {
        vec3 mr = texture(metalRoughMap, fragTexCoord).rgb; // linear
        roughness *= mr.g;
        metallic  *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    float ao = texture(occlusionMap, fragTexCoord).r;       // default white = 1.0

    vec3 N = applyNormalMap(normalize(fragNormal));
    vec3 V = normalize(ubo.cameraPosition.xyz - fragWorldPos);
    // Dielectric base reflectance is 0.04; skin sits a touch lower (~0.028), which
    // takes the edge off the uniform "coated plastic" sheen. Only shift it for
    // subsurface materials so metals/other dielectrics are untouched.
    float dielectricF0 = (material.subsurface > 0.0) ? 0.028 : 0.04;
    vec3 F0 = mix(vec3(dielectricF0), albedo, metallic);

    vec3 Lo = vec3(0.0);
    int lightCount = ubo.lightCountAndPad.x;
    for (int i = 0; i < lightCount; i++) {
        GpuLightData light = ubo.lights[i];
        vec3  L;
        float pointFalloff = 1.0;
        if (light.positionAndType.w < 0.5) {
            L = -light.positionAndType.xyz;                     // already normalized on CPU
        } else {
            vec3 toLight = light.positionAndType.xyz - fragWorldPos;
            float dist   = length(toLight);
            L = toLight / max(dist, 1e-4);
            float r      = light.params.x;
            pointFalloff = 1.0 / (1.0 + (dist * dist) / (r * r));
        }

        // Back-facing surfaces normally contribute nothing — skip all the GGX math.
        // With subsurface on, light wraps past the terminator and transmits through
        // thin geometry, so back-facing fragments still receive light: keep them.
        float NdotL  = dot(N, L);
        if (NdotL <= 0.0 && material.subsurface <= 0.0) continue;
        float NdotLc = max(NdotL, 0.0);   // clamped cosine for specular + energy

        vec3 radiance = light.colorAndIntensity.xyz * light.colorAndIntensity.w * pointFalloff;
        if (light.positionAndType.w < 0.5) {
            radiance *= sampleShadow(fragWorldPos);
        } else {
            int shadowIdx = int(light.params.y);
            if (shadowIdx >= 0) {
                radiance *= samplePointShadow(shadowIdx, fragWorldPos,
                                              light.positionAndType.xyz, light.params.x);
            }
        }

        vec3  H   = normalize(V + L);
        float NDF = distributionGGX(N, H, roughness);
        float G   = geometrySmith(N, V, L, roughness);
        vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 specular = (NDF * G * F)
                      / max(4.0 * max(dot(N, V), 0.0) * NdotLc, 1e-4);

        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

        // Wrap diffuse: softens the terminator so light bleeds slightly past 90°,
        // mimicking shallow subsurface scattering (skin/wax/leaves). At subsurface=0
        // this is exactly the original Lambert term (NdotLc).
        float diffND = NdotLc;
        if (material.subsurface > 0.0) {
            float w = material.subsurface;
            diffND  = max((NdotL + w) / (1.0 + w), 0.0);
        }
        // Diffuse intentionally not divided by PI so brightness matches the engine's
        // existing light intensities. Specular keeps the clamped cosine.
        Lo += kD * albedo * diffND * radiance + specular * NdotLc * radiance;

        // Back-translucency: a crude transmission lobe for light passing through thin
        // geometry toward the eye (ear rims, nostrils, foliage edges).
        if (material.subsurface > 0.0) {
            float back = pow(clamp(dot(V, -L), 0.0, 1.0), 4.0) * material.subsurface;
            Lo += kD * albedo * back * radiance;
        }
    }

    // ── Analytic-sky IBL ────────────────────────────────────────────────────────
    // Diffuse irradiance is computed per-vertex (fragDiffIBL) — low-frequency, no
    // benefit to per-pixel cost. Specular still samples per-pixel in R (it changes
    // fast with V) and uses roughness² to blur toward the diffuse hemisphere.
    vec3  R         = reflect(-V, N);
    float NdotV     = max(dot(N, V), 0.0);
    vec3  diffIBL   = fragDiffIBL;
    vec3  specIBL   = mix(sampleSky(R), diffIBL, roughness * roughness);
    vec3  Frough    = F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(1.0 - NdotV, 5.0);
    // Skin tweaks (gated on subsurface so non-skin materials are unchanged):
    //  - Specular occlusion (Lagarde/Frostbite): dim the ambient sky reflection in
    //    creases/cavities using AO, so the sheen stops sitting uniformly everywhere.
    //  - Tame the grazing-angle skylight rim (the Fresnel term that reads "plastic"
    //    on a smooth analytic sky) by pulling it down as subsurface rises.
    if (material.subsurface > 0.0) {
        float specOcc = clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
        specIBL *= specOcc;
        Frough  *= mix(1.0, 0.5, clamp(material.subsurface, 0.0, 1.0));
    }
    vec3  kDamb     = (vec3(1.0) - Frough) * (1.0 - metallic);
    vec3  ambient   = (kDamb * diffIBL * albedo + Frough * specIBL) * ao;

    vec3 color = ambient + Lo;
    // No tonemap here — output linear HDR so bloom can extract bright values before
    // dynamic range gets compressed. The composite pass owns ACES.
    // Fully opaque everywhere — the ghost pass's see-through comes from the dither
    // discard above (screen-door), not from alpha. So no blend artifacts at the seams.
    outColor = vec4(color, baseSample.a * material.baseColorFactor.a);
}
