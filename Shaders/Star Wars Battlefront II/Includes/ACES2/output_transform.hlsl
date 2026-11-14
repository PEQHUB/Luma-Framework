// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// ACES 2.0 Output Transform, forward direction, per pixel: the aces-output
// preset main() up to (not including) display encoding,
//    rgb = outputTransform_fwd(aces, ODT_PARAMS);
//    rgb = clamp_f3(rgb, 0., peakLuminance / ref_luminance);
// Result: linear RGB in the limiting primaries, 1.0 = 100 nits.
//
// Ported to HLSL for Luma (STAR WARS Battlefront II); modified from aces-core v2.0
// (https://github.com/aces-aswf/aces-core, commit 069b0bc3e1f6c62820f19fdae2fecec3f4fc0f80):
// lib/Lib.Academy.OutputTransform.ctl, Lib.Academy.Tonescale.ctl,
// Lib.Academy.Utilities.ctl. Modifications: translated from CTL to HLSL (SM5,
// fxc); the parameters and tables of init_ODTParams() come from a constant
// buffer built on the CPU (Battlefront2ACES2::Build() in
// Source/Games/Star Wars Battlefront II/aces2_output.h); the reach-M lookup
// and cos/sin of the hue, which the CTL evaluates twice with the same hue,
// are evaluated once; search loops carry explicit iteration bounds; table
// reads are clamped to the table (only reachable where the CTL would read past
// its end); atan2(0, 0) is defined as 0 as in C. Added ACES2_Rec709ToAP0 and
// ACES2_LimitToRec709.

#ifndef LUMA_ACES2_OUTPUT_TRANSFORM_HLSL
#define LUMA_ACES2_OUTPUT_TRANSFORM_HLSL

#ifndef ACES2_CONSTANTS_REGISTER
#define ACES2_CONSTANTS_REGISTER b10
#endif

#define ACES2_TABLE_SIZE 360
#define ACES2_TOTAL_TABLE_SIZE 362
#define ACES2_BASE_INDEX 1
#define ACES2_REACH_ROWS 91
// Upper bound of both binary searches: the first probe (the uniform-table
// guess) may not narrow the interval, every later probe halves it, and the
// widest interval is 361 entries, so 1 + ceil(log2(361)) = 10 probes suffice.
#define ACES2_MAX_SEARCH_STEPS 16

// Layout: Battlefront2ACES2::Constants. Matrices are three float4 rows,
// out[r] = dot(row[r].xyz, in).
cbuffer ACES2Constants : register(ACES2_CONSTANTS_REGISTER)
{
   // c0
   float ACES2_PeakLuminance; // nits
   float ACES2_ForwardLimit; // AP1 clamp ceiling
   float ACES2_LimitJMax;
   float ACES2_ModelGammaInv;
   // c1: tone scale
   float ACES2_TS_n_r;
   float ACES2_TS_g;
   float ACES2_TS_t_1;
   float ACES2_TS_s_2;
   // c2
   float ACES2_TS_m_2;
   float ACES2_Sat;
   float ACES2_SatThr;
   float ACES2_Compr;
   // c3
   float ACES2_ChromaCompressScale;
   float ACES2_MidJ;
   float ACES2_FocusDist;
   float ACES2_LowerHullGammaInv;
   // c4: input (AP0) JMh parameters
   float ACES2_In_F_L_n;
   float ACES2_In_cz;
   float ACES2_In_inv_cz;
   float ACES2_In_A_w_J;
   // c5
   float ACES2_In_inv_A_w_J;
   float ACES2_Limit_inv_cz;
   int ACES2_HueSearchLo;
   int ACES2_HueSearchHi;
   // c6..c26
   float4 ACES2_AP0ToAP1[3];
   float4 ACES2_AP1ToAP0[3];
   float4 ACES2_InRGBToCAM16c[3];
   float4 ACES2_InConeResponseToAab[3];
   float4 ACES2_LimitAabToConeResponse[3];
   float4 ACES2_LimitCAM16cToRGB[3];
   float4 ACES2_LimitToRec709Rows[3];
   // c27..c388: cusp J, cusp M, hue, upper hull gamma inverse
   float4 ACES2_Table[ACES2_TOTAL_TABLE_SIZE];
   // c389..c479: reach M, entry i at [i / 4][i % 4]
   float4 ACES2_ReachM[ACES2_REACH_ROWS];
};

