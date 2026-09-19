//------------------------------------------------------------------------------
// Shadow Sampling Types
//------------------------------------------------------------------------------

// Keep this in sync with PerViewUniforms.h
#define SHADOW_SAMPLING_RUNTIME_PCF     0u
#define SHADOW_SAMPLING_RUNTIME_EVSM    1u
#define SHADOW_SAMPLING_RUNTIME_EVSSM   2u
#define SHADOW_SAMPLING_RUNTIME_DPCF    3u   // lecodes 0039

#define SHADOW_SAMPLING_PCF_HARD        0
#define SHADOW_SAMPLING_PCF_LOW         1

// number of samples for EVSSM, must be 1, 5 or 9
#define SHADOW_SAMPLING_EVSSM_TAPS      5
// whether to use a rotated noise (1) or not (0)
// rotated noise is most effective with 9 taps.
#define SHADOW_SAMPLING_EVSSM_NOISE     0

//------------------------------------------------------------------------------
// PCF Shadow Sampling
//------------------------------------------------------------------------------

// use hardware assisted PCF
float ShadowSample_PCF_Hard(const mediump sampler2DArrayShadow map,
        const highp vec4 scissorNormalized,
        const uint layer, const highp vec4 shadowPosition) {
    // note: shadowPosition.z is in the [1, 0] range (reversed Z)
    highp vec3 position = shadowPosition.xyz * (1.0 / shadowPosition.w);
    position.xy = clamp(position.xy, scissorNormalized.xy, scissorNormalized.zw);
    position.z = saturate(position.z);
    return texture(map, vec4(position.xy, layer, position.z));
}

// use hardware assisted PCF + 3x3 gaussian filter
float ShadowSample_PCF_Low(const mediump sampler2DArrayShadow map,
        const highp vec4 scissorNormalized,
        const uint layer, const highp vec4 shadowPosition) {

    highp vec2 size = vec2(frameUniforms.shadowAtlasResolution.x);
    highp vec2 texelSize = vec2(frameUniforms.shadowAtlasResolution.y);

    // note: shadowPosition.z is in the [1, 0] range (reversed Z)
    highp vec3 position = shadowPosition.xyz * (1.0 / shadowPosition.w);
    position.z = saturate(position.z);

    //  Castaño, 2013, "Shadow Mapping Summary Part 1"

    // clamp position to avoid overflows below, which cause some GPUs to abort
    position.xy = clamp(position.xy, vec2(-1.0), vec2(2.0));

    vec2 offset = vec2(0.5);
    highp vec2 uv = (position.xy * size) + offset;
    highp vec2 base = (floor(uv) - offset) * texelSize;
    highp vec2 st = fract(uv);

    vec2 uw = vec2(3.0 - 2.0 * st.x, 1.0 + 2.0 * st.x);
    vec2 vw = vec2(3.0 - 2.0 * st.y, 1.0 + 2.0 * st.y);

    highp vec2 u = vec2((2.0 - st.x) / uw.x - 1.0, st.x / uw.y + 1.0);
    highp vec2 v = vec2((2.0 - st.y) / vw.x - 1.0, st.y / vw.y + 1.0);

    u *= texelSize.x;
    v *= texelSize.y;

    float w0 = uw.x * vw.x;
    float w1 = uw.y * vw.x;
    float w2 = uw.x * vw.y;
    float w3 = uw.y * vw.y;

    highp vec2 uv0 = base + vec2(u.x, v.x);
    highp vec2 uv1 = base + vec2(u.y, v.x);
    highp vec2 uv2 = base + vec2(u.x, v.y);
    highp vec2 uv3 = base + vec2(u.y, v.y);

    uv0 = clamp(uv0, scissorNormalized.xy, scissorNormalized.zw);
    uv1 = clamp(uv1, scissorNormalized.xy, scissorNormalized.zw);
    uv2 = clamp(uv2, scissorNormalized.xy, scissorNormalized.zw);
    uv3 = clamp(uv3, scissorNormalized.xy, scissorNormalized.zw);

    float sum = 0.0;
    sum += w0 * texture(map, vec4(uv0, layer, position.z));
    sum += w1 * texture(map, vec4(uv1, layer, position.z));
    sum += w2 * texture(map, vec4(uv2, layer, position.z));
    sum += w3 * texture(map, vec4(uv3, layer, position.z));
    return sum * 0.0625;
}

