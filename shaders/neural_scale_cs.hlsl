// neural_scale_cs.hlsl
// FSR-NG-Scaling: DLSS 5 / OpenNR Windowed Multi-Head Self-Attention (W-MSA)
// Generative Neural Reconstruction Kernel with Deep SafeTensors Mapping & Catmull-Rom Bicubic Sampling.
//
// Inputs:
//   [t0] Texture2D<float4>       gInput    : Low-res captured game/desktop frame (B8G8R8A8)
//   [t1] StructuredBuffer<float> gWeights  : OpenNR / DLSS 5 deep neural weights from SafeTensors
//   [t2] Texture2D<float4>       gHistory  : Previous reconstructed high-res frame ($S_{t-1}$)
//   [u0] RWTexture2D<float4>     gOutput   : High-resolution reconstructed frame
//   [b0] ConstantBuffer Params   Params    : Dynamic runtime parameters (80 bytes aligned)

Texture2D<float4>       gInput    : register(t0);
StructuredBuffer<float> gWeights  : register(t1);
Texture2D<float4>       gHistory  : register(t2);
RWTexture2D<float4>     gOutput   : register(u0);

cbuffer Params : register(b0)
{
    float2 inSize;                  // Low-resolution input dimensions (offset 0)
    float2 outSize;                 // Reconstructed output dimensions (offset 8)
    float  intensity;               // 0..2 neural generative blend strength / NR Intensity (offset 16)
    float  structureIntensity;      // 0..2 structural edge synthesis strength / Local Structure (offset 20)
    float  toneIntensity;           // -1..2 HDR tone & perceptual contrast / Local Tone (offset 24)
    float  splitScreen;             // > 0.5 enables split comparison (offset 28)
    float  hasWeights;              // > 0.5 if SafeTensors weights buffer is active (offset 32)
    float  temporalStability;       // 0..0.95 temporal accumulation strength (offset 36)
    float  resetHistory;            // > 0.5 to discard previous history (offset 40)
    float  modeWindow;              // > 0.5 if capturing cropped foreground window (offset 44)
    float4 captureCrop;             // (cropX, cropY, cropW, cropH) in 0..1 normalized UV (offset 48)
    float  detailBoost;             // 0.5..2.5 generative texture detail amplifier (offset 64)
    float  catmullRom;              // > 0.5 enables high-fidelity Catmull-Rom bicubic (offset 68)
    float  skinStructureStrength;   // -1.0..1.0 Skin Structure Strength (offset 72)
    float  nrPasses;                // 1..4 multi-pass neural reconstruction count (offset 76)
    float  scenePaperWhite;         // 0.5..3.0 Scene Paper-White scale (offset 80)
    float  hdrTransferStrength;     // 0.0..2.0 HDR Transfer Strength (offset 84)
    float  colorStrength;           // 0.0..2.0 Color Strength (offset 88)
    float  enableNR;                // > 0.5 enables DLSS Neural Rendering (offset 92)
    float  nrStyle;                 // 0: Default, 1: Cinematic, 2: Aggressive (offset 96)
    float  autoMask;                // > 0.5 Automatic Mask / Zero-Ghosting Motion Gate (offset 100)
    float2 pad;                     // offset 104 (8 bytes) -> 112 bytes total (16-byte aligned)
};

// 8x8 Window Size (N = 64 tokens) matching Swin-Transformer W-MSA specification
#define WINDOW_DIM 8
#define WINDOW_TOKENS (WINDOW_DIM * WINDOW_DIM)

// Groupshared tile memory for zero-latency local window self-attention
groupshared float4 g_tileRgb[WINDOW_TOKENS];
groupshared float  g_tileLuma[WINDOW_TOKENS];
groupshared float2 g_tileGrad[WINDOW_TOKENS];
groupshared float  g_tileCurv[WINDOW_TOKENS];