#define ACES2_MUL(M, v) float3(dot(M[0].xyz, (v)), dot(M[1].xyz, (v)), dot(M[2].xyz, (v)))

static const float ACES2_REF_LUMINANCE = 100.0;
static const float ACES2_HUE_LIMIT = 360.0;
static const float ACES2_J_SCALE = 100.0;
static const float ACES2_CAM_NL_OFFSET = 0.2713 * 100.0;
static const float ACES2_PI = 3.14159265358979323846;
static const float ACES2_SMOOTH_CUSPS = 0.12;
static const float ACES2_CUSP_MID_BLEND = 1.3;
static const float ACES2_FOCUS_GAIN_BLEND = 0.3;
static const float ACES2_COMPRESSION_THRESHOLD = 0.75;

// Linear BT.709 (D65) -> ACES2065-1 (AP0, ACES white), Bradford chromatic
// adaptation D65 -> ACES white, as OpenColorIO's ACES configs convert
// ("Linear Rec.709 (sRGB)" to ACES2065-1, i.e. the inverse of
// UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD applied to BT.709 -> XYZ):
//    M = XYZ_to_AP0 * Bradford(D65 -> (0.32168, 0.33767)) * BT709_to_XYZ
// with RGBtoXYZ_f33 from the primaries, the Bradford cone matrix
// {0.8951, 0.2664, -0.1614; -0.7502, 1.7135, 0.0367; 0.0389, -0.0685, 1.0296},
// evaluated in double and rounded to float. Rows sum to 1 (white -> white).
static const float3x3 ACES2_REC709_TO_AP0 = float3x3(
   0.439632982, 0.382988698, 0.177378320,
   0.0897764430, 0.813439429, 0.0967841283,
   0.0175411704, 0.111546553, 0.870912276);

float3 ACES2_Rec709ToAP0(float3 linearRec709)
{
   return mul(ACES2_REC709_TO_AP0, linearRec709);
}

// Limiting-primaries RGB -> CIE XYZ -> linear BT.709 (both D65, no adaptation).
float3 ACES2_LimitToRec709(float3 rgb)
{
   return ACES2_MUL(ACES2_LimitToRec709Rows, rgb);
}

// ---- Lib.Academy.Utilities.ctl ----
float ACES2_Lerp(float a, float b, float t)
{
   return a + t * (b - a);
}

float ACES2_Sign(float x)
{
   return x < 0.0 ? -1.0 : (x > 0.0 ? 1.0 : 0.0);
}

float3 ACES2_Sign3(float3 x)
{
   return float3(ACES2_Sign(x.x), ACES2_Sign(x.y), ACES2_Sign(x.z));
}

// copysign(x, y) = sign(y) * fabs(x): 0 for y == 0, as in the CTL.
float ACES2_CopySign(float x, float y)
{
   return ACES2_Sign(y) * abs(x);
}

float ACES2_WrapTo360(float hue)
{
   float y = fmod(hue, 360.0);
   if (y < 0.0)
      y = y + 360.0;
   return y;
}

int ACES2_HuePositionInUniformTable(float hue, int table_size)
{
   const float wrapped_hue = ACES2_WrapTo360(hue);
   return int(wrapped_hue / ACES2_HUE_LIMIT * float(table_size));
}

// ---- CAM ----
float3 ACES2_PostAdaptationConeResponseCompressionFwd(float3 v)
{
   const float3 F_L_Y = pow(abs(v), 0.42);
   const float3 Ra = F_L_Y / (ACES2_CAM_NL_OFFSET + F_L_Y);
   return ACES2_Sign3(v) * Ra;
}

float3 ACES2_PostAdaptationConeResponseCompressionInv(float3 v)
{
   const float3 Ra_lim = min(abs(v), 0.99);
   const float3 F_L_Y = (ACES2_CAM_NL_OFFSET * Ra_lim) / (1.0 - Ra_lim);
   const float3 Rc = pow(F_L_Y, 1.0 / 0.42);
   return ACES2_Sign3(v) * Rc;
}