// use manual PCF
float ShadowSample_PCF(const mediump sampler2DArray map,
        const highp vec4 scissorNormalized,
        const uint layer, const highp vec4 shadowPosition) {
    highp vec3 position = shadowPosition.xyz * (1.0 / shadowPosition.w);
    // note: shadowPosition.z is in the [1, 0] range (reversed Z)
    position.xy = clamp(position.xy, scissorNormalized.xy, scissorNormalized.zw);
    position.z = saturate(position.z);
    highp float depth = textureLod(map, vec3(position.xy, layer), 0.0).r;
    return step(0.0, position.z - depth);
}

//------------------------------------------------------------------------------
// EVSM
//------------------------------------------------------------------------------

highp vec2 computeTexelSizeInWorldSpace(
        const highp mat4 lightFromWorldMatrix,
        const highp vec2 ndcShadowPosition,
        const highp float w,
        const highp vec2 ndcTexelSize) {
    // This code is an optimized version of ShadowMap::texelSizeWorldSpaceAt(). Here we take advantage of
    // the fact that we know the projection matrix is directional (albeit LiSPSM), that is the projection
    // is independent of z in light space, and we only need the lengths of the vectors.
    // The output should be a constant if LiSPSM is not used, and we could put all this in a
    // uniform. But currently we don't have a LiSPSM variant.
    // For non-LiSPSM the math would simplify to:
    //      highp float len_J0 = 1.0 / length(ST3[0]);
    //      highp float len_J1 = 1.0 / length(ST3[1]);

    // 1. Transpose and downcast to drop the 4th element of S's rows
    highp mat4x3 ST3 = mat4x3(transpose(lightFromWorldMatrix));

    // 2. Uniform Hoisting (ST3[0] is row 0 of S, ST3[1] is row 1, etc.)
    // (this could go into a uniform)
    highp vec3 J_origin_0 = cross(ST3[1], ST3[2]);
    highp vec3 J_origin_1 = cross(ST3[2], ST3[0]);
    highp vec3 cross_23   = cross(ST3[2], ST3[3]);

    // 3. Per-pixel vectors (shadowPosition.z terms are completely gone for LiSPSM)
    highp vec3 k0 = J_origin_0 + (ndcShadowPosition.y * cross_23);
    highp vec3 k1 = J_origin_1 - (ndcShadowPosition.x * cross_23);

    // 4. Per-pixel Determinant
    highp float det_origin = dot(ST3[0], J_origin_0);
    highp float det = det_origin
            + (ndcShadowPosition.y * dot(ST3[0], cross_23))
            - (ndcShadowPosition.x * dot(ST3[3], J_origin_0));

    // 5. Final lengths
    highp float scale = w / abs(det);
    highp float len_J0 = length(k0) * scale;
    highp float len_J1 = length(k1) * scale;
    return vec2(len_J0, len_J1) * ndcTexelSize;
}

float chebyshevUpperBound(const highp vec2 moments, const highp float depth,
        const highp float minVariance, const highp float lbrAmount) {
    // Fast path: if the receiver is fully in front of the caster
    if (depth <= moments.x) {
        return 1.0;
    }

    // Calculate variance with our dynamically injected floor
    highp float variance = max(moments.y - (moments.x * moments.x), minVariance);

    // Standard Chebyshev inequality
    highp float d = depth - moments.x;
    highp float p_max = variance / (variance + d * d);

    // Apply Light Bleeding Reduction (LBR)
    return saturate((p_max - lbrAmount) / (1.0 - lbrAmount));
}

float evaluateEVSM(const bool ELVSM, const float c, const highp vec4 moments, const highp float zReceiver) {
    const highp float EPSILON_MULTIPLIER = 0.002; // could be 0.00001 in fp32
    float lbrAmount = frameUniforms.vsmLightBleedReduction;

    // remap depth to [-1, 1]
    highp float depth = zReceiver * 2.0 - 1.0;

    // positive wrap
    highp float pw = exp(c * depth);
    // Dynamic variance for the positive side (derivative of wraped depth w.r.t. light-space depth via Chain Rule)
    highp float pMinVariance = EPSILON_MULTIPLIER * (pw * pw);
    float p = chebyshevUpperBound(moments.xy, pw, pMinVariance, lbrAmount);

    // negative wrap
    if (ELVSM) {
        highp float nw = -1.0 / pw;
        // Dynamic variance for the negative side (derivative of wraped depth w.r.t. light-space depth via Chain Rule)
        highp float nMinVariance = EPSILON_MULTIPLIER * (nw * nw);
        float n = chebyshevUpperBound(moments.zw, nw, nMinVariance, lbrAmount);
       p = min(p, n);
    }

    return p;
}

