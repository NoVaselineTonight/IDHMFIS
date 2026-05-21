// post_process.hlsl — Post-processing compute shader.
// Reads the HDR RGBA16F beam accumulation buffer and produces an SDR RGBA8 output.
//
// Three logical passes (selected by gPassMode encoded in MSByte of gFrameCounter):
//   Mode 0: Horizontal Gaussian bloom pass  — HDR → BloomTmp
//   Mode 1: Vertical Gaussian bloom pass    — BloomTmp → BloomTmp (in-place)
//   Mode 2: Compose — HDR + Bloom + Haze + Exposure tone-map + Grain → SDR
//
// Dispatch: ceil(width/8) × ceil(height/8) × 1 for each of the three passes.
//
// Requires: Shader Model 5.1, D3D12.

// ── Resource bindings ───────────────────────────────────────────────────────

// HDR accumulation input (read-only in post-process)
Texture2D<float4>   gHDR      : register(t0);

// Bloom temporary buffer (written by pass 0, read/written by pass 1, read by pass 2)
Texture2D<float4>   gBloomTmp : register(t1);

// Output UAVs
RWTexture2D<float4> gBloomOut : register(u0);  // BloomTmp (passes 0 and 1)
RWTexture2D<float4> gSDR      : register(u1);  // SDR output (pass 2)

// Linear clamp sampler for smooth bloom reads
SamplerState gLinearClamp : register(s0);

// ── Constant buffer ─────────────────────────────────────────────────────────

cbuffer PostCB : register(b0) {
    uint  gWidth;           // Render target dimensions
    uint  gHeight;
    float gBloomRadius;     // Bloom Gaussian sigma in pixels
    float gHazeDensity;     // 0–1 atmospheric fog strength
    float gExposure;        // Linear exposure multiplier
    uint  gFilmGrain;       // 0 = disabled, 1 = enabled
    float gGrainAmount;     // Noise amplitude (normalised 0–1)
    uint  gFrameCounter;    // Monotonic frame counter
                            // MSByte (bits 24-31) encodes gPassMode (0/1/2)
    float gPad[8];
};

// ── Hash / noise ─────────────────────────────────────────────────────────────

// Integer hash → uniformly distributed uint, mapped to [0,1)
float Hash(uint2 p, uint seed) {
    uint n = p.x * 1619u + p.y * 31337u + seed * 1013904223u;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = n ^ (n >> 16u);
    return float(n) * (1.0f / 4294967296.0f);
}

// ── Tone mapping ─────────────────────────────────────────────────────────────

// Reinhard extended — prevents overshooting white.
// maxWhite: the scene luminance value that maps to output 1.0.
// Lower values create a more punchy/constrasty look.
float3 ReinhardExtended(float3 v, float maxWhite) {
    float3 numerator = v * (1.0f + v / (maxWhite * maxWhite));
    return numerator / (1.0f + v);
}

// ── Gaussian kernel ──────────────────────────────────────────────────────────

// Unnormalised 1D Gaussian weight at position x with standard deviation sigma.
// Normalisation happens implicitly in the accumulation loop.
float GaussWeight(float x, float sigma) {
    return exp(-(x * x) / (2.0f * sigma * sigma));
}

// ── Pass implementations ─────────────────────────────────────────────────────

// Pass 0: Horizontal bloom.
// Extracts the overexposed portion of the HDR signal (values > 1.0) and blurs
// horizontally into gBloomOut.  The threshold prevents bloom on the inner beam
// core (which is already handled by the Gaussian rasteriser).
void BloomHorizontal(uint2 px) {
    float sigma  = max(gBloomRadius, 0.5f);
    int   radius = (int)ceil(sigma * 3.0f);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int d = -radius; d <= radius; ++d) {
        int sx = clamp((int)px.x + d, 0, (int)gWidth - 1);
        float  w   = GaussWeight((float)d, sigma);
        float3 tap = gHDR.Load(int3(sx, px.y, 0)).rgb;
        // Threshold: only bloom where beam intensity exceeds peak (>1.0 in HDR)
        float3 bright = max(tap - 1.0f, 0.0f);
        sum  += bright * w;
        wsum += w;
    }

    gBloomOut[px] = float4(sum / max(wsum, 1e-6f), 0.0f);
}

