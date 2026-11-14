// Feature bits of Battlefront2ScenePacker::Recipe (dlss.h).
#define BF2_PACK_DISTORTION ((BF2_PACK_FEATURES & 1) != 0)
#define BF2_PACK_FILM_GRAIN ((BF2_PACK_FEATURES & 2) != 0)
#define BF2_PACK_IRONSIGHTS ((BF2_PACK_FEATURES & 4) != 0)
#define BF2_PACK_CHROMATIC ((BF2_PACK_FEATURES & 8) != 0)
#define BF2_PACK_LENS ((BF2_PACK_FEATURES & 16) != 0)

float2 BF2LensUV(float2 uv, float4 polynomial, float2 axisScale)
{
   precise float2 centered=uv-0.5f;
   precise float2 radiusUV=centered*axisScale;
   precise float radius2=radiusUV.x*radiusUV.x+radiusUV.y*radiusUV.y;
   precise float lensScale;
   if(abs(polynomial.y)<0.0001f) lensScale=mad(radius2,polynomial.x,1.0f)*polynomial.z;
   else lensScale=mad(radius2,mad(polynomial.y,sqrt(radius2),polynomial.x),1.0f)*polynomial.w;
   return mad(centered,lensScale,0.5f);
}