float ShadowSample_VSM(const bool DIRECTIONAL, const highp sampler2DArray shadowMap,
        const highp vec4 scissorNormalized,
        const uint layer, const int index,
        const highp vec4 shadowPosition, const highp float zLight) {

    bool ELVSM = bool((shadowUniforms.shadows[index].evsm4_layer >> 8u) & 0x1u);
    mediump vec2 bulbRadius_vsmExponent = unpackHalf2x16(shadowUniforms.shadows[index].bulbRadius_vsmExponent);
    mediump float c = bulbRadius_vsmExponent.y;

    // note: shadowPosition.z is in linear light-space normalized to [0, 1] (near to far plane)
    //  see: ShadowMap::computeVsmLightSpaceMatrix() in ShadowMap.cpp
    //  see: computeLightSpacePosition() in common_shadowing.fs
    highp vec3 position = vec3(shadowPosition.xy * (1.0 / shadowPosition.w), shadowPosition.z);

    // clamp uv to border
    position.xy = clamp(position.xy, scissorNormalized.xy, scissorNormalized.zw);

    // Read the shadow map with all available filtering
    highp vec4 moments = texture(shadowMap, vec3(position.xy, layer));

    return evaluateEVSM(ELVSM, c, moments, position.z);
}

float ShadowSample_EVSSM(const bool DIRECTIONAL, const highp sampler2DArray shadowMap,
        const highp vec4 scissorNormalized,
        const uint layer, const int index,
        const highp vec4 shadowPosition, const highp float zLight) {

    highp vec2 texelSize = vec2(frameUniforms.shadowAtlasResolution.y);

    mediump vec2 maxPenumbraRatio_penumbraRatioScale= unpackHalf2x16(shadowUniforms.shadows[index].maxPenumbraRatio_penumbraRatioScale);
    mediump vec2 bulbRadius_vsmExponent             = unpackHalf2x16(shadowUniforms.shadows[index].bulbRadius_vsmExponent);
    mediump vec2 maxMipLevel_maxSearchRadius        = unpackHalf2x16(shadowUniforms.shadows[index].maxMipLevel_maxSearchRadius);
    mediump float bulbRadius                        = bulbRadius_vsmExponent.x;
    mediump float c                                 = bulbRadius_vsmExponent.y;
    mediump float maxMipLevel                       = maxMipLevel_maxSearchRadius.x;
    mediump float maxSearchRadius                   = maxMipLevel_maxSearchRadius.y;
    mediump float maxPenumbraRatio                  = maxPenumbraRatio_penumbraRatioScale.x;
    mediump float penumbraRatioScale                = maxPenumbraRatio_penumbraRatioScale.y;
    highp float projectionParam                     = shadowUniforms.shadows[index].projectionParam; // (f-n) or n/(f-n)

    highp vec3 position = vec3(shadowPosition.xy * (1.0 / shadowPosition.w), shadowPosition.z);
    position.xy = clamp(position.xy, scissorNormalized.xy, scissorNormalized.zw);

    highp vec2 wsOneOverTexelSizeAtOneMeter;
    if (DIRECTIONAL) {
        wsOneOverTexelSizeAtOneMeter = 1.0 / computeTexelSizeInWorldSpace(
                shadowUniforms.shadows[index].lightFromWorldMatrix,
                position.xy * 2.0 - 1.0, shadowPosition.w,
                2.0f * texelSize);
    } else {
        wsOneOverTexelSizeAtOneMeter = vec2(shadowUniforms.shadows[index].wsOneOverTexelSizeAtOneMeter);
    }

    highp float oneOverZLight = 1.0 / zLight;

    // ==========================================
    // STEP 1: DYNAMIC O(1) BLOCKER SEARCH
    // ==========================================
    highp float physicalSearchRadius;

    if (DIRECTIONAL) {
        // Orthographic/LiSPSM: Search cone widens with physical depth into the cascade.
        // position.z * projectionParam elegantly evaluates to absolute meters from the near plane.
        highp float distanceToNearPlane = position.z * projectionParam;
        physicalSearchRadius = bulbRadius * distanceToNearPlane;
    } else {
        // Perspective (Spot/Point): Angular size of light shrinks with absolute distance.
        physicalSearchRadius = bulbRadius * oneOverZLight;
    }

    // Prevent the physical cone from ever exceeding what Cascade 0 can handle.
    // This mathematically guarantees all cascades search the exact same world-space footprint.
    // This is only needed if we limit the number of LODs
    physicalSearchRadius = min(physicalSearchRadius, maxSearchRadius);

    highp float searchRadiusInTexels = physicalSearchRadius *
            min(wsOneOverTexelSizeAtOneMeter.x, wsOneOverTexelSizeAtOneMeter.y);

    mediump float searchLod = clamp(log2(max(searchRadiusInTexels, 1.0)), 0.0, maxMipLevel);
    highp vec4 searchMoments = textureLod(shadowMap, vec3(position.xy, layer), searchLod);

    // Extract average blocker depth.
    highp float negMoment = max(-searchMoments.z, 1e-8);
    highp float zBlocker = (log(negMoment) / -c + 1.0) * 0.5;
    zBlocker = clamp(zBlocker, 0.0, position.z);

    // =================================================
    // STEP 2: PENUMBRA ESTIMATION (w/ Artistic Shaping)
    // =================================================
    highp float pureGeometricRatio;
    if (DIRECTIONAL) {
        // purely physical meters/units
        pureGeometricRatio = (position.z - zBlocker) * projectionParam;
    } else {
        // purely dimensionless similar-triangles ratio
        pureGeometricRatio = (position.z - zBlocker) / (projectionParam + zBlocker);
    }

    // Optional Artistic Control
    pureGeometricRatio *= penumbraRatioScale;

    // Optional Artistic Control to Squash extreme penumbras.
    // If the geometric ratio implies the penumbra is wider than the distance
    // to the blocker itself, it's likely a 2.5D artifact.
    if (pureGeometricRatio > maxPenumbraRatio) {
        pureGeometricRatio = maxPenumbraRatio + (1.0 - exp(-(pureGeometricRatio - maxPenumbraRatio)));
    }

    // Final physical scaling and projection texture minification
    highp float penumbraWidth;
    if (DIRECTIONAL) {
        penumbraWidth = pureGeometricRatio * bulbRadius;
    } else {
        penumbraWidth = pureGeometricRatio * (bulbRadius * oneOverZLight);
    }

    highp vec2 penumbraWidthInTexels = penumbraWidth * wsOneOverTexelSizeAtOneMeter;

    // ==========================================
    // STEP 3: PENUMBRA FILTERING
    // ==========================================

    // 1. Calculate Base LOD
    mediump float idealLod = log2(max(max(penumbraWidthInTexels.x, penumbraWidthInTexels.y), 1.0));

    #if SHADOW_SAMPLING_EVSSM_TAPS == 1
        // Mathematically derived offset to match the variance of the 5-tap Quincunx.
        // Accounts for the convolution of the 3x3 Gaussian mipmaps + hardware box filter.
        idealLod -= 0.86;
    #elif SHADOW_SAMPLING_EVSSM_TAPS == 5
        // Halves the base LOD to bypass extreme EVSM compression,
        // relying on the spatial Quincunx taps to rebuild the shape.
        idealLod -= 1.0;
    #elif SHADOW_SAMPLING_EVSSM_TAPS == 9
        // 9 taps provide massive spatial coverage. Drop the LOD by 1.5 to 2.0
        // to fetch ultra-high precision moments from the top of the mipchain.
        idealLod -= 1.5;
    #endif

    mediump float targetLod = clamp(idealLod, 0.0, maxMipLevel);
    mediump float lodDeficit = exp2(max(idealLod - targetLod, 0.0));

    // 2. Setup the Spatial Grid (Decoupled Base)
    highp vec2 r = (penumbraWidthInTexels * texelSize) * 0.5 * lodDeficit;
    highp vec2 rotX = vec2(r.x, 0.0);
    highp vec2 rotY = vec2(0.0, r.y);
    highp vec2 anchor = position.xy;

    // 3. Inject Noise (Rotates the grid and jitters the anchor)
    #if defined(SHADOW_SAMPLING_EVSSM_NOISE) && SHADOW_SAMPLING_EVSSM_NOISE == 1
        highp float noise = interleavedGradientNoise(gl_FragCoord.xy);
        highp float theta = noise * (2.0 * PI);
        highp float cosT = cos(theta);
        highp float sinT = sin(theta);

        // Rotate the orthogonal basis vectors
        rotX = vec2( cosT * r.x, sinT * r.x);
        rotY = vec2(-sinT * r.y, cosT * r.y);

        // Micro-jitter the anchor to break up the static core
        anchor += vec2(cosT, sinT) * (texelSize * 0.5);
    #endif

    // Helper macro to keep the sample code perfectly clean and avoid typo-bugs
    #define FETCH_MOMENTS(offset) textureLod(shadowMap, \
            vec3(clamp(anchor + (offset), scissorNormalized.xy, scissorNormalized.zw), layer), targetLod)

    // 4. Tap Evaluation (Cascading Architecture)
    highp vec4 finalMoments = FETCH_MOMENTS(vec2(0.0)); // The Center Tap (Used by 1, 5, and 9)

    #if SHADOW_SAMPLING_EVSSM_TAPS >= 5
        // Corner Taps (Used by 5 and 9)
        highp vec4 sumCorners = FETCH_MOMENTS( rotX + rotY) +
                                FETCH_MOMENTS( rotX - rotY) +
                                FETCH_MOMENTS(-rotX + rotY) +
                                FETCH_MOMENTS(-rotX - rotY);
    #endif

    #if SHADOW_SAMPLING_EVSSM_TAPS >= 9
        // Edge Taps (Used by 9 only)
        highp vec4 sumEdges   = FETCH_MOMENTS( rotX) +
                                FETCH_MOMENTS(-rotX) +
                                FETCH_MOMENTS( rotY) +
                                FETCH_MOMENTS(-rotY);
    #endif

    // 5. Final Assembly
    #if SHADOW_SAMPLING_EVSSM_TAPS == 1
        // 1-Tap: finalMoments is already just the center tap.
    #elif SHADOW_SAMPLING_EVSSM_TAPS == 5
        // 5-Tap Quincunx: Center (0.5) + Corners (4 * 0.125)
        finalMoments = (finalMoments * 0.5) + (sumCorners * 0.125);
    #elif SHADOW_SAMPLING_EVSSM_TAPS == 9
        // 9-Tap Binomial: Center (0.25) + Edges (4 * 0.125) + Corners (4 * 0.0625)
        finalMoments = (finalMoments * 0.25) + (sumEdges * 0.125) + (sumCorners * 0.0625);
    #endif

