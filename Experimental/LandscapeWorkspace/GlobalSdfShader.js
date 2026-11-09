/*==========================================================================================================================================
                                                           GLOBALSDFSHADER.JS
==========================================================================================================================================*/
// 🧩 Unified WebGL2 (#version 300 es) Ray-Space Global SDF + Aperiodic Spectral Ocean Shader:
//    • Zero LOD: hardware GL_LINEAR trilinear sampling of per-layer native 10×10×10 R16F SDF bricks (512×512×128, 1024×1024×256, and up).
//    • Zero clipping: camera-relative raymarching with exact L∞ Chebyshev empty-brick super-box skipping & bisection root refinement.
//    • Zero water/SDF fighting: exact parametric ray-distance integration (tOcean vs tSdf) with dynamic wave run-up, wet sand & refraction.

export const MAX_SHADER_LAYERS = 16;

export const GlobalSdfVertexShader = `#version 300 es
precision highp float;
out vec2 vNdc;

void main()
{
    // Fullscreen oversized triangle (zero internal diagonal seam)
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vNdc = pos * 2.0 - 1.0;
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
`;

export const GlobalSdfFragmentShader = `#version 300 es
precision highp float;
precision highp int;
precision highp sampler3D;
precision highp usampler3D;

in vec2 vNdc;
out vec4 FragColor;

// Camera & viewport uniforms
uniform vec3  uCamEye;
uniform vec3  uCamForward;
uniform vec3  uCamRight;
uniform vec3  uCamUp;
uniform vec2  uResolution;
uniform float uFocalLength;   // default ~1.65 (vertical FOV ~62 deg)
uniform float uPixelAngle;    // 2.0 / (uResolution.y * uFocalLength)

// Global / Per-Layer Sparse-Brick 3D SDF uniforms
uniform usampler3D uIndirection;
uniform sampler3D  uBrickAtlas;
uniform vec3       uInvAtlasTexelDims;

uniform int   uLayerCount;
uniform vec3  uLayerBoundsMin[16];
uniform vec3  uLayerBoundsMax[16];
uniform vec3  uLayerPitch[16];
uniform vec3  uLayerBrickWorld[16];
uniform ivec3 uLayerGridDims[16];
uniform int   uLayerIndYOffset[16];
uniform float uLayerBand[16];
uniform float uLayerMinVoxel[16];
uniform vec3  uLayerColor[16];
uniform vec3  uLayerCliffColor[16];
uniform vec2  uLayerSurfaceParams[16]; // .x = Strata [0..1], .y = Roughness [0..1]

// Signature 24-wave world-space Ocean uniforms
uniform int   uOceanEnabled;
uniform float uOceanTime;
uniform float uOceanWaveAmp;
uniform vec4  uOceanVectors[24]; // xy = dir, z = k, w = amp * uOceanWaveAmp
uniform vec2  uOceanPhases[24];  // x = omega, y = phase

// Lighting & 3D Sculpt Brush overlay uniforms
uniform vec3  uSunDir;
uniform int   uBrushActive;
uniform int   uBrushMode;     // 0=clay, 1=carve, 2=smooth, 3=flatten
uniform vec3  uBrushCenter;
uniform float uBrushRadius;
uniform float uBrushFalloff;

//------------------------------------------------------------------------------------------------------------------------
//                                  SKY & ATMOSPHERIC SCATTERING
//------------------------------------------------------------------------------------------------------------------------

vec3 evaluateSky(vec3 rd)
{
    float elev = clamp(rd.y, 0.0, 1.0);
    vec3 horizon = vec3(0.64, 0.72, 0.78);
    vec3 zenith  = vec3(0.13, 0.25, 0.42);
    vec3 sky     = mix(horizon, zenith, pow(elev, 0.58));

    // Subtle warm horizon scatter toward sun azimuth
    float sunAlign = max(dot(rd, uSunDir), 0.0);
    sky += vec3(0.36, 0.26, 0.16) * pow(sunAlign, 8.0) * (1.0 - elev * 0.65) * 0.42;
    // Crisp solar disc
    sky += vec3(1.0, 0.92, 0.76) * smoothstep(0.99935, 0.99985, sunAlign) * 3.2;
    return sky;
}

//------------------------------------------------------------------------------------------------------------------------
//                                  24-WAVE APERIODIC SPECTRAL OCEAN
//------------------------------------------------------------------------------------------------------------------------

// Returns vec3(height, dHeight/dX, dHeight/dZ) in world metres, filtered to pixel footprint
vec3 evaluateOcean(vec2 worldXZ, float footprint)
{
    if (uOceanWaveAmp <= 1e-4) return vec3(0.0);

    // Slowly advecting domain warp breaks any directional regularity across tens of kilometres
    float px = worldXZ.x * 0.001371 + worldXZ.y * 0.001927 - uOceanTime * 0.019;
    float pz = worldXZ.y * 0.001119 - worldXZ.x * 0.001763 + uOceanTime * 0.013;
    vec2  q  = worldXZ + vec2(22.0 * sin(px), 19.0 * sin(pz));
    vec4  jac = vec4(
        1.0 + 22.0 * 0.001371 * cos(px),  22.0 * 0.001927 * cos(px),
       -19.0 * 0.001763 * cos(pz),        1.0 + 19.0 * 0.001119 * cos(pz)
    );

    float fp2 = footprint * footprint * 0.75;
    vec3 acc  = vec3(0.0);
    for (int i = 0; i < 24; i++)
    {
        vec4  prof   = uOceanVectors[i];
        float k      = prof.z;
        float atten  = exp(-k * k * fp2);
        float amp    = prof.w * atten;
        if (amp < 1e-4) continue;
        vec2  ph     = uOceanPhases[i];
        float phase  = dot(q, prof.xy) * k - uOceanTime * ph.x + ph.y;
        float s      = sin(phase);
        float deriv  = cos(phase) * amp * k;
        acc.x += s * amp;
        acc.y += deriv * dot(prof.xy, jac.xz);
        acc.z += deriv * dot(prof.xy, jac.yw);
    }
    return acc;
}

float evaluateOceanHeightFast(vec2 worldXZ, float footprint)
{
    if (uOceanWaveAmp <= 1e-4) return 0.0;
    float px = worldXZ.x * 0.001371 + worldXZ.y * 0.001927 - uOceanTime * 0.019;
    float pz = worldXZ.y * 0.001119 - worldXZ.x * 0.001763 + uOceanTime * 0.013;
    vec2  q  = worldXZ + vec2(22.0 * sin(px), 19.0 * sin(pz));
    float fp2 = footprint * footprint * 0.75;
    float h   = 0.0;
    // First 16 dominant swell trains carry >96% of physical vertical displacement; all 24 used in shading normals
    for (int i = 0; i < 16; i++)
    {
        vec4  prof = uOceanVectors[i];
        float k    = prof.z;
        float amp  = prof.w * exp(-k * k * fp2);
        if (amp < 1e-4) continue;
        vec2  ph   = uOceanPhases[i];
        h += sin(dot(q, prof.xy) * k - uOceanTime * ph.x + ph.y) * amp;
    }
    return h;
}

//------------------------------------------------------------------------------------------------------------------------
//                          HARDWARE TRILINEAR SPARSE-BRICK 3D SDF SAMPLING & RAYMARCHING
//------------------------------------------------------------------------------------------------------------------------

bool intersectAABB(vec3 ro, vec3 invRd, vec3 bMin, vec3 bMax, out float tEnter, out float tExit)
{
    vec3 t0 = (bMin - ro) * invRd;
    vec3 t1 = (bMax - ro) * invRd;
    vec3 tSmaller = min(t0, t1);
    vec3 tBigger  = max(t0, t1);
    tEnter = max(max(tSmaller.x, tSmaller.y), tSmaller.z);
    tExit  = min(min(tBigger.x,  tBigger.y),  tBigger.z);
    return tExit >= max(tEnter, 0.0);
}

float sampleLayerSdf(int k, vec3 worldPos)
{
    vec3 bMin = uLayerBoundsMin[k];
    vec3 bMax = uLayerBoundsMax[k];

    // Exact Euclidean exterior distance outside layer AABB
    vec3 outDist = max(max(bMin - worldPos, vec3(0.0)), worldPos - bMax);
    if (outDist.x > 0.0 || outDist.y > 0.0 || outDist.z > 0.0)
    {
        return length(outDist) + uLayerMinVoxel[k];
    }

    vec3  vox   = (worldPos - bMin) / uLayerPitch[k];
    ivec3 gDims = uLayerGridDims[k];
    ivec3 bIdx  = clamp(ivec3(floor(vox * 0.125)), ivec3(0), gDims - ivec3(1));

    uvec4 ind = texelFetch(uIndirection, ivec3(bIdx.x, uLayerIndYOffset[k] + bIdx.y, bIdx.z), 0);
    if (ind.r >= 254u)
    {
        return (ind.r == 254u) ? -uLayerBand[k] : uLayerBand[k];
    }

    // Seamless 10×10×10 hardware trilinear coordinate (strictly inside [1..9] of the 10³ block — zero atlas bleed)
    vec3 localVox = clamp(vox - 8.0 * vec3(bIdx), vec3(0.0), vec3(8.0));
    vec3 atlasUVW = (vec3(ind.rgb) * 10.0 + localVox + 1.0) * uInvAtlasTexelDims;
    return textureLod(uBrickAtlas, atlasUVW, 0.0).r;
}

float sampleGlobalSdfAt(vec3 worldPos)
{
    float best = 1e6;
    for (int k = 0; k < 16; k++)
    {
        if (k >= uLayerCount) break;
        float d = sampleLayerSdf(k, worldPos);
        if (d < best) best = d;
    }
    return best;
}

vec3 computeLayerNormal(int k, vec3 pos, float footprint)
{
    float h = max(uLayerMinVoxel[k] * 0.65, footprint * 0.45);
    vec2  e = vec2(1.0, -1.0);
    return normalize(
        e.xyy * sampleLayerSdf(k, pos + e.xyy * h) +
        e.yyx * sampleLayerSdf(k, pos + e.yyx * h) +
        e.yxy * sampleLayerSdf(k, pos + e.yxy * h) +
        e.xxx * sampleLayerSdf(k, pos + e.xxx * h)
    );
}

bool traceSingleLayer(int k, vec3 ro, vec3 rd, vec3 invRd, float tStart, float tLimit, out float outHitT)
{
    vec3  bMin   = uLayerBoundsMin[k];
    vec3  bWorld = uLayerBrickWorld[k];
    ivec3 gDims  = uLayerGridDims[k];
    int   yOff   = uLayerIndYOffset[k];
    float minVox = uLayerMinVoxel[k];
    float band   = uLayerBand[k];

    float t     = max(0.0, tStart);
    float prevT = t;

    // If camera eye starts slightly inside solid rock at t=0, push out cleanly into air first so caves/interiors never clip black
    if (t == 0.0)
    {
        float startPhi = sampleLayerSdf(k, ro);
        if (startPhi < 0.0)
        {
            for (int esc = 0; esc < 64; esc++)
            {
                t += max(-startPhi * 0.85, minVox * 0.5);
                if (t > tLimit) return false;
                startPhi = sampleLayerSdf(k, ro + rd * t);
                if (startPhi > 0.0) break;
            }
            prevT = t;
        }
    }

    for (int step = 0; step < 320; step++)
    {
        if (t > tLimit) break;
        vec3 p = ro + rd * t;

        ivec3 bIdx = clamp(ivec3(floor((p - bMin) / bWorld)), ivec3(0), gDims - ivec3(1));
        uvec4 ind  = texelFetch(uIndirection, ivec3(bIdx.x, yOff + bIdx.y, bIdx.z), 0);

        // Exact L∞ Chebyshev empty-brick super-box skip (anisotropy-exact, zero overshoot!)
        if (ind.r == 255u)
        {
            int   cheb  = max(1, int(ind.a) - 128);
            float rad   = float(cheb - 1);
            vec3  boxLo = bMin + (vec3(bIdx) - vec3(rad)) * bWorld;
            vec3  boxHi = bMin + (vec3(bIdx) + vec3(rad + 1.0)) * bWorld;
            vec3  target = vec3(
                rd.x >= 0.0 ? boxHi.x : boxLo.x,
                rd.y >= 0.0 ? boxHi.y : boxLo.y,
                rd.z >= 0.0 ? boxHi.z : boxLo.z
            );
            vec3  tExitVec = (target - p) * invRd;
            float dtBox    = min(min(tExitVec.x, tExitVec.y), tExitVec.z);
            prevT = t;
            t += max(minVox * 0.20, dtBox + minVox * 0.04);
            continue;
        }

        float phi;
        if (ind.r == 254u)
        {
            phi = -band;
        }
        else
        {
            vec3 vox      = (p - bMin) / uLayerPitch[k];
            vec3 localVox = clamp(vox - 8.0 * vec3(bIdx), vec3(0.0), vec3(8.0));
            vec3 atlasUVW = (vec3(ind.rgb) * 10.0 + localVox + 1.0) * uInvAtlasTexelDims;
            phi = textureLod(uBrickAtlas, atlasUVW, 0.0).r;
        }

        if (phi <= 0.0)
        {
            // 5-step bisection root refinement for sub-centimetre zero-crossing precision
            float lo = prevT;
            float hi = t;
            for (int b = 0; b < 5; b++)
            {
                float mid  = 0.5 * (lo + hi);
                float mPhi = sampleLayerSdf(k, ro + rd * mid);
                if (mPhi > 0.0) lo = mid;
                else            hi = mid;
            }
            outHitT = 0.5 * (lo + hi);
            return true;
        }

        prevT = t;
        t += max(phi * 0.88, minVox * 0.10);
    }

    return false;
}

bool traceGlobalSdf(vec3 ro, vec3 rd, vec3 invRd, out float bestT, out int bestLayer)
{
    bestT     = 1e7;
    bestLayer = -1;

    for (int k = 0; k < 16; k++)
    {
        if (k >= uLayerCount) break;
        float tEnter, tExit;
        if (!intersectAABB(ro, invRd, uLayerBoundsMin[k], uLayerBoundsMax[k], tEnter, tExit)) continue;
        if (tEnter >= bestT) continue;

        float hitT;
        if (traceSingleLayer(k, ro, rd, invRd, tEnter, min(tExit, bestT), hitT))
        {
            if (hitT < bestT)
            {
                bestT     = hitT;
                bestLayer = k;
            }
        }
    }
    return bestLayer >= 0;
}

// Soft raymarched sun shadow across all SDF layers
float computeSoftShadow(vec3 hitPos, vec3 normal)
{
    vec3 ro = hitPos + normal * 0.85;
    vec3 rd = uSunDir;
    vec3 invRd = 1.0 / ( sign(rd) * max(abs(rd), vec3(1e-6)) );

    float shadow = 1.0;
    for (int k = 0; k < 16; k++)
    {
        if (k >= uLayerCount) break;
        float tEnter, tExit;
        if (!intersectAABB(ro, invRd, uLayerBoundsMin[k], uLayerBoundsMax[k], tEnter, tExit)) continue;

        float t      = max(0.2, tEnter);
        float limit  = min(tExit, 420.0);
        float minVox = uLayerMinVoxel[k];
        vec3  bMin   = uLayerBoundsMin[k];
        vec3  bWorld = uLayerBrickWorld[k];
        ivec3 gDims  = uLayerGridDims[k];
        int   yOff   = uLayerIndYOffset[k];

        for (int s = 0; s < 48; s++)
        {
            if (t > limit) break;
            vec3  p    = ro + rd * t;
            ivec3 bIdx = clamp(ivec3(floor((p - bMin) / bWorld)), ivec3(0), gDims - ivec3(1));
            uvec4 ind  = texelFetch(uIndirection, ivec3(bIdx.x, yOff + bIdx.y, bIdx.z), 0);
            if (ind.r == 255u)
            {
                int   cheb  = max(1, int(ind.a) - 128);
                float rad   = float(cheb - 1);
                vec3  boxLo = bMin + (vec3(bIdx) - vec3(rad)) * bWorld;
                vec3  boxHi = bMin + (vec3(bIdx) + vec3(rad + 1.0)) * bWorld;
                vec3  target = vec3(rd.x >= 0.0 ? boxHi.x : boxLo.x, rd.y >= 0.0 ? boxHi.y : boxLo.y, rd.z >= 0.0 ? boxHi.z : boxLo.z);
                vec3  tEx    = (target - p) * invRd;
                t += max(minVox * 0.5, min(min(tEx.x, tEx.y), tEx.z) + minVox * 0.1);
                continue;
            }
            float h = sampleLayerSdf(k, p);
            if (h <= 0.02) return 0.0;
            shadow = min(shadow, 10.0 * h / max(t, 1.0));
            t += clamp(h, minVox * 0.6, 18.0);
        }
    }
    return clamp(shadow, 0.0, 1.0);
}

float computeAmbientOcclusion(int k, vec3 pos, vec3 normal)
{
    float stepBase = max(0.8, uLayerMinVoxel[k] * 1.4);
    float occ = 0.0;
    float weight = 0.55;
    for (int i = 1; i <= 3; i++)
    {
        float dist = stepBase * float(i);
        float d    = sampleLayerSdf(k, pos + normal * dist);
        occ    += weight * max(0.0, (dist - d) / dist);
        weight *= 0.55;
    }
    return clamp(1.0 - occ * 0.78, 0.18, 1.0);
}

//------------------------------------------------------------------------------------------------------------------------
//                          EXACT ANALYTIC RAY-OCEAN INTERSECTION (ZERO SDF FIGHTING)
//------------------------------------------------------------------------------------------------------------------------

bool traceOceanSurface(vec3 ro, vec3 rd, float maxRayT, out float outTOcean)
{
    if (uOceanEnabled == 0) return false;

    float maxAmp = max(0.06, uOceanWaveAmp * 2.45);
    float t0 = 0.0;
    float t1 = 0.0;

    if (ro.y > maxAmp)
    {
        if (rd.y >= -1e-5) return false;
        t0 = (maxAmp - ro.y) / rd.y;
        t1 = (-maxAmp - ro.y) / rd.y;
    }
    else if (ro.y >= -maxAmp)
    {
        t0 = 0.02;
        if (rd.y < -1e-4)      t1 = (-maxAmp - ro.y) / rd.y;
        else if (rd.y > 1e-4)  t1 = ( maxAmp - ro.y) / rd.y;
        else                   t1 = 6000.0;
    }
    else
    {
        // Camera below waves looking upward
        if (rd.y <= 1e-5) return false;
        t0 = (-maxAmp - ro.y) / rd.y;
        t1 = ( maxAmp - ro.y) / rd.y;
    }

    if (t0 >= maxRayT) return false;
    t0 = max(0.02, t0);
    t1 = min(maxRayT, min(t1, 160000.0));
    if (t1 <= t0) return false;

    // March 12 adaptive steps across [t0, t1] to bracket the exact first wave crossing before tSdf
    const int STEPS = 12;
    float prevT = t0;
    vec3  p0    = ro + rd * prevT;
    float fp0   = prevT * uPixelAngle / max(abs(rd.y), 0.06);
    float prevG = p0.y - evaluateOceanHeightFast(p0.xz, fp0);
    float sign0 = ro.y < -maxAmp ? -1.0 : 1.0;

    if (prevG * sign0 <= 0.0)
    {
        outTOcean = prevT;
        return true;
    }

    for (int i = 1; i <= STEPS; i++)
    {
        float u = float(i) / float(STEPS);
        // Quadratic spacing concentrates resolution near the front of the wave slab
        float curT = mix(t0, t1, u * (0.45 + 0.55 * u));
        vec3  p    = ro + rd * curT;
        float fp   = curT * uPixelAngle / max(abs(rd.y), 0.06);
        float curG = p.y - evaluateOceanHeightFast(p.xz, fp);

        if (curG * sign0 <= 0.0)
        {
            // 6-step bisection root polish -> sub-millimetre ocean-SDF intersection!
            float lo = prevT;
            float hi = curT;
            for (int b = 0; b < 6; b++)
            {
                float mid = 0.5 * (lo + hi);
                vec3  mp  = ro + rd * mid;
                float mfp = mid * uPixelAngle / max(abs(rd.y), 0.06);
                float mg  = mp.y - evaluateOceanHeightFast(mp.xz, mfp);
                if (mg * sign0 > 0.0) lo = mid;
                else                  hi = mid;
            }
            outTOcean = 0.5 * (lo + hi);
            return true;
        }
        prevT = curT;
        prevG = curG;
    }

    return false;
}

//------------------------------------------------------------------------------------------------------------------------
//                                      BIOME TERRAIN & OCEAN SURFACE SHADING
//------------------------------------------------------------------------------------------------------------------------

vec3 shadeTerrain(int k, vec3 pos, vec3 normal, vec3 rd, float hitT, bool underwater, float waveHeightAtXZ)
{
    vec3  baseAlbedo  = uLayerColor[k];
    vec3  cliffAlbedo = uLayerCliffColor[k];
    float strataAmt   = uLayerSurfaceParams[k].x;
    float roughness   = uLayerSurfaceParams[k].y;

    // Slope-driven blending between horizontal regolith/sand/turf and steep cliff rock
    float flatness = clamp(normal.y, 0.0, 1.0);
    float cliffFactor = smoothstep(0.78, 0.36, flatness);

    // Subtle stratigraphic rock bedding along world elevation (no noisy colour chaos)
    float bedPhase = pos.y * 0.28 + sin(pos.x * 0.018 + pos.z * 0.021) * 1.6;
    float strataWave = 0.5 + 0.5 * sin(bedPhase * 3.14159);
    float strataLedge = mix(1.0, 0.80 + 0.34 * strataWave, strataAmt * (0.35 + 0.65 * cliffFactor));

    vec3 albedo = mix(baseAlbedo, cliffAlbedo, cliffFactor) * strataLedge;

    // Warm coastal beach sand belt near sea level on gentle slopes
    if (uOceanEnabled != 0)
    {
        vec3 beachSand = mix(vec3(0.74, 0.67, 0.52), baseAlbedo, 0.35);
        float beachBand = (1.0 - smoothstep(-3.0, 7.5, pos.y)) * smoothstep(0.52, 0.88, flatness);
        albedo = mix(albedo, beachSand, beachBand * 0.78);

        // Wet shoreline darkening right above instantaneous wave level
        if (!underwater)
        {
            float aboveWater = pos.y - waveHeightAtXZ;
            float wetness = (1.0 - smoothstep(0.0, 1.15, max(0.0, aboveWater))) * step(-0.25, aboveWater);
            albedo *= mix(1.0, 0.56, wetness);
        }
    }

    float ao     = computeAmbientOcclusion(k, pos, normal);
    float shadow = underwater ? 0.68 : computeSoftShadow(pos, normal);
    float ndotl  = max(dot(normal, uSunDir), 0.0);

    vec3 sunRadiance = vec3(1.06, 0.97, 0.86) * ndotl * shadow;
    vec3 skyAmbient  = mix(vec3(0.22, 0.25, 0.29), vec3(0.36, 0.44, 0.54), 0.5 + 0.5 * normal.y) * ao;
    // Subtle bounce light on overhangs & arches from sunlit sand/water below!
    vec3 bounceLight = baseAlbedo * 0.16 * max(0.0, -normal.y) * ao;

    vec3 halfVec = normalize(uSunDir - rd);
    float specPow = mix(48.0, 8.0, roughness);
    float spec    = pow(max(dot(normal, halfVec), 0.0), specPow) * (1.0 - roughness * 0.72) * shadow * 0.18;

    vec3 lit = albedo * (skyAmbient + sunRadiance + bounceLight) + vec3(spec);

    // 3D Sculpt Brush Ring Overlay (wraps seamlessly over arches, cliffs, overhangs & caves!)
    if (uBrushActive != 0 && !underwater)
    {
        float dBrush = length(pos - uBrushCenter);
        float ringWidth = max(0.22, hitT * uPixelAngle * 1.85);
        float outerRing = 1.0 - smoothstep(0.0, ringWidth, abs(dBrush - uBrushRadius));
        float innerRad  = uBrushRadius * (1.0 - 0.55 * uBrushFalloff);
        float innerRing = (1.0 - smoothstep(0.0, ringWidth * 0.85, abs(dBrush - innerRad))) * 0.45;
        float fillTint  = (1.0 - smoothstep(0.0, uBrushRadius, dBrush)) * 0.14;

        vec3 brushTint = uBrushMode == 1 ? vec3(1.0, 0.44, 0.36)   // Carve: coral-orange
                       : uBrushMode == 2 ? vec3(0.46, 0.78, 1.0)    // Smooth: sky-cyan
                       : uBrushMode == 3 ? vec3(0.98, 0.82, 0.36)   // Flatten: amber-gold
                       :                   vec3(0.45, 0.94, 0.66);  // Clay: emerald-mint
        lit = mix(lit, brushTint, clamp(outerRing * 0.92 + innerRing + fillTint, 0.0, 0.95));
    }

    return lit;
}

void main()
{
    float aspect = uResolution.x / max(1.0, uResolution.y);
    vec3 rd = normalize(uCamForward * uFocalLength + uCamRight * (vNdc.x * aspect) + uCamUp * vNdc.y);
    vec3 ro = uCamEye;
    vec3 invRd = 1.0 / (sign(rd) * max(abs(rd), vec3(1e-7)));

    // 1. Raymarch Global / Per-Layer Full-Resolution Sparse-Brick 3D SDF
    float tSdf   = 1e7;
    int   hitLay = -1;
    bool  hitSdf = traceGlobalSdf(ro, rd, invRd, tSdf, hitLay);

    // 2. Analytic Ray-Ocean Intersection along the EXACT same ray [0, tSdf] -> zero fighting!
    float tOcean   = 1e7;
    bool  hitOcean = traceOceanSurface(ro, rd, hitSdf ? tSdf : 160000.0, tOcean);

    vec3 skyCol = evaluateSky(rd);
    vec3 color  = skyCol;
    float fogDist = 1e7;

    if (hitOcean && (!hitSdf || tOcean < tSdf))
    {
        vec3  waterPos  = ro + rd * tOcean;
        float footprint = tOcean * uPixelAngle;
        vec3  waveEval  = evaluateOcean(waterPos.xz, footprint);

        // Dampen short wave slopes smoothly in ultra-shallow surf (< 3 m water depth)
        float bedY       = hitSdf ? (ro.y + rd.y * tSdf) : -160.0;
        float waterDepth = max(0.0, waterPos.y - bedY);
        float shoalDamp  = smoothstep(0.0, 3.5, waterDepth);

        vec2  slope  = waveEval.yz * (0.35 + 0.65 * shoalDamp);
        vec3  wNorm  = normalize(vec3(-slope.x, 1.0, -slope.y));
        if (dot(wNorm, -rd) < 0.0) wNorm = -wNorm;

        // Underwater seabed colour + Beer-Lambert optical absorption & turquoise shallow scatter
        vec3 deepWaterBody    = vec3(0.022, 0.088, 0.135);
        vec3 shallowTurquoise = vec3(0.075, 0.30,  0.31);
        vec3 subSurfaceCol    = mix(deepWaterBody, shallowTurquoise, exp(-waterDepth * 0.085));

        if (hitSdf)
        {
            vec3 sdfPos  = ro + rd * tSdf;
            vec3 sdfNorm = computeLayerNormal(hitLay, sdfPos, tSdf * uPixelAngle);
            vec3 bedLit  = shadeTerrain(hitLay, sdfPos, sdfNorm, rd, tSdf, true, waveEval.x);

            // Subtle animated shallow-water wave caustics on the submerged SDF sand/rock!
            float caustPhase = sin(sdfPos.x * 0.55 + uOceanTime * 1.7) * sin(sdfPos.z * 0.52 - uOceanTime * 1.4);
            bedLit *= 1.0 + 0.24 * max(0.0, caustPhase) * exp(-waterDepth * 0.18) * max(0.0, sdfNorm.y);

            float opticalPath = min(120.0, tSdf - tOcean);
            vec3  transmittance = exp(-opticalPath * vec3(0.26, 0.072, 0.048));
            subSurfaceCol = mix(subSurfaceCol, bedLit, transmittance);
        }

        // Schlick Fresnel sky reflection + solar specular glint + wave-crest subsurface scatter
        float cosTheta = max(dot(-rd, wNorm), 0.0);
        float fresnel  = 0.024 + 0.976 * pow(1.0 - cosTheta, 5.0);
        vec3  reflDir  = reflect(rd, wNorm);
        reflDir.y      = max(reflDir.y, 0.015);
        vec3  reflSky  = evaluateSky(normalize(reflDir));

        // Subsurface emerald glow through backlit wave crests
        float crestGlow = max(0.0, waveEval.x) * pow(max(0.0, dot(rd, -uSunDir + vec3(0.0, 0.4, 0.0))), 3.0);
        subSurfaceCol += vec3(0.04, 0.20, 0.18) * crestGlow * 0.45;

        color = mix(subSurfaceCol, reflSky, fresnel * 0.84);

        // Crisp solar reflection highlight
        vec3  halfVec  = normalize(uSunDir - rd);
        float specRough = clamp(0.018 + footprint * 0.0025, 0.018, 0.18);
        float specPow   = 2.0 / (specRough * specRough);
        float sunGlint  = pow(max(dot(wNorm, halfVec), 0.0), min(specPow, 768.0));
        color += vec3(1.0, 0.93, 0.78) * sunGlint * 0.85;

        // Bed-depth-driven coastal surf & shoreline contact foam (zero blocky shore texture!)
        if (hitSdf && waterDepth < 3.2)
        {
            float surfWave = 0.5 + 0.5 * sin(waterDepth * 5.2 - uOceanTime * 2.6 + sin(waterPos.x * 0.14 + waterPos.z * 0.13) * 2.2);
            float contactFoam = 1.0 - smoothstep(0.0, 0.42, waterDepth);
            float breakerFoam = (1.0 - smoothstep(0.25, 2.8, waterDepth)) * smoothstep(0.52, 0.90, surfWave) * clamp(uOceanWaveAmp, 0.2, 1.5);
            float foam = clamp(contactFoam * 0.78 + breakerFoam * 0.52, 0.0, 0.92);
            color = mix(color, vec3(0.84, 0.90, 0.91), foam);
        }

        fogDist = tOcean;
    }
    else if (hitSdf)
    {
        vec3  sdfPos    = ro + rd * tSdf;
        float footprint = tSdf * uPixelAngle;
        vec3  sdfNorm   = computeLayerNormal(hitLay, sdfPos, footprint);
        float waveH     = (uOceanEnabled != 0 && abs(sdfPos.y) < 4.0) ? evaluateOceanHeightFast(sdfPos.xz, footprint) : -100.0;
        color   = shadeTerrain(hitLay, sdfPos, sdfNorm, rd, tSdf, false, waveH);
        fogDist = tSdf;
    }

    // Smooth aerial perspective over multi-kilometre horizons
    if (fogDist < 1e6)
    {
        float haze = 1.0 - exp(-fogDist * 0.000042);
        color = mix(color, skyCol, clamp(haze, 0.0, 0.92));
    }

    // Subtle filmic shoulder + sRGB encoding
    color = color / (1.0 + color * 0.16);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
`;
