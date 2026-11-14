// -----
// MIT License
//
// Copyright (c) 2025 Polyphony Digital Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
// -----
//
// HLSL port of gt7_tone_mapping.cpp ("Sample implementation of the GT7 Tone
// Mapping operator", version 1.0, 2025-08-10), published with "Driving Toward
// Reality: Physically Based Tone Mapping and Perceptual Fidelity in Gran
// Turismo 7" (SIGGRAPH 2025, Physically Based Shading course):
// https://blog.selfshadow.com/publications/s2025-shading-course/
// Ported for Luma (STAR WARS Battlefront II). The constants, curve, PQ, ICtCp,
// Jzazbz and blend are the sample's own, operation for operation. Changes:
//  - The C++ structs become HLSL structs initialised from uniforms, and the
//    compile-time UCS switch (TONE_MAPPING_UCS) becomes a runtime argument
//    (GT7::UCS_ICTCP is the sample's default).
//  - InitializeAsSDR takes the SDR paper white as an argument; the sample's
//    GRAN_TURISMO_SDR_PAPER_WHITE (250 nits) is the default the caller passes.
//  - The curve and blend constants of initializeParameters() are passed in
//    (Parameters); the struct's comments give the sample's values. The gray
//    point is limited to the linear section's start, which the defaults
//    never reach.
//  - ApplyToneMapping clamps negative input to zero first. The sample takes
//    non-negative frame-buffer values; a negative channel would make its PQ
//    encode return NaN.
// Input level: the sample's frame buffer is exposed for its paper white. Its
// SDR path maps the frame-buffer value 2.5 (250 nits) to SDR white, and the
// curve's toe ends at 0.538. A caller whose scene white is 1.0 must scale its
// signal by paper white / REFERENCE_LUMINANCE before ApplyToneMapping, as
// Luma_SWBF2_PsychoDisplay.hlsl does.
// This port was checked against the original C++ sample.
#ifndef GT7_TONE_MAPPING_HLSL
#define GT7_TONE_MAPPING_HLSL

namespace GT7
{
   static const int UCS_ICTCP = 0;
   static const int UCS_JZAZBZ = 1;

   // Defines the SDR reference white level used in the tone mapping (typically 250 nits).
   static const float GRAN_TURISMO_SDR_PAPER_WHITE = 250.0f; // cd/m^2

   // In Gran Turismo, 1.0f in the linear frame-buffer space corresponds to
   // REFERENCE_LUMINANCE cd/m^2 of physical luminance (typically 100 cd/m^2).
   static const float REFERENCE_LUMINANCE = 100.0f; // cd/m^2 <-> 1.0f

   float FrameBufferValueToPhysicalValue(float fbValue) { return fbValue * REFERENCE_LUMINANCE; }
   float PhysicalValueToFrameBufferValue(float physical) { return physical / REFERENCE_LUMINANCE; }

   float SmoothStep(float x, float edge0, float edge1)
   {
      float t = (x - edge0) / (edge1 - edge0);
      if (x < edge0) return 0.0f;
      if (x > edge1) return 1.0f;
      return t * t * (3.0f - 2.0f * t);
   }

   float ChromaCurve(float x, float a, float b) { return 1.0f - SmoothStep(x, a, b); }

   // "GT Tone Mapping" curve with convergent shoulder.
   struct CurveV2
   {
      float peakIntensity;
      float alpha;
      float midPoint;
      float linearSection;
      float toeStrength;
      float kA, kB, kC;
   };

   CurveV2 InitializeCurve(float monitorIntensity, float alpha, float grayPoint, float linearSection, float toeStrength)
   {
      CurveV2 c;
      c.peakIntensity = monitorIntensity;
      c.alpha = alpha;
      c.midPoint = grayPoint;
      c.linearSection = linearSection;
      c.toeStrength = toeStrength;
      // Pre-compute constants for the shoulder region.
      float k = (c.linearSection - 1.0f) / (c.alpha - 1.0f);
      c.kA = c.peakIntensity * c.linearSection + c.peakIntensity * k;
      c.kB = -c.peakIntensity * k * exp(c.linearSection / k);
      c.kC = -1.0f / (k * c.peakIntensity);
      return c;
   }

