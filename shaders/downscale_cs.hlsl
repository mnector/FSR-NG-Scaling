Texture2D<float4> InputTex : register(t0);
RWTexture2D<float4> OutputTex : register(u0);

cbuffer Params : register(b0) {
    float2 inSize;
    float2 outSize;
    float4 pad[6];
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID) {
    if (DTid.x >= (uint)outSize.x || DTid.y >= (uint)outSize.y) return;
    
    // Calculate normalized texture coordinates for the output pixel
    float2 uv = (float2(DTid.xy) + 0.5f) / outSize;
    
    // Map to input pixel coordinates
    float2 srcPos = uv * inSize - 0.5f;
    
    // Bilinear interpolation
    int2 p00 = int2(floor(srcPos));
    int2 p11 = min(p00 + 1, int2(inSize) - 1);
    p00 = max(p00, 0);
    
    float2 f = frac(srcPos);
    
    float4 c00 = InputTex.Load(int3(p00.x, p00.y, 0));
    float4 c10 = InputTex.Load(int3(p11.x, p00.y, 0));
    float4 c01 = InputTex.Load(int3(p00.x, p11.y, 0));
    float4 c11 = InputTex.Load(int3(p11.x, p11.y, 0));
    
    float4 top = lerp(c00, c10, f.x);
    float4 bot = lerp(c01, c11, f.x);
    
    OutputTex[DTid.xy] = lerp(top, bot, f.y);
}