#undef FETCH_MOMENTS

    return evaluateEVSM(true, c, finalMoments, position.z);
}

//------------------------------------------------------------------------------
// DPCF (lecodes 0039): PCSS on the plain depth map
//------------------------------------------------------------------------------
// Upstream dropped its depth-map DPCF / PCSS for the EVSM one (#10188). The EVSM filters read the blocker and the
// penumbra from what is UNDER THE PIXEL IN THE MAP, so every receiver has to be in the map; a level whose statics are
// baked keeps them out of it (their shadow is in the lightmap) and cannot use those. This one needs the casters only:
// the receiver's depth comes from the fragment. A blocker search over a disk, then a variable-width PCF - in METRES, through
// the gradients of the light matrix, so LiSPSM's warp is accounted for (upstream's version ignored it).

// The taps sit on a Vogel (golden-angle) spiral turned by a per-pixel angle: unlike a rotated Poisson table it has no
// preferred direction left after the turn, so the dither that remains in the penumbra has no hatch pattern to it.
highp vec2 dpcfTap(const uint i, const float count, const highp float phase) {
    highp float r = sqrt((float(i) + 0.5) / count);
    highp float a = float(i) * 2.3999632 + phase;
    return vec2(cos(a), sin(a)) * r;
}

const uint DPCF_SEARCH_TAP_COUNT = 32u;    // more samples = a better shape of the hardened shadow
const uint DPCF_FILTER_TAP_COUNT = 64u;    // fewer samples = a noisier penumbra
const float DPCF_MAX_PENUMBRA    = 0.60;   // metres, the widest penumbra (= twice the blocker search radius)