// J_to_Y with the input (AP0) parameters.
float ACES2_JToY(float J)
{
   const float A = pow(abs(J) * (1.0 / ACES2_J_SCALE), ACES2_In_inv_cz);
   const float Ra = ACES2_In_A_w_J * A;
   const float Ra_lim = min(Ra, 0.99);
   const float F_L_Y = (ACES2_CAM_NL_OFFSET * Ra_lim) / (1.0 - Ra_lim);
   return pow(F_L_Y, 1.0 / 0.42) / ACES2_In_F_L_n;
}

// Y_to_J with the input (AP0) parameters.
float ACES2_YToJ(float Y)
{
   const float abs_Y = abs(Y);
   const float F_L_Y = pow(abs_Y * ACES2_In_F_L_n, 0.42);
   const float Ra = F_L_Y / (ACES2_CAM_NL_OFFSET + F_L_Y);
   const float J = ACES2_J_SCALE * pow(Ra * ACES2_In_inv_A_w_J, ACES2_In_cz);
   return ACES2_CopySign(J, Y);
}

// RGB_to_JMh with the input (AP0) parameters.
float3 ACES2_RGBToJMh(float3 rgb)
{
   const float3 rgb_m = ACES2_MUL(ACES2_InRGBToCAM16c, rgb);
   const float3 rgb_a = ACES2_PostAdaptationConeResponseCompressionFwd(rgb_m);
   const float3 Aab = ACES2_MUL(ACES2_InConeResponseToAab, rgb_a);
   if (Aab.x <= 0.0)
      return float3(0.0, 0.0, 0.0);
   const float J = ACES2_J_SCALE * pow(Aab.x, ACES2_In_cz);
   const float M = sqrt(Aab.y * Aab.y + Aab.z * Aab.z);
   const float h_rad = (Aab.y == 0.0 && Aab.z == 0.0) ? 0.0 : atan2(Aab.z, Aab.y);
   const float h = ACES2_WrapTo360(h_rad * 180.0 / ACES2_PI);
   return float3(J, M, h);
}

// JMh_to_RGB with the limiting parameters; cos/sin of the hue precomputed.
float3 ACES2_JMhToRGB(float J, float M, float cos_hr, float sin_hr)
{
   const float A = pow(J * (1.0 / ACES2_J_SCALE), ACES2_Limit_inv_cz);
   const float3 Aab = float3(A, M * cos_hr, M * sin_hr);
   const float3 rgb_a = ACES2_MUL(ACES2_LimitAabToConeResponse, Aab);
   const float3 rgb_m = ACES2_PostAdaptationConeResponseCompressionInv(rgb_a);
   return ACES2_MUL(ACES2_LimitCAM16cToRGB, rgb_m);
}

// ---- Tables ----
int ACES2_TableIndex(int i)
{
   return clamp(i, 0, ACES2_TOTAL_TABLE_SIZE - 1);
}

float ACES2_ReachMEntry(int i)
{
   i = ACES2_TableIndex(i);
   return ACES2_ReachM[i >> 2][i & 3];
}

float ACES2_ReachMFromTable(float h)
{
   const int base = ACES2_HuePositionInUniformTable(h, ACES2_TABLE_SIZE);
   const float t = h - float(base);
   const int i_lo = base + ACES2_BASE_INDEX;
   const int i_hi = i_lo + 1;
   return ACES2_Lerp(ACES2_ReachMEntry(i_lo), ACES2_ReachMEntry(i_hi), t);
}

// cusp_from_table: binary search of the hue column over the whole table.
float2 ACES2_CuspFromTable(float h)
{
   int low_i = 0;
   int high_i = ACES2_BASE_INDEX + ACES2_TABLE_SIZE;
   int i = ACES2_HuePositionInUniformTable(h, ACES2_TABLE_SIZE) + ACES2_BASE_INDEX;
   [loop]
   for (int step = 0; step < ACES2_MAX_SEARCH_STEPS; ++step)
   {
      if (low_i + 1 >= high_i)
         break;
      if (h > ACES2_Table[ACES2_TableIndex(i)].z)
         low_i = i;
      else
         high_i = i;
      i = (low_i + high_i) >> 1; // midpoint(), truncated; indices are non-negative
   }
   const float4 lo = ACES2_Table[high_i - 1];
   const float4 hi = ACES2_Table[high_i];
   const float t = (h - lo.z) / (hi.z - lo.z);
   return float2(ACES2_Lerp(lo.x, hi.x, t), ACES2_Lerp(lo.y, hi.y, t));
}