   float EvaluateCurve(CurveV2 c, float x)
   {
      if (x < 0.0f) return 0.0f;
      float weightLinear = SmoothStep(x, 0.0f, c.midPoint);
      float weightToe = 1.0f - weightLinear;
      // Shoulder mapping for highlights.
      float shoulder = c.kA + c.kB * exp(x * c.kC);
      if (x < c.linearSection * c.peakIntensity)
      {
         float toeMapped = c.midPoint * pow(x / c.midPoint, c.toeStrength);
         return weightToe * toeMapped + weightLinear * x;
      }
      return shoulder;
   }

   // EOTF / inverse-EOTF for ST-2084 (PQ). exponentScaleFactor scales the
   // exponent in the EOTF for Jzazbz.
   float EotfSt2084(float n, float exponentScaleFactor)
   {
      n = n < 0.0f ? 0.0f : n;
      n = n > 1.0f ? 1.0f : n;
      const float m1 = 0.1593017578125f;
      const float m2 = 78.84375f * exponentScaleFactor;
      const float c1 = 0.8359375f;
      const float c2 = 18.8515625f;
      const float c3 = 18.6875f;
      const float pqC = 10000.0f;
      float np = pow(n, 1.0f / m2);
      float l = np - c1;
      l = l < 0.0f ? 0.0f : l;
      l = l / (c2 - c3 * np);
      l = pow(l, 1.0f / m1);
      // Convert absolute luminance (cd/m^2) into the frame-buffer linear scale.
      return PhysicalValueToFrameBufferValue(l * pqC);
   }