float ShadowSample_DPCF(const bool DIRECTIONAL,
        const highp sampler2DArray map,
        const highp vec4 scissorNormalized,
        const uint layer, const int index,
        const highp vec4 shadowPosition, const highp float zLight) {
    highp vec2 size = vec2(textureSize(map, 0).xy);
    highp vec2 texelSize = vec2(1.0) / size;
    highp float invW = 1.0 / shadowPosition.w;
    highp vec3 position = shadowPosition.xyz * invW;

    // Receiver-plane depth bias (GDC '06, Shadow Mapping: GPU-based Tips and Techniques): the kernels are wide, and a
    // receiver that is itself in the map would shadow itself along its slope without it.
    highp vec3 dx = dFdx(position);
    highp vec3 dy = dFdy(position);
    highp float det = dx.x * dy.y - dx.y * dy.x;
    highp vec2 dz_duv = vec2(0.0);
    if (abs(det) > 1e-12) {
        dz_duv = vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det;
        // a grazing receiver gives unbounded slopes: hold them to what one texel of constant bias would hide
        dz_duv = clamp(dz_duv, vec2(-64.0), vec2(64.0));
    }
    // note: position.z is in the [1, 0] range (reversed Z): a blocker's depth is ABOVE the receiver's. Saturated AFTER the
    // slopes are taken: a receiver past the light's far plane sits at 0, which is also what an EMPTY texel holds - so a
    // blocker is strictly above the receiver's plane, and that plane never goes under 0. (With >= the whole ground past
    // the far plane - the deepest caster IN VIEW - went dark along a straight line; fps-demo yard, 2026-09-19.)
    position.z = saturate(position.z);

    // How far the map coordinate and the depth move per METRE of world space at this fragment: the gradient of
    // (row . p) / w is (row - value * row3) / w. It holds for the ortho, the LiSPSM-warped and the perspective matrix.
    highp mat4 m = shadowUniforms.shadows[index].lightFromWorldMatrix;
    highp vec3 r0 = vec3(m[0][0], m[1][0], m[2][0]);
    highp vec3 r1 = vec3(m[0][1], m[1][1], m[2][1]);
    highp vec3 r2 = vec3(m[0][2], m[1][2], m[2][2]);
    highp vec3 r3 = vec3(m[0][3], m[1][3], m[2][3]);
    highp vec2 uvPerMetre = vec2(length(r0 - position.x * r3), length(r1 - position.y * r3)) * abs(invW);
    highp float zPerMetre = max(length(r2 - position.z * r3) * abs(invW), 1e-6);

    mediump float bulbRadius = unpackHalf2x16(shadowUniforms.shadows[index].bulbRadius_vsmExponent).x;
    // penumbra width per metre between the blocker and the receiver
    highp float spread = DIRECTIONAL ? bulbRadius : bulbRadius / max(zLight, 0.05);

    // turn the spiral by a per-pixel angle
    highp float phase = interleavedGradientNoise(gl_FragCoord.xy + vec2(frameUniforms.temporalNoise)) * (2.0 * PI);

    // 1. blocker search, over the widest penumbra this receiver can get: its blocker cannot be further than the near plane
    highp float reach = (1.0 - position.z) / zPerMetre;
    highp float searchRadius = 0.5 * min(spread * reach, DPCF_MAX_PENUMBRA);
    highp vec2 searchRadii = max(searchRadius * uvPerMetre, texelSize);
    highp float blockers = 0.0;
    highp float zBlockers = 0.0;
    for (uint i = 0u; i < DPCF_SEARCH_TAP_COUNT; i++) {
        highp vec2 duv = dpcfTap(i, float(DPCF_SEARCH_TAP_COUNT), phase) * searchRadii;
        highp vec2 tc = clamp(position.xy + duv, scissorNormalized.xy, scissorNormalized.zw);
        highp float z = textureLod(map, vec3(tc, layer), 0.0).r;
        highp float blocked = float(z > max(position.z + dot(dz_duv, duv), 0.0));   // the receiver's own plane at this tap
        blockers += blocked;
        zBlockers += z * blocked;
    }
    if (blockers == 0.0) {
        return 1.0;
    }

    // 2. the penumbra, from the blockers' mean distance
    highp float distance = max(zBlockers / blockers - position.z, 0.0) / zPerMetre;
    highp float penumbra = min(spread * distance, DPCF_MAX_PENUMBRA);
    // never under 1.5 texels: that is the hardest edge the map can draw without a staircase
    highp vec2 filterRadii = max(0.5 * penumbra * uvPerMetre, 1.5 * texelSize);

    // 3. the filter: each tap is a 2x2 bilinear PCF, which helps a lot where the map is coarse
    highp float occluded = 0.0;
    for (uint i = 0u; i < DPCF_FILTER_TAP_COUNT; i++) {
        highp vec2 duv = dpcfTap(i, float(DPCF_FILTER_TAP_COUNT), phase) * filterRadii;
        highp vec2 tc = clamp(position.xy + duv, scissorNormalized.xy, scissorNormalized.zw);
        highp vec2 st = tc * size - 0.5;
        highp vec2 grad = fract(st);
        highp vec4 d;
        d[0] = texelFetchOffset(map, ivec3(st, layer), 0, ivec2(0, 1)).r;
        d[1] = texelFetchOffset(map, ivec3(st, layer), 0, ivec2(1, 1)).r;
        d[2] = texelFetchOffset(map, ivec3(st, layer), 0, ivec2(1, 0)).r;
        d[3] = texelFetchOffset(map, ivec3(st, layer), 0, ivec2(0, 0)).r;
        highp vec4 pcf = vec4(greaterThan(d, vec4(max(position.z + dot(dz_duv, duv), 0.0))));
        occluded += mix(mix(pcf.w, pcf.z, grad.x), mix(pcf.x, pcf.y, grad.x), grad.y);
    }
    return 1.0 - occluded * (1.0 / float(DPCF_FILTER_TAP_COUNT));
}