// lookup_hue_interval: the CTL positions the search with a 362-entry uniform
// table and the precomputed hue_linearity_search_range.
int ACES2_LookupHueInterval(float h)
{
   int i = ACES2_BASE_INDEX + ACES2_HuePositionInUniformTable(h, ACES2_TOTAL_TABLE_SIZE);
   int i_lo = max(ACES2_BASE_INDEX, i + ACES2_HueSearchLo);
   int i_hi = min(ACES2_BASE_INDEX + ACES2_TABLE_SIZE, i + ACES2_HueSearchHi);
   [loop]
   for (int step = 0; step < ACES2_MAX_SEARCH_STEPS; ++step)
   {
      if (i_lo + 1 >= i_hi)
         break;
      if (h > ACES2_Table[ACES2_TableIndex(i)].z)
         i_lo = i;
      else
         i_hi = i;
      i = (i_lo + i_hi) >> 1;
   }
   i_hi = max(1, i_hi);
   return ACES2_TableIndex(i_hi);
}

// ---- Tone scale and chroma compression ----
float ACES2_TonescaleFwd(float x)
{
   const float f = ACES2_TS_m_2 * pow(max(0.0, x) / (x + ACES2_TS_s_2), ACES2_TS_g);
   const float h = max(0.0, f * f / (f + ACES2_TS_t_1));
   return h * ACES2_TS_n_r;
}

float ACES2_ChromaCompressNorm(float a, float b) // a = cos(hr), b = sin(hr)
{
   const float cos_hr2 = a * a - b * b;
   const float sin_hr2 = 2.0 * a * b;
   const float cos_hr3 = 4.0 * a * a * a - 3.0 * a;
   const float sin_hr3 = 3.0 * b - 4.0 * b * b * b;
   const float M = 11.34072 * a + 16.46899 * cos_hr2 + 7.88380 * cos_hr3 + 14.66441 * b + -6.37224 * sin_hr2 + 9.19364 * sin_hr3 + 77.12896;
   return M * ACES2_ChromaCompressScale;
}

float ACES2_Toe(float x, float limit, float k1_in, float k2_in)
{
   if (x > limit)
      return x;
   const float k2 = max(k2_in, 0.001);
   const float k1 = sqrt(k1_in * k1_in + k2 * k2);
   const float k3 = (limit + k1) / (limit + k2);
   const float minus_b = k3 * x - k1;
   const float minus_c = k2 * k3 * x;
   return 0.5 * (minus_b + sqrt(minus_b * minus_b + 4.0 * minus_c));
}

// ---- Gamut compression ----
float ACES2_ComputeCompressionVectorSlope(float intersect_J, float focus_J, float limit_J_max, float slope_gain)
{
   const float direction_scalar = intersect_J < focus_J ? intersect_J : limit_J_max - intersect_J;
   return direction_scalar * (intersect_J - focus_J) / (focus_J * slope_gain);
}

float ACES2_SolveJIntersect(float J, float M, float focusJ, float maxJ, float slope_gain)
{
   const float M_scaled = M / slope_gain;
   const float a = M_scaled / focusJ;
   if (J < focusJ)
   {
      const float b = 1.0 - M_scaled;
      const float c = -J;
      const float det = b * b - 4.0 * a * c;
      const float root = sqrt(det);
      return -2.0 * c / (b + root);
   }
   const float b = -(1.0 + M_scaled + maxJ * a);
   const float c = maxJ * M_scaled + J;
   const float det = b * b - 4.0 * a * c;
   const float root = sqrt(det);
   return -2.0 * c / (b - root);
}

float ACES2_SminScaled(float a, float b, float scale_reference)
{
   const float s_scaled = ACES2_SMOOTH_CUSPS * scale_reference;
   const float h = max(s_scaled - abs(a - b), 0.0) / s_scaled;
   return min(a, b) - h * h * h * s_scaled * (1.0 / 6.0);
}

