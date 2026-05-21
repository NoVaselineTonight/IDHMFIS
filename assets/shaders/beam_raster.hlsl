// beam_raster.hlsl — Laser beam rasteriser compute shader.
// Rasterises vector laser segments into an HDR RGBA16F UAV using
// a Gaussian cross-section beam profile.  Additive blending produces
// realistic beam accumulation for overlapping or closely-spaced beams.
//
// Dispatch: one thread group per 8×8 pixel tile.
// Thread groups: ceil(width/8) × ceil(height/8) × 1.
//
// Requires: Shader Model 5.1 or higher, D3D12.

// ── Structures ─────────────────────────────────────────────────────────────

struct LaserPoint {
    float2 pos;     // Normalised device coordinates: x ∈ [-1,1], y ∈ [-1,1]
    float3 color;   // Linear RGB, 0–1.  Values >1 are valid (HDR).
    int    blanked; // 1 = beam-off travel move — contributes no light
    float  pad;     // Padding to match C++ struct alignment
};

// ── Resource bindings ───────────────────────────────────────────────────────

// u0: HDR accumulation render target.  Written with additive blending.
RWTexture2D<float4>          gOutput : register(u0);

// t0: Laser point structured buffer.  One entry per output point.
StructuredBuffer<LaserPoint> gPoints : register(t0);

// b0: Per-frame constant buffer.
cbuffer PassCB : register(b0) {
    uint  gPointCount;      // Total number of points in gPoints
    float gBeamThickness;   // Beam diameter in pixels (FWHM ≈ 2.35 × sigma)
    uint  gWidth;           // Render target width in pixels
    uint  gHeight;          // Render target height in pixels
    float gPad[4];
};

// ── Utility functions ───────────────────────────────────────────────────────

// NDC → pixel coordinate conversion.
// NDC: x points right, y points up.
// Pixel: x points right, y points down (top-left origin).
float2 NdcToPixel(float2 ndc) {
    float px =  (ndc.x * 0.5f + 0.5f) * float(gWidth);
    float py = (-ndc.y * 0.5f + 0.5f) * float(gHeight);
    return float2(px, py);
}

// Squared perpendicular distance from point P to infinite line through A, B,
// clamped to the finite segment AB.
float DistSqPointSegment(float2 P, float2 A, float2 B) {
    float2 AB = B - A;
    float2 AP = P - A;
    float  t  = dot(AP, AB) / max(dot(AB, AB), 1e-8f);
    t = clamp(t, 0.0f, 1.0f);
    float2 closest = A + t * AB;
    float2 d       = P - closest;
    return dot(d, d);
}

// Linearly interpolate color along the segment at the closest-point parameter t.
float3 ColorAtT(float2 P, float2 A, float2 B, float3 colorA, float3 colorB) {
    float2 AB = B - A;
    float2 AP = P - A;
    float  t  = dot(AP, AB) / max(dot(AB, AB), 1e-8f);
    t = clamp(t, 0.0f, 1.0f);
    return lerp(colorA, colorB, t);
}

// ── Main entry point ────────────────────────────────────────────────────────

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint px = dispatchID.x;
    uint py = dispatchID.y;
    if (px >= gWidth || py >= gHeight) return;

    // Centre of the pixel in pixel coordinates
    float2 pixel = float2(float(px) + 0.5f, float(py) + 0.5f);

    // Gaussian beam parameters.
    // sigma = HWHM (half-width at half-maximum in pixels).
    // FWHM = 2 * sqrt(2 * ln2) * sigma ≈ 2.355 * sigma.
    // We equate FWHM to beam_thickness so sigma = beam_thickness / 2.355.
    // For artistic purposes we use the simpler sigma = beam_thickness / 2,
    // which gives a slightly wider beam that reads better at small sizes.
    float sigma     = max(gBeamThickness * 0.5f, 0.5f);
    float inv2sig2  = 1.0f / (2.0f * sigma * sigma);

    // Cutoff: contributions below exp(−4.5) ≈ 0.011 are discarded.
    // cutoff_dist = 3σ gives exp(−(3σ)²/(2σ²)) = exp(−4.5).
    float cutoff2 = (3.0f * sigma) * (3.0f * sigma);

    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Iterate over consecutive point pairs (segments).
    // Blanked → blanked:  no beam (both blanked = travel)
    // Lit → blanked:      no beam (beam-off transition)
    // Blanked → lit:      no beam (beam-on transition at a new position)
    // Lit → lit:          draw the segment
    for (uint i = 0; i + 1u < gPointCount; ++i) {
        LaserPoint ptA = gPoints[i];
        LaserPoint ptB = gPoints[i + 1u];

        // Skip any segment that involves a blanked point
        if (ptA.blanked != 0 || ptB.blanked != 0) continue;

        float2 pA = NdcToPixel(ptA.pos);
        float2 pB = NdcToPixel(ptB.pos);

        // Fast AABB reject with σ-margin before computing true distance.
        // This avoids the division in DistSqPointSegment for most pixels.
        float margin = 3.0f * sigma;
        float minX   = min(pA.x, pB.x) - margin;
        float maxX   = max(pA.x, pB.x) + margin;
        float minY   = min(pA.y, pB.y) - margin;
        float maxY   = max(pA.y, pB.y) + margin;

        if (float(px) < minX || float(px) > maxX ||
            float(py) < minY || float(py) > maxY) continue;

        float d2 = DistSqPointSegment(pixel, pA, pB);
        if (d2 > cutoff2) continue;

        // Gaussian profile: brightness = exp( −d² / (2σ²) )
        // Peak at d=0 → 1.0; falls to 0.011 at d=3σ.
        float gaussian = exp(-d2 * inv2sig2);

        // Interpolated color along the segment
        float3 col = ColorAtT(pixel, pA, pB, ptA.color, ptB.color);

        // Additive accumulation — laser beams add energy
        accum.rgb += col * gaussian;
    }

    // Additively blend into the HDR buffer.
    // The alpha channel carries accumulated exposure weight (unused in post-process,
    // but available for future use).
    float4 existing = gOutput[uint2(px, py)];
    gOutput[uint2(px, py)] = float4(existing.rgb + accum.rgb, existing.a);
}