//------------------------------------------------------------------------------
// Screen-space Contact Shadows
//------------------------------------------------------------------------------

struct ScreenSpaceRay {
    highp vec3 ssRayStart;
    highp vec3 ssRayEnd;
    highp vec3 ssViewRayEnd;
    highp vec3 uvRayStart;
    highp vec3 uvRay;
};

void initScreenSpaceRay(out ScreenSpaceRay ray, highp vec3 wsRayStart, vec3 wsRayDirection, float wsRayLength) {
    highp mat4 worldToClip = getClipFromWorldMatrix();
    highp mat4 viewToClip = getClipFromViewMatrix();

    // ray end in world space
    highp vec3 wsRayEnd = wsRayStart + wsRayDirection * wsRayLength;

    // ray start/end in clip space (z is inverted: [1,0])
    highp vec4 csRayStart = worldToClip * vec4(wsRayStart, 1.0);
    highp vec4 csRayEnd = worldToClip * vec4(wsRayEnd, 1.0);
    highp vec4 csViewRayEnd = csRayStart + viewToClip * vec4(0.0, 0.0, wsRayLength, 0.0);

    // ray start/end in screen space (z is inverted: [1,0])
    ray.ssRayStart = csRayStart.xyz * (1.0 / csRayStart.w);
    ray.ssRayEnd = csRayEnd.xyz * (1.0 / csRayEnd.w);
    ray.ssViewRayEnd = csViewRayEnd.xyz * (1.0 / csViewRayEnd.w);

    // convert all to uv (texture) space (z is inverted: [1,0])
    highp vec3 uvRayEnd = vec3(ray.ssRayEnd.xy * 0.5 + 0.5, ray.ssRayEnd.z);
    ray.uvRayStart = vec3(ray.ssRayStart.xy * 0.5 + 0.5, ray.ssRayStart.z);
    ray.uvRay = uvRayEnd - ray.uvRayStart;
}