float ACES2_EstimateLineAndBoundaryIntersectionM(float J_axis_intersect, float slope, float inv_gamma, float J_max, float M_max, float J_intersection_reference)
{
   const float normalised_J = J_axis_intersect / J_intersection_reference;
   const float shifted_intersection = J_intersection_reference * pow(normalised_J, inv_gamma);
   return shifted_intersection * M_max / (J_max - slope * M_max);
}

float ACES2_FindGamutBoundaryIntersection(float2 JM_cusp, float J_max, float gamma_top_inv, float gamma_bottom_inv, float J_intersect_source, float slope, float J_intersect_cusp)
{
   const float M_boundary_lower = ACES2_EstimateLineAndBoundaryIntersectionM(J_intersect_source, slope, gamma_bottom_inv, JM_cusp.x, JM_cusp.y, J_intersect_cusp);
   const float f_J_intersect_cusp = J_max - J_intersect_cusp;
   const float f_J_intersect_source = J_max - J_intersect_source;
   const float f_JM_cusp_J = J_max - JM_cusp.x;
   const float M_boundary_upper = ACES2_EstimateLineAndBoundaryIntersectionM(f_J_intersect_source, -slope, gamma_top_inv, f_JM_cusp_J, JM_cusp.y, f_J_intersect_cusp);
   return ACES2_SminScaled(M_boundary_lower, M_boundary_upper, JM_cusp.y);
}

float ACES2_GetFocusGain(float J, float analytical_threshold, float limit_J_max, float focus_dist)
{
   float gain = limit_J_max * focus_dist;
   if (J > analytical_threshold)
   {
      float gain_adjustment = log10((limit_J_max - analytical_threshold) / max(0.0001, limit_J_max - J));
      gain_adjustment = gain_adjustment * gain_adjustment + 1.0;
      gain = gain * gain_adjustment;
   }
   return gain;
}

float ACES2_RemapM(float M, float gamut_boundary_M, float reach_boundary_M)
{
   const float boundary_ratio = gamut_boundary_M / reach_boundary_M;
   const float proportion = max(boundary_ratio, ACES2_COMPRESSION_THRESHOLD);
   const float threshold = proportion * gamut_boundary_M;
   if (M <= threshold || proportion >= 1.0)
      return M;
   const float m_offset = M - threshold;
   const float gamut_offset = gamut_boundary_M - threshold;
   const float reach_offset = reach_boundary_M - threshold;
   const float scale = reach_offset / ((reach_offset / gamut_offset) - 1.0);
   const float nd = m_offset / scale;
   return threshold + scale * nd / (1.0 + nd); // reinhard_remap, forward
}

// gamut_compress_fwd: returns (J, M); the hue is unchanged.
float2 ACES2_GamutCompressFwd(float J, float M, float h, float reach_max_M)
{
   if (J <= 0.0)
      return float2(0.0, 0.0);
   if (M < 0.0 || J > ACES2_LimitJMax)
      return float2(J, 0.0);

   // init_HueDependentGamutParams
   const int i_hi = ACES2_LookupHueInterval(h);
   const float4 entry_lo = ACES2_Table[i_hi - 1];
   const float4 entry_hi = ACES2_Table[i_hi];
   const float t = h - entry_lo.z; // CTL interpolation_weight(): h - h_lo, not normalised
   const float2 JMcusp = ACES2_CuspFromTable(h);
   const float gamma_bottom_inv = ACES2_LowerHullGammaInv;
   const float gamma_top_inv = ACES2_Lerp(entry_lo.w, entry_hi.w, t);
   const float focus_J = ACES2_Lerp(JMcusp.x, ACES2_MidJ, min(1.0, ACES2_CUSP_MID_BLEND - (JMcusp.x / ACES2_LimitJMax)));
   const float analytical_threshold = ACES2_Lerp(JMcusp.x, ACES2_LimitJMax, ACES2_FOCUS_GAIN_BLEND);

   // compress_gamut(JMh, Jx = J, invert = false)
   const float slope_gain = ACES2_GetFocusGain(J, analytical_threshold, ACES2_LimitJMax, ACES2_FocusDist);
   const float J_intersect_source = ACES2_SolveJIntersect(J, M, focus_J, ACES2_LimitJMax, slope_gain);
   const float gamut_slope = ACES2_ComputeCompressionVectorSlope(J_intersect_source, focus_J, ACES2_LimitJMax, slope_gain);
   const float J_intersect_cusp = ACES2_SolveJIntersect(JMcusp.x, JMcusp.y, focus_J, ACES2_LimitJMax, slope_gain);
   const float gamut_boundary_M = ACES2_FindGamutBoundaryIntersection(JMcusp, ACES2_LimitJMax, gamma_top_inv, gamma_bottom_inv, J_intersect_source, gamut_slope, J_intersect_cusp);
   if (gamut_boundary_M <= 0.0)
      return float2(J, 0.0);
   const float reach_boundary_M = ACES2_EstimateLineAndBoundaryIntersectionM(J_intersect_source, gamut_slope, ACES2_ModelGammaInv, ACES2_LimitJMax, reach_max_M, ACES2_LimitJMax);
   const float remapped_M = ACES2_RemapM(M, gamut_boundary_M, reach_boundary_M);
   return float2(J_intersect_source + remapped_M * gamut_slope, remapped_M);
}

