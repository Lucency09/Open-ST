// 文件职责：在 FP16 效果链执行参考白归一化、曝光和逐像素连续肩部。
#define D2D_INPUT_COUNT 1
#define D2D_INPUT0_SIMPLE
#include "d2d1effecthelpers.hlsli"
cbuffer Parameters : register(b0)
{
    float4 parameters; // x=80/W，y=曝光，z=P=max(2,H/W)，w=未使用。
};
D2D_PS_ENTRY(main)
{
    float3 rgb = D2DGetInput(0).rgb * parameters.x * parameters.y;
    float m = max(rgb.r, max(rgb.g, rgb.b));
    float A = .875, B = .995, K = .6, P = parameters.z;
    float d = (B-A) / log(P), y = A*m;
    if (m > K && m < 1) {
        float t = (m-K)/(1-K);
        y = A*m + (1-K)*(A-d)*t*t*(1-t);
    } else if (m >= 1 && m <= P) y = A+(B-A)*log(m)/log(P);
    else if (m > P) y = B+(1-B)*(1-exp(-(m-P)*d/(P*(1-B))));
    return float4(m > 0 ? rgb*(y/m) : rgb*A, 1);
}