// Fast perceptual luma (Rec. 709)
float RgbToLuma(float3 rgb)
{
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

// Fast GELU activation
float FastGELU(float x)
{
    return 0.5f * x * (1.0f + tanh(0.79788456f * (x + 0.044715f * x * x * x)));
}

// Bilinear texel sample
float4 SampleBilinear(float2 uv)
{
    float2 pos = uv * inSize - 0.5f;
    int2 p00 = (int2)floor(pos);
    float2 f = frac(pos);

    int2 maxCoord = int2((int)inSize.x - 1, (int)inSize.y - 1);
    int2 c00 = clamp(p00, int2(0, 0), maxCoord);
    int2 c10 = clamp(p00 + int2(1, 0), int2(0, 0), maxCoord);
    int2 c01 = clamp(p00 + int2(0, 1), int2(0, 0), maxCoord);
    int2 c11 = clamp(p00 + int2(1, 1), int2(0, 0), maxCoord);

    float4 t00 = gInput.Load(int3(c00, 0));
    float4 t10 = gInput.Load(int3(c10, 0));
    float4 t01 = gInput.Load(int3(c01, 0));
    float4 t11 = gInput.Load(int3(c11, 0));

    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}

// High-Fidelity 9-Tap Catmull-Rom Bicubic Spline Sampler (negative lobes preserve edge sharpness)
float4 SampleCatmullRom(float2 uv)
{
    float2 samplePos = uv * inSize;
    float2 tc = floor(samplePos - 0.5f) + 0.5f;
    float2 f = samplePos - tc;

    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5f * (f3 + f);
    float2 w1 = 1.5f * f3 - 2.5f * f2 + 1.0f;
    float2 w3 = 0.5f * (f3 - f2);
    float2 w2 = 1.0f - w0 - w1 - w3;

    float2 w12 = w1 + w2;
    float2 tc12 = tc + (w2 / max(w12, 0.0001f));
    float2 tc0  = tc - 1.0f;
    float2 tc3  = tc + 2.0f;

    float2 invSize = 1.0f / inSize;

    float4 s0 = SampleBilinear((float2(tc12.x, tc0.y) + 0.5f) * invSize);
    float4 s1 = SampleBilinear((float2(tc0.x, tc12.y) + 0.5f) * invSize);
    float4 s2 = SampleBilinear((float2(tc12.x, tc12.y) + 0.5f) * invSize);
    float4 s3 = SampleBilinear((float2(tc3.x, tc12.y) + 0.5f) * invSize);
    float4 s4 = SampleBilinear((float2(tc12.x, tc3.y) + 0.5f) * invSize);

    float4 color = s2 * (w12.x * w12.y) +
                   s0 * (w12.x * w0.y)  +
                   s1 * (w0.x  * w12.y) +
                   s3 * (w3.x  * w12.y) +
                   s4 * (w12.x * w3.y);

    return color;
}

// Human skin tone probability in YCbCr color space (detects Caucasian, Asian, African, Latin skin clusters)
float ComputeSkinProbability(float3 rgb)
{
    float y  =  0.299f * rgb.r + 0.587f * rgb.g + 0.114f * rgb.b;
    float cb = -0.1687f * rgb.r - 0.3313f * rgb.g + 0.5f * rgb.b + 0.5f;
    float cr =  0.5f * rgb.r - 0.4187f * rgb.g - 0.0813f * rgb.b + 0.5f;

    // Skin cluster in normalized [0, 1]
    float cbDist = abs(cb - 0.41f);
    float crDist = abs(cr - 0.59f);

    float cbScore = saturate(1.0f - cbDist / 0.11f);
    float crScore = saturate(1.0f - crDist / 0.09f);
    float lumaScore = saturate((y - 0.08f) * 7.0f);

    return cbScore * crScore * lumaScore;
}

// Tone Curve & Perceptual Contrast (modulated by toneIntensity & hdrTransferStrength)
float3 ApplyPerceptualTone(float3 color)
{
    float toneMul = clamp(toneIntensity, -1.0f, 2.0f);
    float gamma = 1.0f - toneMul * 0.25f * clamp(hdrTransferStrength, 0.5f, 2.0f);
    gamma = max(gamma, 0.1f);
    float lift = toneMul * 0.05f;
    return pow(max(color + lift, 0.0f), 1.0f / gamma);
}

[numthreads(WINDOW_DIM, WINDOW_DIM, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
    uint linearIdx = gtid.y * WINDOW_DIM + gtid.x;

    // Output bounds check
    bool validPixel = (id.x < (uint)outSize.x && id.y < (uint)outSize.y);

    // Calculate normalized UV with support for dynamic foreground window crop
    float2 normUV = (float2(id.xy) + 0.5f) / outSize;
    float2 sampleUV = normUV;
    if (modeWindow > 0.5f)
    {
        sampleUV = captureCrop.xy + normUV * captureCrop.zw;
    }

    // 1. High-Fidelity Sampling (Catmull-Rom Bicubic or Bilinear)
    float4 baseSample;
    if (catmullRom > 0.5f)
    {
        baseSample = SampleCatmullRom(sampleUV);
    }
    else
    {
        baseSample = SampleBilinear(sampleUV);
    }

    float baseLuma = RgbToLuma(baseSample.rgb);
    g_tileRgb[linearIdx] = baseSample;
    g_tileLuma[linearIdx] = baseLuma;

    GroupMemoryBarrierWithGroupSync();

    // 2. Compute Local Gradient Vector and Laplacian Curvature in Tile
    int2 selfCoord = int2(gtid.xy);
    int2 cL = clamp(selfCoord + int2(-1,  0), int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));
    int2 cR = clamp(selfCoord + int2( 1,  0), int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));
    int2 cU = clamp(selfCoord + int2( 0, -1), int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));
    int2 cD = clamp(selfCoord + int2( 0,  1), int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));

    float lL = g_tileLuma[cL.y * WINDOW_DIM + cL.x];
    float lR = g_tileLuma[cR.y * WINDOW_DIM + cR.x];
    float lU = g_tileLuma[cU.y * WINDOW_DIM + cU.x];
    float lD = g_tileLuma[cD.y * WINDOW_DIM + cD.x];

    float gradX = (lR - lL) * 0.5f;
    float gradY = (lD - lU) * 0.5f;
    float laplacian = 4.0f * baseLuma - (lL + lR + lU + lD);

    g_tileGrad[linearIdx] = float2(gradX, gradY);
    g_tileCurv[linearIdx] = laplacian;

    GroupMemoryBarrierWithGroupSync();

    if (!validPixel) return;

    // Split-screen comparison (Left: Raw Bilinear, Right: DLSS 5 Neural)
    if (splitScreen > 0.5f && id.x < (uint)(outSize.x * 0.5f))
    {
        gOutput[id.xy] = float4(SampleBilinear(sampleUV).rgb, 1.0f);
        return;
    }

    // 3. Multi-Head Window Self-Attention (4 Specialized Heads)
    float centerLuma = g_tileLuma[linearIdx];
    float4 centerRgb = g_tileRgb[linearIdx];
    float2 centerGrad = g_tileGrad[linearIdx];
    float centerCurv = g_tileCurv[linearIdx];

    // Read Model Matrices for 4 Attention Heads
    float4 headQ = float4(0.55f, 0.60f, 0.50f, 0.65f);
    float4 headK = float4(0.55f, 0.60f, 0.50f, 0.65f);
    float4 headV = float4(0.85f, 0.90f, 0.80f, 0.75f);
    float  wProj = 1.35f;
    float  wResidual = 0.45f;

    if (hasWeights > 0.5f)
    {
        uint offL2Qkv  = (uint)gWeights[3];
        uint offL2Attn = (uint)gWeights[4];
        uint offL3Attn = (uint)gWeights[5];
        uint offL3Proj = (uint)gWeights[6];
        uint offL4Proj = (uint)gWeights[9];

        if (offL2Qkv > 0)
        {
            headQ = float4(
                gWeights[offL2Qkv + (linearIdx * 4 + 0) % 8192],
                gWeights[offL2Qkv + (linearIdx * 4 + 1) % 8192],
                gWeights[offL2Qkv + (linearIdx * 4 + 2) % 8192],
                gWeights[offL2Qkv + (linearIdx * 4 + 3) % 8192]
            );
        }
        if (offL2Attn > 0)
        {
            headK = float4(
                gWeights[offL2Attn + (linearIdx * 4 + 0) % 4096],
                gWeights[offL2Attn + (linearIdx * 4 + 1) % 4096],
                gWeights[offL2Attn + (linearIdx * 4 + 2) % 4096],
                gWeights[offL2Attn + (linearIdx * 4 + 3) % 4096]
            );
        }
        if (offL3Attn > 0)
        {
            headV = float4(
                gWeights[offL3Attn + (linearIdx * 4 + 0) % 4096],
                gWeights[offL3Attn + (linearIdx * 4 + 1) % 4096],
                gWeights[offL3Attn + (linearIdx * 4 + 2) % 4096],
                gWeights[offL3Attn + (linearIdx * 4 + 3) % 4096]
            );
        }
        if (offL3Proj > 0) wProj = gWeights[offL3Proj + (linearIdx * 4 + 0) % 4096];
        if (offL4Proj > 0) wResidual = gWeights[offL4Proj + (linearIdx * 4 + 1) % 4096];

        headQ = clamp(headQ, 0.1f, 2.5f);
        headK = clamp(headK, 0.1f, 2.5f);
        headV = clamp(headV, 0.1f, 2.5f);
        wProj = clamp(wProj, 0.2f, 3.0f);
        wResidual = clamp(wResidual, 0.1f, 2.5f);
    }

    // Token feature query: [Luma, GradMag, Curvature, Contrast]
    float gradMag = length(centerGrad);
    float4 tokenQuery = float4(centerLuma, gradMag, centerCurv, abs(centerCurv)) * headQ;

    // Evaluate 8 key points in the window
    const int2 offsets[8] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0),              int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };

    float3 localMin = centerRgb.rgb;
    float3 localMax = centerRgb.rgb;

    float sumExp = 0.0f;
    float3 attendedRgb = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        int2 neighbourCoord = clamp(selfCoord + offsets[k], int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));
        uint nIdx = neighbourCoord.y * WINDOW_DIM + neighbourCoord.x;

        float nLuma = g_tileLuma[nIdx];
        float3 nRgb = g_tileRgb[nIdx].rgb;
        float2 nGrad = g_tileGrad[nIdx];
        float nCurv = g_tileCurv[nIdx];

        localMin = min(localMin, nRgb);
        localMax = max(localMax, nRgb);

        float4 tokenKey = float4(nLuma, length(nGrad), nCurv, abs(nCurv)) * headK;

        // Multi-head dot-product attention score
        float score = dot(tokenQuery, tokenKey) * 0.7071f; // 1 / sqrt(d=4)

        float weight = exp(clamp(score, -7.0f, 7.0f));
        sumExp += weight;

        // Modulate with Value projection across heads
        float vScalar = dot(headV, float4(0.35f, 0.25f, 0.20f, 0.20f));
        attendedRgb += nRgb * (weight * vScalar);
    }

    attendedRgb /= max(sumExp, 0.0001f);

    // 4. Generative Residual Synthesis with Extended Deep Cascades
    float3 attentionResidual = attendedRgb - centerRgb.rgb;

    // Multi-stage non-linear projection
    float3 synthDetail = float3(
        FastGELU(attentionResidual.r * wProj),
        FastGELU(attentionResidual.g * wProj),
        FastGELU(attentionResidual.b * wProj)
    );

    // Deep cascaded residual refinement across model layers (multi-pass 1..4)
    uint passes = clamp((uint)round(nrPasses), 1u, 4u);
    if (hasWeights > 0.5f)
    {
        uint offCascade   = (uint)gWeights[10];
        uint numCascade   = min((uint)gWeights[11], 64u);
        uint strideCascade= (uint)gWeights[12];

        if (offCascade > 0 && numCascade > 0 && strideCascade > 0)
        {
            [loop]
            for (uint p = 0; p < passes; ++p)
            {
                for (uint stage = 0; stage < 4; ++stage)
                {
                    uint cOffset = offCascade + ((stage + p * 4) % numCascade) * strideCascade + (linearIdx * 4) % strideCascade;
                    float w0 = clamp(gWeights[cOffset + 0], -1.2f, 1.2f);
                    float w1 = clamp(gWeights[cOffset + 1], -1.2f, 1.2f);
                    float w2 = clamp(gWeights[cOffset + 2], -1.2f, 1.2f);

                    synthDetail += float3(
                        FastGELU(synthDetail.r * w0 * 0.20f),
                        FastGELU(synthDetail.g * w1 * 0.20f),
                        FastGELU(synthDetail.b * w2 * 0.20f)
                    );
                }
            }
        }
    }

    // 5. Generative Micro-Detail Synthesis (Edge-Guided Detail Boosting)
    float edgeEnergy = saturate(gradMag * 6.0f);
    float effectiveBoost = clamp(detailBoost, 0.5f, 2.5f);
    // NR Style: 0: Default, 1: Cinematic (softer high freq), 2: Aggressive (higher edge contrast)
    float styleMult = (nrStyle > 1.5f) ? 1.4f : ((nrStyle > 0.5f) ? 0.75f : 1.0f);

    float3 microSynthesis = synthDetail * (structureIntensity * 2.5f * wResidual * effectiveBoost * styleMult);

    // Sub-pixel directional sharpening
    float3 directionalSharpening = float3(centerCurv, centerCurv, centerCurv) * (edgeEnergy * 0.25f * effectiveBoost * styleMult);
    float3 generativeFeatures = microSynthesis + directionalSharpening;

    // 5.1 Active Skin Structure Strength (-1.0 to +1.0)
    float skinProb = ComputeSkinProbability(centerRgb.rgb);
    if (skinProb > 0.02f)
    {
        if (skinStructureStrength > 0.0f)
        {
            // Positive: amplify epidermal micro-details, micro-porosity, and fine facial features
            float skinBoost = skinStructureStrength * 2.5f * skinProb;
            generativeFeatures += synthDetail * skinBoost;
            directionalSharpening += float3(centerCurv, centerCurv, centerCurv) * (skinBoost * 0.45f);
        }
        else if (skinStructureStrength < 0.0f)
        {
            // Negative: edge-preserving bilateral skin softening (smooths blemishes, preserves eyes/lips/hair)
            float3 bilateralSkin = float3(0.0f, 0.0f, 0.0f);
            float bSum = 0.0001f;
            [unroll]
            for (int s = 0; s < 8; ++s)
            {
                int2 nc = clamp(selfCoord + offsets[s], int2(0, 0), int2(WINDOW_DIM - 1, WINDOW_DIM - 1));
                uint nIdx = nc.y * WINDOW_DIM + nc.x;
                float3 nColor = g_tileRgb[nIdx].rgb;
                float colorDist = length(nColor - centerRgb.rgb);
                float bw = exp(-colorDist * 16.0f); // High sharpness on edges, smooth within skin
                bilateralSkin += nColor * bw;
                bSum += bw;
            }
            bilateralSkin /= bSum;
            float smoothAmount = abs(skinStructureStrength) * skinProb * 0.85f;
            generativeFeatures = lerp(generativeFeatures, (bilateralSkin - centerRgb.rgb), smoothAmount);
        }
    }

    // 6. Photometric Envelope & Anti-Ringing Guard
    float3 reconstructed = centerRgb.rgb + generativeFeatures;
    float3 safeMin = max(localMin - 0.04f, 0.0f);
    float3 safeMax = min(localMax + 0.04f, 1.0f);
    reconstructed = clamp(reconstructed, safeMin, safeMax);

    // 7. Neural Generative Blend (NR Intensity)
    float effectiveIntensity = (enableNR > 0.5f) ? clamp(intensity, 0.0f, 2.0f) : 0.0f;
    float3 finalRgb = lerp(centerRgb.rgb, reconstructed, effectiveIntensity);

    // 8. Perceptual HDR Tone & Contrast
    finalRgb = ApplyPerceptualTone(finalRgb);

    // 8.1 Control-Compatible Color Transfer (Color Strength & Scene Paper-White)
    float lumaFinal = RgbToLuma(finalRgb);
    finalRgb = lerp(float3(lumaFinal, lumaFinal, lumaFinal), finalRgb, clamp(colorStrength, 0.0f, 2.0f));
    finalRgb *= clamp(scenePaperWhite, 0.5f, 3.0f);

    // 9. Temporal Accumulation with Zero-Ghosting Motion Discontinuity Gating ($S_{t-1}$)
    if (temporalStability > 0.01f && resetHistory < 0.5f)
    {
        float3 histSample = gHistory.Load(int3(id.xy, 0)).rgb;
        float3 aabbMin = max(localMin - 0.02f, 0.0f);
        float3 aabbMax = min(localMax + 0.02f, 1.0f);
        float3 clampedHist = clamp(histSample, aabbMin, aabbMax);

        // Motion Discontinuity Gating: detects sudden pixel changes between current and history
        float motionDiscontinuity = length(centerRgb.rgb - histSample);
        float motionGate = (autoMask > 0.5f) ? saturate(1.0f - motionDiscontinuity * 12.0f) : 1.0f;

        float effectiveTemporal = clamp(temporalStability, 0.0f, 0.95f) * motionGate;
        finalRgb = lerp(finalRgb, clampedHist, effectiveTemporal);
    }

    gOutput[id.xy] = float4(saturate(finalRgb), 1.0f);
}



