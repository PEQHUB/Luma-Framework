#ifndef BF2_NATIVE_LUT_MATH
#define BF2_NATIVE_LUT_MATH

// The transfer functions of the game's LUT generation shaders (the
// PQ-output ColorCubeConstants family, FDB2BE2E among them).
float3 BF2LUTEncode(float3 linear100) {
   float3 p=pow(max(linear100,0.0f)*0.01f,0.1593017578125f);
   return pow((p*18.8515625f+0.8359375f)/(p*18.6875f+1.0f),78.84375f);
}
float3 BF2LUTDecode(float3 pq) {
   float3 p=pow(max(pq,0.0f),1.0f/78.84375f);
   return pow(max(p-0.8359375f,0.0f)/max(18.8515625f-18.6875f*p,1e-6f),1.0f/0.1593017578125f)*100.0f;
}
float3 BF2NativeLUTShoulder(float3 color) {
   return color<0.25f ? color : 0.25f+0.75f*(1.0f-exp2((color-0.25f)*-1.923593402f));
}
float3 BF2NativeLUTCompression(float3 linear100) {
   // Full float32 literals recovered from native DXBC, not its rounded
   // six-decimal disassembly (cancellation can otherwise change dark colors).
   float3 xyz=float3(dot(linear100,float3(0.4124563932f,0.357576102f,0.1804375052f)),
      dot(linear100,float3(0.2126729041f,0.715152204f,0.07217500359f)),
      dot(linear100,float3(0.01933390088f,0.1191919968f,0.950304091f)));
   float3 lms=float3(dot(xyz,float3(0.3592f,0.6976f,-0.0358f)),
      dot(xyz,float3(-0.1922f,1.1004f,0.0755f)),dot(xyz,float3(0.007f,0.0749f,0.8434f)));
   lms=BF2LUTEncode(lms);
   float3 ictcp=float3(dot(lms,float3(0.5f,0.5f,0.0f)),
      dot(lms,float3(1.6137f,-3.3234f,1.7097f)),
      dot(lms,float3(4.378f,-4.2455f,-0.1325f)));
   float amount=saturate((ictcp.x-1.0f)*-1.66666666667f);
   amount=pow(amount*amount*(3.0f-2.0f*amount),1.33f);
   float3 compressed=ictcp*float3(1.0f,amount,amount);
   lms=float3(dot(compressed,float3(1.0f,0.008605145849f,0.1110356078f)),
      dot(compressed,float3(1.0f,-0.008605145849f,-0.1110356078f)),
      dot(compressed,float3(1.0f,0.5600488782f,-0.3206374645f)));
   lms=BF2LUTDecode(lms);
   xyz=float3(dot(lms,float3(2.070179939f,-1.326456904f,0.2066160142f)),
      dot(lms,float3(0.3649882376f,0.6804673672f,-0.04542175308f)),
      dot(lms,float3(-0.04959554225f,-0.04942116141f,1.187995911f)));
   compressed=float3(dot(xyz,float3(3.240454912f,-1.53713882f,-0.4985315502f)),
      dot(xyz,float3(-0.9692664146f,1.876010895f,0.04155608267f)),
      dot(xyz,float3(0.05564342067f,-0.2040258497f,1.057225108f)));
   float maximum=max(compressed.r,max(compressed.g,compressed.b));
   compressed=maximum!=0.0f ? compressed*BF2NativeLUTShoulder(maximum).x/maximum : 0.0f;
   return lerp(compressed,BF2NativeLUTShoulder(linear100),min(ictcp.x,1.0f));
}

#endif