// Pass 1: Vertical bloom.
// Reads from gBloomTmp (which holds the horizontally-blurred result from pass 0)
// and writes the final separable bloom back into gBloomOut.
void BloomVertical(uint2 px) {
    float sigma  = max(gBloomRadius, 0.5f);
    int   radius = (int)ceil(sigma * 3.0f);

    float3 sum  = float3(0.0f, 0.0f, 0.0f);
    float  wsum = 0.0f;

    for (int d = -radius; d <= radius; ++d) {
        int sy = clamp((int)px.y + d, 0, (int)gHeight - 1);
        float  w = GaussWeight((float)d, sigma);
        sum  += gBloomTmp.Load(int3(px.x, sy, 0)).rgb * w;
        wsum += w;
    }

    gBloomOut[px] = float4(sum / max(wsum, 1e-6f), 0.0f);
}

// Pass 2: Composition → SDR.
// Combines HDR beam accumulation with bloom, applies haze, exposure, and grain,
// tone-maps to SDR, and applies gamma correction.
void Compose(uint2 px) {
    float3 hdr   = gHDR.Load(int3(px, 0)).rgb;
    float3 bloom = gBloomTmp.Load(int3(px, 0)).rgb;

    // Additive bloom — the blur extends the visible glow radius well beyond
    // the mathematical beam width, simulating volumetric light scatter in haze.
    float3 color = hdr + bloom;

    // ── Haze / atmospheric scatter ──────────────────────────────────────────
    // Modelled as a radially-varying ambient fog layer.  In a real haze-filled
    // room, laser beams illuminate suspended particles; the haze term adds a
    // faint ambient contribution that falls off from the screen centre.
    // A light-blue tint (0.06, 0.08, 0.12) approximates Rayleigh scattering.
    float2 uv      = (float2(px) + 0.5f) / float2(gWidth, gHeight);
    float2 centred = uv * 2.0f - 1.0f;
    float  dist    = length(centred);

    // Haze transparency: exp(-density * dist²) — denser fog near edges
    float  haze_t    = exp(-dist * dist * gHazeDensity * 2.0f);
    float3 haze_col  = float3(0.06f, 0.08f, 0.12f) * gHazeDensity;

    // Attenuate beam color slightly toward the edges, add ambient haze fill
    color = color * (1.0f - gHazeDensity * 0.15f) + haze_col * (1.0f - haze_t);

    // ── Exposure ────────────────────────────────────────────────────────────
    // Linear exposure acts like adjusting the virtual "camera" f-stop.
    // Values > 1 brighten the image; values < 1 darken it.
    color *= gExposure;

    // ── HDR → SDR tone-map ──────────────────────────────────────────────────
    // Reinhard extended with white point 4.0 — allows the brightest beams
    // (which can accumulate 3-5× in HDR) to compress gracefully without clipping.
    color = ReinhardExtended(color, 4.0f);

    // ── sRGB gamma correction ───────────────────────────────────────────────
    // Approximate sRGB OETF: L = L^(1/2.2).
    // Full piecewise sRGB would be more accurate but the difference is subtle
    // at the low luminance values typical for laser shows.
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    // ── Film grain ──────────────────────────────────────────────────────────
    // Per-pixel white noise seeded by pixel position and frame counter.
    // Adds subtle sensor-like noise that hides banding in dark regions.
    if (gFilmGrain != 0u) {
        uint seed = gFrameCounter & 0x00FFFFFFu; // strip the pass mode
        float noise = Hash(px, seed) * 2.0f - 1.0f;  // centre at 0
        color += noise * gGrainAmount;
    }

    // Final clamp to [0,1] before writing to UNORM UAV
    color = clamp(color, 0.0f, 1.0f);

    gSDR[px] = float4(color.r, color.g, color.b, 1.0f);
}

// ── Entry point ──────────────────────────────────────────────────────────────

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= gWidth || px.y >= gHeight) return;

    // Pass mode is encoded in the top byte of gFrameCounter so we can share
    // a single constant buffer across all three passes.
    uint passMode = (gFrameCounter >> 24u) & 0x3u;

    if      (passMode == 0u) BloomHorizontal(px);
    else if (passMode == 1u) BloomVertical(px);
    else                     Compose(px);
}