// ACES2065-1 in; clamped linear limiting-primaries RGB out, 1.0 = 100 nits.
float3 ACES2_OutputTransform(float3 aces)
{
   // clamp_AP0_to_AP1(aces, 0, forward_limit); clamp() = max(lo, min(x, hi)) as in the CTL
   float3 ap1 = ACES2_MUL(ACES2_AP0ToAP1, aces);
   ap1 = max(0.0, min(ap1, ACES2_ForwardLimit));
   const float3 ap0 = ACES2_MUL(ACES2_AP1ToAP0, ap1);

   const float3 JMh = ACES2_RGBToJMh(ap0);
   const float J = JMh.x;
   const float M = JMh.y;
   const float h = JMh.z;

   // Every step below keeps this hue: chroma_compress_norm and JMh_to_Aab use
   // its cos/sin, chroma compression and gamut compression its reach M.
   float sin_hr, cos_hr;
   sincos(h / 180.0 * ACES2_PI, sin_hr, cos_hr);
   const float reach_max_M = ACES2_ReachMFromTable(h);

   // tonemap_and_compress_fwd
   const float linear_Y = ACES2_JToY(J) / ACES2_REF_LUMINANCE;
   const float tonemapped_Y = ACES2_TonescaleFwd(linear_Y);
   const float J_ts = ACES2_YToJ(tonemapped_Y);

   // chroma_compress_fwd
   float M_compr = M;
   [branch]
   if (M != 0.0)
   {
      const float nJ = J_ts / ACES2_LimitJMax;
      const float snJ = max(0.0, 1.0 - nJ);
      const float Mnorm = ACES2_ChromaCompressNorm(cos_hr, sin_hr);
      const float limit = pow(nJ, ACES2_ModelGammaInv) * reach_max_M / Mnorm;

      const float toe_limit = limit - 0.001;
      const float toe_snJ_sat = snJ * ACES2_Sat;
      const float toe_sqrt_nJ_sat_thr = sqrt(nJ * nJ + ACES2_SatThr);
      const float toe_nJ_compr = nJ * ACES2_Compr;

      M_compr = M * pow(J_ts / J, ACES2_ModelGammaInv);
      M_compr = M_compr / Mnorm;
      M_compr = limit - ACES2_Toe(limit - M_compr, toe_limit, toe_snJ_sat, toe_sqrt_nJ_sat_thr);
      M_compr = ACES2_Toe(M_compr, limit, toe_nJ_compr, snJ);
      M_compr = M_compr * Mnorm;
   }

   const float2 compressed = ACES2_GamutCompressFwd(J_ts, M_compr, h, reach_max_M);
   const float3 rgb = ACES2_JMhToRGB(compressed.x, compressed.y, cos_hr, sin_hr);

   // Preset main(): clamp_f3(rgb, 0, peakLuminance / ref_luminance)
   return max(0.0, min(rgb, ACES2_PeakLuminance / ACES2_REF_LUMINANCE));
}

#endif // LUMA_ACES2_OUTPUT_TRANSFORM_HLSL