float screenSpaceContactShadow(vec3 lightDirection) {
    // cast a ray in the direction of the light
    float occlusion = 0.0;
    int kStepCount = (frameUniforms.directionalShadows >> 8) & 0xFF;
    float kDistanceMax = frameUniforms.ssContactShadowDistance;

    ScreenSpaceRay rayData;
    initScreenSpaceRay(rayData, shading_position, lightDirection, kDistanceMax);

    // step
    highp float dt = 1.0 / float(kStepCount);

    // tolerance
    highp float tolerance = abs(rayData.ssViewRayEnd.z - rayData.ssRayStart.z) * dt;

    // dither the ray with interleaved gradient noise
    float dither = interleavedGradientNoise(gl_FragCoord.xy) - 0.5;

    // normalized position on the ray (0 to 1)
    highp float t = dt * dither + dt;

    highp vec3 ray;
    for (int i = 0 ; i < kStepCount ; i++, t += dt) {
        ray = rayData.uvRayStart + rayData.uvRay * t;
        highp float z = textureLod(sampler0_structure, uvToRenderTargetUV(ray.xy), 0.0).r;
        highp float dz = z - ray.z;
        if (abs(tolerance - dz) < tolerance) {
            occlusion = 1.0;
            break;
        }
    }

    // we fade out the contribution of contact shadows towards the edge of the screen
    // because we don't have depth data there
    vec2 fade = max(12.0 * abs(ray.xy - 0.5) - 5.0, 0.0);
    occlusion *= saturate(1.0 - dot(fade, fade));
    return occlusion;
}