   float InverseEotfSt2084(float v, float exponentScaleFactor)
   {
      const float m1 = 0.1593017578125f;
      const float m2 = 78.84375f * exponentScaleFactor;
      const float c1 = 0.8359375f;
      const float c2 = 18.8515625f;
      const float c3 = 18.6875f;
      const float pqC = 10000.0f;
      float physical = FrameBufferValueToPhysicalValue(v);
      float y = physical / pqC;
      float ym = pow(y, m1);
      return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0f + c3 * ym)));
   }

   // ICtCp conversion (ITU-T T.302). Input/output: linear Rec.2020.
   float3 RgbToICtCp(float3 rgb)
   {
      float l = (rgb.r * 1688.0f + rgb.g * 2146.0f + rgb.b * 262.0f) / 4096.0f;
      float m = (rgb.r * 683.0f + rgb.g * 2951.0f + rgb.b * 462.0f) / 4096.0f;
      float s = (rgb.r * 99.0f + rgb.g * 309.0f + rgb.b * 3688.0f) / 4096.0f;
      float lPQ = InverseEotfSt2084(l, 1.0f);
      float mPQ = InverseEotfSt2084(m, 1.0f);
      float sPQ = InverseEotfSt2084(s, 1.0f);
      return float3((2048.0f * lPQ + 2048.0f * mPQ) / 4096.0f,
                    (6610.0f * lPQ - 13613.0f * mPQ + 7003.0f * sPQ) / 4096.0f,
                    (17933.0f * lPQ - 17390.0f * mPQ - 543.0f * sPQ) / 4096.0f);
   }

   float3 ICtCpToRgb(float3 ictCp)
   {
      float l = ictCp.x + 0.00860904f * ictCp.y + 0.11103f * ictCp.z;
      float m = ictCp.x - 0.00860904f * ictCp.y - 0.11103f * ictCp.z;
      float s = ictCp.x + 0.560031f * ictCp.y - 0.320627f * ictCp.z;
      float lLin = EotfSt2084(l, 1.0f);
      float mLin = EotfSt2084(m, 1.0f);
      float sLin = EotfSt2084(s, 1.0f);
      return float3(max(3.43661f * lLin - 2.50645f * mLin + 0.0698454f * sLin, 0.0f),
                    max(-0.79133f * lLin + 1.9836f * mLin - 0.192271f * sLin, 0.0f),
                    max(-0.0259499f * lLin - 0.0989137f * mLin + 1.12486f * sLin, 0.0f));
   }

   // Jzazbz (Safdar et al. 2017), coefficients adjusted for linear Rec.2020.
   static const float JZAZBZ_EXPONENT_SCALE_FACTOR = 1.7f; // Scale factor for exponent

   float3 RgbToJzazbz(float3 rgb)
   {
      float l = rgb.r * 0.530004f + rgb.g * 0.355704f + rgb.b * 0.086090f;
      float m = rgb.r * 0.289388f + rgb.g * 0.525395f + rgb.b * 0.157481f;
      float s = rgb.r * 0.091098f + rgb.g * 0.147588f + rgb.b * 0.734234f;
      float lPQ = InverseEotfSt2084(l, JZAZBZ_EXPONENT_SCALE_FACTOR);
      float mPQ = InverseEotfSt2084(m, JZAZBZ_EXPONENT_SCALE_FACTOR);
      float sPQ = InverseEotfSt2084(s, JZAZBZ_EXPONENT_SCALE_FACTOR);
      float iz = 0.5f * lPQ + 0.5f * mPQ;
      return float3((0.44f * iz) / (1.0f - 0.56f * iz) - 1.6295499532821566e-11f,
                    3.524000f * lPQ - 4.066708f * mPQ + 0.542708f * sPQ,
                    0.199076f * lPQ + 1.096799f * mPQ - 1.295875f * sPQ);
   }

   float3 JzazbzToRgb(float3 jab)
   {
      float jz = jab.x + 1.6295499532821566e-11f;
      float iz = jz / (0.44f + 0.56f * jz);
      float a = jab.y;
      float b = jab.z;
      float l = iz + a * 1.386050432715393e-1f + b * 5.804731615611869e-2f;
      float m = iz + a * -1.386050432715393e-1f + b * -5.804731615611869e-2f;
      float s = iz + a * -9.601924202631895e-2f + b * -8.118918960560390e-1f;
      float lLin = EotfSt2084(l, JZAZBZ_EXPONENT_SCALE_FACTOR);
      float mLin = EotfSt2084(m, JZAZBZ_EXPONENT_SCALE_FACTOR);
      float sLin = EotfSt2084(s, JZAZBZ_EXPONENT_SCALE_FACTOR);
      return float3(lLin * 2.990669f + mLin * -2.049742f + sLin * 0.088977f,
                    lLin * -1.634525f + mLin * 3.145627f + sLin * -0.483037f,
                    lLin * -0.042505f + mLin * -0.377983f + sLin * 1.448019f);
   }

   // Unified color space (UCS): ICtCp or Jzazbz.
   float3 RgbToUcs(float3 rgb, int ucs) { return ucs == UCS_JZAZBZ ? RgbToJzazbz(rgb) : RgbToICtCp(rgb); }
   float3 UcsToRgb(float3 ucs, int space) { return space == UCS_JZAZBZ ? JzazbzToRgb(ucs) : ICtCpToRgb(ucs); }

   struct ToneMapping
   {
      float sdrCorrectionFactor;
      float framebufferLuminanceTarget;
      float framebufferLuminanceTargetUcs; // Target luminance in UCS space
      CurveV2 curve;
      float blendRatio;
      float fadeStart;
      float fadeEnd;
      int ucs;
   };

   // The curve and blend parameters the sample's initializeParameters() uses
   // ("slightly different parameters from GT Sport").
   struct Parameters
   {
      float alpha;         // 0.25
      float grayPoint;     // 0.538 (frame-buffer units)
      float linearSection; // 0.444 (fraction of the target)
      float toeStrength;   // 1.280
      float blendRatio;    // 0.6
      float fadeStart;     // 0.98
      float fadeEnd;       // 1.16
   };

   // Initializes the tone mapping curve and related parameters based on the
   // target display luminance. Use InitializeAsHDR() or InitializeAsSDR().
   // Luma exposes the sample's parameters.
   // The toe must end inside the linear section for the curve to stay
   // continuous, so the gray point is limited to linearSection * target
   // (no effect for the defaults, whose minimum target is 250 nits).
   ToneMapping InitializeParameters(float physicalTargetLuminance, float sdrCorrectionFactor, int ucs, Parameters p)
   {
      ToneMapping t;
      t.sdrCorrectionFactor = sdrCorrectionFactor;
      t.ucs = ucs;
      t.framebufferLuminanceTarget = PhysicalValueToFrameBufferValue(physicalTargetLuminance);
      // Initialize the curve (slightly different parameters from GT Sport).
      t.curve = InitializeCurve(t.framebufferLuminanceTarget, p.alpha,
         min(p.grayPoint, p.linearSection * t.framebufferLuminanceTarget), p.linearSection, p.toeStrength);
      t.blendRatio = p.blendRatio;
      t.fadeStart = p.fadeStart;
      t.fadeEnd = p.fadeEnd;
      float target = t.framebufferLuminanceTarget;
      float3 ucsValue = RgbToUcs(float3(target, target, target), ucs);
      t.framebufferLuminanceTargetUcs = ucsValue.x; // Use the first UCS component (I or Jz) as luminance
      return t;
   }

   // HDR display. Input: target display peak luminance in nits (range: 250 to
   // 10,000). The lower limit is 250 because the curve parameters were
   // determined based on an SDR paper white assumption of 250 nits.
   ToneMapping InitializeAsHDR(float physicalTargetLuminance, int ucs, Parameters p)
   {
      return InitializeParameters(physicalTargetLuminance, 1.0f, ucs, p);
   }

   // SDR display. GT assumes a maximum SDR output of 1.0 corresponds to the
   // SDR paper white (typically 250 nits), so the curve targets that paper
   // white; sRGB defines 1.0 as 100 nits, so the result is scaled by
   // sdrCorrectionFactor to keep the brightness of HDR and SDR consistent.
   ToneMapping InitializeAsSDR(float sdrPaperWhite, int ucs, Parameters p)
   {
      return InitializeParameters(sdrPaperWhite, 1.0f / PhysicalValueToFrameBufferValue(sdrPaperWhite), ucs, p);
   }

   // Input:  linear Rec.2020 RGB (frame buffer values)
   // Output: tone-mapped RGB (frame buffer values);
   //         - in SDR mode: mapped to [0, 1], ready for sRGB OETF
   //         - in HDR mode: mapped to [0, framebufferLuminanceTarget], ready for PQ inverse-EOTF
   float3 ApplyToneMapping(ToneMapping t, float3 rgb)
   {
      rgb = max(rgb, 0.0f);
      // Convert to UCS to separate luminance and chroma.
      float3 ucs = RgbToUcs(rgb, t.ucs);
      // Per-channel tone mapping ("skewed" color).
      float3 skewedRgb = float3(EvaluateCurve(t.curve, rgb.r), EvaluateCurve(t.curve, rgb.g), EvaluateCurve(t.curve, rgb.b));
      float3 skewedUcs = RgbToUcs(skewedRgb, t.ucs);
      float chromaScale = ChromaCurve(ucs.x / t.framebufferLuminanceTargetUcs, t.fadeStart, t.fadeEnd);
      float3 scaledUcs = float3(skewedUcs.x,         // Luminance from skewed color
                                ucs.y * chromaScale, // Scaled chroma components
                                ucs.z * chromaScale);
      // Convert back to RGB.
      float3 scaledRgb = UcsToRgb(scaledUcs, t.ucs);
      // Final blend between per-channel and UCS-scaled results.
      float3 blended = (1.0f - t.blendRatio) * skewedRgb + t.blendRatio * scaledRgb;
      // When using SDR, apply the correction factor (1.0 for HDR).
      return t.sdrCorrectionFactor * min(blended, t.framebufferLuminanceTarget);
   }
}

#endif
