#version 450

// PBR fragment shader (Cook-Torrance, metallic-roughness workflow).
// Shared by the static and skinned mesh pipelines.

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragWorldPos;
layout(location = 3) in vec2 fragTexCoord;
layout(location = 4) in vec3 fragDiffIBL;       // computed per-vertex, interpolated

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
} ubo;

// Sun shadow map (depth texture, written each frame from the sun's POV). Sampled
// only for directional lights; point lights skip shadows (would need cubemaps).
// sampler2DShadow + a comparison sampler does hardware bilinear PCF compare in one
// texture() call — ~9× cheaper than software 3×3 PCF for equivalent softness.
layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

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
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

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

        // Back-facing surfaces contribute nothing — skip all the GGX math entirely.
        // For 3 lights this is a meaningful saving on roughly half the pixels.
        float NdotL = dot(N, L);
        if (NdotL <= 0.0) continue;

        vec3 radiance = light.colorAndIntensity.xyz * light.colorAndIntensity.w * pointFalloff;
        // Only directional lights cast shadows here (point lights would need cubemap
        // shadows — out of scope for this iteration).
        if (light.positionAndType.w < 0.5) {
            radiance *= sampleShadow(fragWorldPos);
        }

        vec3  H   = normalize(V + L);
        float NDF = distributionGGX(N, H, roughness);
        float G   = geometrySmith(N, V, L, roughness);
        vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

        vec3 specular = (NDF * G * F)
                      / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-4);

        vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
        // Diffuse intentionally not divided by PI so brightness matches the engine's
        // existing light intensities.
        Lo += (kD * albedo + specular) * radiance * NdotL;
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
    vec3  kDamb     = (vec3(1.0) - Frough) * (1.0 - metallic);
    vec3  ambient   = (kDamb * diffIBL * albedo + Frough * specIBL) * ao;

    vec3 color = ambient + Lo;
    // No tonemap here — output linear HDR so bloom can extract bright values before
    // dynamic range gets compressed. The composite pass owns ACES.
    outColor = vec4(color, baseSample.a * material.baseColorFactor.a);
}