//------------------------------------------------------------------------------
// Shadow sampling dispatch
//------------------------------------------------------------------------------

/**
 * Samples the light visibility at the specified position in light (shadow)
 * space. The output is a filtered visibility factor that can be used to multiply
 * the light intensity.
 */

#if defined(MATERIAL_HAS_LIGHTING)
// get texture coordinate for directional and spot shadow maps
highp vec4 getShadowPosition(const int cascade) {
    return getCascadeLightSpacePosition(cascade);
}

highp vec4 getShadowPosition(const int index,  const highp vec3 dir, const highp float zLight) {
    return getSpotLightSpacePosition(index, dir, zLight);
}
#endif

int getPointLightFace(const highp vec3 r) {
    highp vec4 tc;
    highp float rx = abs(r.x);
    highp float ry = abs(r.y);
    highp float rz = abs(r.z);
    highp float d = max(rx, max(ry, rz));
    if (d == rx) {
        return (r.x >= 0.0 ? 0 : 1);
    } else if (d == ry) {
        return (r.y >= 0.0 ? 2 : 3);
    } else {
        return (r.z >= 0.0 ? 4 : 5);
    }
}

#if defined(MATERIAL_HAS_SHADOW_STRENGTH)
void applyShadowStrength(inout float visibility, float strength) {
    visibility = 1.0 - (1.0 - visibility) * strength;
}
#endif

// PCF sampling
float shadow(const bool DIRECTIONAL,
        const mediump sampler2DArrayShadow shadowMap,
        const int index, highp vec4 shadowPosition, highp float zLight) {
    highp vec4 scissorNormalized = shadowUniforms.shadows[index].scissorNormalized;
    uint layer = shadowUniforms.shadows[index].evsm4_layer & 0xFFu;

    if (CONFIG_SHADOW_SAMPLING_METHOD == SHADOW_SAMPLING_PCF_HARD) {
        return ShadowSample_PCF_Hard(shadowMap, scissorNormalized, layer, shadowPosition);
    } else if (CONFIG_SHADOW_SAMPLING_METHOD == SHADOW_SAMPLING_PCF_LOW) {
        return ShadowSample_PCF_Low(shadowMap, scissorNormalized, layer, shadowPosition);
    }

    // should not happen
    return 0.0;
}

// Shadow requiring a sampler2D sampler (EVSM and EVSSM)
float shadow(const bool DIRECTIONAL,
        const highp sampler2DArray shadowMap,
        const int index, highp vec4 shadowPosition, highp float zLight) {
    highp vec4 scissorNormalized = shadowUniforms.shadows[index].scissorNormalized;
    uint layer = shadowUniforms.shadows[index].evsm4_layer & 0xFFu;
    // This conditional is resolved at compile time
    if (frameUniforms.shadowSamplingType == SHADOW_SAMPLING_RUNTIME_EVSM) {
        return ShadowSample_VSM(DIRECTIONAL, shadowMap, scissorNormalized, layer, index,
                shadowPosition, zLight);
    }

    if (frameUniforms.shadowSamplingType == SHADOW_SAMPLING_RUNTIME_EVSSM) {
        return ShadowSample_EVSSM(DIRECTIONAL, shadowMap, scissorNormalized, layer, index,
                shadowPosition, zLight);
    }

    if (frameUniforms.shadowSamplingType == SHADOW_SAMPLING_RUNTIME_DPCF) {   // lecodes 0039
        return ShadowSample_DPCF(DIRECTIONAL, shadowMap, scissorNormalized, layer, index,
                shadowPosition, zLight);
    }

    if (frameUniforms.shadowSamplingType == SHADOW_SAMPLING_RUNTIME_PCF) {
        // This is here mostly for debugging at this point.
        // Note: In this codepath, the normal bias is not applied because we're in the VSM variant.
        // (see: get{Cascade|Spot}LightSpacePosition)
        return ShadowSample_PCF(shadowMap, scissorNormalized, layer,
                shadowPosition);
    }

    // should not happen
    return 0.0;
}
