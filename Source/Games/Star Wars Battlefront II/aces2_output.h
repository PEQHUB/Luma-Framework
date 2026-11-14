// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the ACES Project.
//
// ACES 2.0 Output Transform, forward direction: CPU construction of the
// per-settings parameters and tables (init_ODTParams).
//
// Ported to C++ for Luma (STAR WARS Battlefront II); modified from aces-core v2.0
// (https://github.com/aces-aswf/aces-core, commit 069b0bc3e1f6c62820f19fdae2fecec3f4fc0f80):
// lib/Lib.Academy.OutputTransform.ctl, Lib.Academy.Tonescale.ctl,
// Lib.Academy.Utilities.ctl. Modifications: translated from CTL to C++,
// matrices constructed in double precision and rounded to float, the result
// packed into an HLSL constant buffer image (Constants), and index clamps that
// only take effect where the CTL would read past the end of a table.
// The math, constants, tables and searches are otherwise those of the CTL.
//
// The per-pixel path is Shaders/Star Wars Battlefront II/Includes/ACES2/output_transform.hlsl,
// which reads Constants from register b10.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Battlefront2ACES2
{
   struct Chromaticities
   {
      float red[2];
      float green[2];
      float blue[2];
      float white[2];
   };

   inline constexpr Chromaticities Rec709 = {{0.6400f, 0.3300f}, {0.3000f, 0.6000f}, {0.1500f, 0.0600f}, {0.3127f, 0.3290f}};
   inline constexpr Chromaticities P3D65 = {{0.6800f, 0.3200f}, {0.2650f, 0.6900f}, {0.1500f, 0.0600f}, {0.3127f, 0.3290f}};
   inline constexpr Chromaticities Rec2020 = {{0.7080f, 0.2920f}, {0.1700f, 0.7970f}, {0.1310f, 0.0460f}, {0.3127f, 0.3290f}};
   // ACES white (~D60), the limiting white of the "D60 sim" presets.
   inline constexpr float ACESWhite[2] = {0.32168f, 0.33767f};

   // Luma's user parameters. Every default is the CTL constant it replaces,
   // so a default Tuning reproduces aces-core exactly.
   struct Tuning
   {
      float grey_nits = 10.013f;      // Tonescale c_d: output of 18% grey at 100 nits
      float grey_lift = 0.14f;        // Tonescale w_g: grey increase per stop of peak
      float contrast = 1.15f;         // Tonescale g (surround / contrast)
      float flare = 0.04f;            // Tonescale t_1 (shadow toe / flare)
      float chroma_compression = 1.f; // scales init_ODTParams compr
      float chroma_expansion = 1.f;   // scales init_ODTParams sat
      // Preset scale_white (true in the D60 presets): scale the output so the
      // largest channel of limiting white is 1.0 in the encoding primaries.
      bool scale_white = false;
      Chromaticities encoding = Rec709;
   };
   inline bool SameTuning(const Tuning& a, const Tuning& b)
   {
      return a.grey_nits == b.grey_nits && a.grey_lift == b.grey_lift && a.contrast == b.contrast && a.flare == b.flare && a.chroma_compression == b.chroma_compression && a.chroma_expansion == b.chroma_expansion && a.scale_white == b.scale_white && std::memcmp(&a.encoding, &b.encoding, sizeof(Chromaticities)) == 0;
   }

   // Table geometry (CTL: tableSize, additionalTableEntries, baseIndex).
   inline constexpr int kTableSize = 360;
   inline constexpr int kTotalTableSize = kTableSize + 2;
   inline constexpr int kBaseIndex = 1;
   inline constexpr int kReachRows = (kTotalTableSize + 3) / 4;

   // Byte image of `cbuffer ACES2Constants : register(b10)` in
   // Shaders/Star Wars Battlefront II/Includes/ACES2/output_transform.hlsl (HLSL packing: every row is
   // one float4 register). Matrices are three float4 rows with
   // out[r] = dot(row[r].xyz, in); the .w of each row is unused (zero).
   struct alignas(16) Constants
   {
      // c0
      float peak_luminance; // nits
      float forward_limit;  // TSParams.forward_limit: AP1 clamp ceiling
      float limit_J_max;
      float model_gamma_inv;
      // c1: tone scale (TSParams)
      float ts_n_r;
      float ts_g;
      float ts_t_1;
      float ts_s_2;
      // c2
      float ts_m_2;
      float sat;
      float sat_thr;
      float compr;
      // c3
      float chroma_compress_scale;
      float mid_J;
      float focus_dist;
      float lower_hull_gamma_inv;
      // c4: input (AP0) JMhParams scalars
      float in_F_L_n;
      float in_cz;
      float in_inv_cz;
      float in_A_w_J;
      // c5
      float in_inv_A_w_J;
      float limit_inv_cz;         // limiting JMhParams.inv_cz
      std::int32_t hue_search_lo; // hue_linearity_search_range[0]
      std::int32_t hue_search_hi; // hue_linearity_search_range[1]
      // c6..c26
      float ap0_to_ap1[3][4];
      float ap1_to_ap0[3][4];
      float in_rgb_to_cam16_c[3][4];          // input MATRIX_RGB_to_CAM16_c
      float in_cone_response_to_aab[3][4];    // input MATRIX_cone_response_to_Aab
      float limit_aab_to_cone_response[3][4]; // limiting MATRIX_Aab_to_cone_response
      float limit_cam16_c_to_rgb[3][4];       // limiting MATRIX_CAM16_c_to_RGB
      float limit_to_rec709[3][4];            // limiting RGB -> XYZ -> linear BT.709 (no adaptation)
      // c27..c388: x = cusp J, y = cusp M (scaled by 1 + smooth_m * smooth_cusps as in the CTL),
      // z = hue (TABLE_hues), w = upper hull gamma inverse (TABLE_upper_hull_gamma).
      float table[kTotalTableSize][4];
      // c389..c479: TABLE_reach_M, entry i at reach_m[i / 4][i % 4]; the last two slots are zero.
      float reach_m[kReachRows][4];
   };
   static_assert(sizeof(Constants) == 480 * 16, "ACES2Constants must be 480 float4 registers");
   static_assert(offsetof(Constants, ts_n_r) == 1 * 16);
   static_assert(offsetof(Constants, ts_m_2) == 2 * 16);
   static_assert(offsetof(Constants, chroma_compress_scale) == 3 * 16);
   static_assert(offsetof(Constants, in_F_L_n) == 4 * 16);
   static_assert(offsetof(Constants, in_inv_A_w_J) == 5 * 16);
   static_assert(offsetof(Constants, hue_search_lo) == 5 * 16 + 8);
   static_assert(offsetof(Constants, ap0_to_ap1) == 6 * 16);
   static_assert(offsetof(Constants, ap1_to_ap0) == 9 * 16);
   static_assert(offsetof(Constants, in_rgb_to_cam16_c) == 12 * 16);
   static_assert(offsetof(Constants, in_cone_response_to_aab) == 15 * 16);
   static_assert(offsetof(Constants, limit_aab_to_cone_response) == 18 * 16);
   static_assert(offsetof(Constants, limit_cam16_c_to_rgb) == 21 * 16);
   static_assert(offsetof(Constants, limit_to_rec709) == 24 * 16);
   static_assert(offsetof(Constants, table) == 27 * 16);
   static_assert(offsetof(Constants, reach_m) == (27 + kTotalTableSize) * 16);

   // CTL matrices use the row-vector convention: out = in * M, out[j] = sum_i in[i] * M[i][j].
   struct M33
   {
      float m[3][3];
   };

   struct JMhParams
   {
      M33 MATRIX_RGB_to_CAM16_c;
      M33 MATRIX_CAM16_c_to_RGB;
      M33 MATRIX_cone_response_to_Aab;
      M33 MATRIX_Aab_to_cone_response;
      float F_L_n;
      float cz;
      float inv_cz;
      float A_w_J;
      float inv_A_w_J;
   };

   struct TSParams
   {
      float n;
      float n_r;
      float g;
      float t_1;
      float c_t;
      float s_2;
      float u_2;
      float m_2;
      float forward_limit;
      float inverse_limit;
      float log_peak;
   };

   // init_ODTParams() output, plus the AP0/AP1 matrices of clamp_AP0_to_AP1 and
   // the caller's limiting -> BT.709 matrix.
   struct ODTParams
   {
      float peakLuminance;
      JMhParams input_params;
      JMhParams reach_params;
      JMhParams limit_params;
      TSParams ts;
      float limit_J_max;
      float model_gamma_inv;
      float TABLE_reach_M[kTotalTableSize];
      float sat;
      float sat_thr;
      float compr;
      float chroma_compress_scale;
      float mid_J;
      float focus_dist;
      float lower_hull_gamma_inv;
      float TABLE_hues[kTotalTableSize];
      float TABLE_gamut_cusps[kTotalTableSize][3];
      float TABLE_upper_hull_gamma[kTotalTableSize];
      int hue_linearity_search_range[2];
      M33 AP0_TO_AP1;
      M33 AP1_TO_AP0;
      M33 limit_to_rec709;
   };

   namespace detail
   {
      // ---- Constants (Lib.Academy.OutputTransform.ctl) ----
      inline constexpr Chromaticities AP0 = {{0.73470f, 0.26530f}, {0.00000f, 1.00000f}, {0.00010f, -0.07700f}, {0.32168f, 0.33767f}};
      inline constexpr Chromaticities AP1 = {{0.713f, 0.293f}, {0.165f, 0.830f}, {0.128f, 0.044f}, {0.32168f, 0.33767f}};
      inline constexpr Chromaticities CAM16_PRI = {{0.8336f, 0.1735f}, {2.3854f, -1.4659f}, {0.087f, -0.125f}, {0.333f, 0.333f}};

      inline constexpr float hue_limit = 360.f;
      inline constexpr int cuspCornerCount = 6;
      inline constexpr int totalCornerCount = cuspCornerCount + 2;
      inline constexpr int max_sorted_corners = 2 * cuspCornerCount;
      inline constexpr float reach_cusp_tolerance = 1e-3f;
      inline constexpr float display_cusp_tolerance = 1e-7f;

      inline constexpr float gamma_minimum = 0.0f;
      inline constexpr float gamma_maximum = 5.0f;
      inline constexpr float gamma_search_step = 0.4f;
      inline constexpr float gamma_accuracy = 1e-5f;

      inline constexpr float ref_luminance = 100.f;
      inline constexpr float L_A = 100.f;
      inline constexpr float Y_b = 20.f;
      inline constexpr float surround[3] = {0.9f, 0.59f, 0.9f};

      inline constexpr float J_scale = 100.0f;
      inline constexpr float cam_nl_Y_reference = 100.0f;
      inline constexpr float cam_nl_offset = 0.2713f * cam_nl_Y_reference;
      inline constexpr float cam_nl_scale = 4.0f * cam_nl_Y_reference;

      inline constexpr float chroma_compress = 2.4f;
      inline constexpr float chroma_compress_fact = 3.3f;
      inline constexpr float chroma_expand = 1.3f;
      inline constexpr float chroma_expand_fact = 0.69f;
      inline constexpr float chroma_expand_thr = 0.5f;

      inline constexpr float smooth_cusps = 0.12f;
      inline constexpr float smooth_m = 0.27f;
      inline constexpr float cusp_mid_blend = 1.3f;

      inline constexpr float focus_gain_blend = 0.3f;
      inline constexpr float focus_distance = 1.35f;
      inline constexpr float focus_distance_scaling = 1.75f;

      inline constexpr int test_count = 5;
      inline constexpr float testPositions[test_count] = {0.01f, 0.1f, 0.5f, 0.8f, 0.99f};

      inline constexpr float M_PI_F = 3.14159265358979323846f;

      inline float model_gamma()
      {
         return surround[1] * (1.48f + std::sqrt(Y_b / ref_luminance));
      }

      struct F3
      {
         float v[3];
         float& operator[](int i)
         {
            return v[i];
         }
         float operator[](int i) const
         {
            return v[i];
         }
      };

      // ---- Lib.Academy.Utilities.ctl (float) ----
      // CTL's min/max/clamp: comparisons in this order (matters only for NaN).
      inline float ctl_min(float a, float b)
      {
         return a < b ? a : b;
      }
      inline float ctl_max(float a, float b)
      {
         return a > b ? a : b;
      }
      inline int ctl_sign(float x)
      {
         return x < 0.f ? -1 : (x > 0.f ? 1 : 0);
      }
      inline float ctl_copysign(float x, float y)
      {
         return float(ctl_sign(y)) * std::fabs(x);
      }
      inline float ctl_round(float x)
      {
         const int x1 = x < 0.f ? int(x - 0.5f) : int(x + 0.5f);
         return float(x1);
      }
      inline float lerp(float a, float b, float t)
      {
         return a + t * (b - a);
      }
      inline F3 lerp_f3(const F3& a, const F3& b, float t)
      {
         return {{lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)}};
      }
      inline F3 mult_f_f3(float f, const F3& a)
      {
         return {{f * a[0], f * a[1], f * a[2]}};
      }
      inline float radians_to_degrees(float radians)
      {
         return radians * 180.0f / M_PI_F;
      }
      inline float degrees_to_radians(float degrees)
      {
         return degrees / 180.0f * M_PI_F;
      }
      inline float midpoint(float low, float high)
      {
         return (low + high) / 2.f;
      }

      inline F3 mult_f3_f33(const F3& v, const M33& M)
      {
         F3 r;
         for (int j = 0; j < 3; ++j)
            r[j] = v[0] * M.m[0][j] + v[1] * M.m[1][j] + v[2] * M.m[2][j];
         return r;
      }

      // ---- Matrix construction, double precision, rounded to float once ----
      struct D33
      {
         double m[3][3];
      };

      inline D33 mul(const D33& a, const D33& b)
      {
         D33 r{};
         for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
               r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
         return r;
      }

      inline bool invert(const D33& a, D33& r)
      {
         const auto& m = a.m;
         const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
         const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
         const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
         const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
         if (!(std::fabs(det) > 1e-30) || !std::isfinite(det))
            return false;
         const double inv = 1.0 / det;
         r.m[0][0] = c00 * inv;
         r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
         r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
         r.m[1][0] = c01 * inv;
         r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
         r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
         r.m[2][0] = c02 * inv;
         r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
         r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
         return true;
      }

      inline D33 diagonal(const double d[3])
      {
         D33 r{};
         r.m[0][0] = d[0];
         r.m[1][1] = d[1];
         r.m[2][2] = d[2];
         return r;
      }

      inline M33 to_float(const D33& a)
      {
         M33 r;
         for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
               r.m[i][j] = float(a.m[i][j]);
         return r;
      }

      // RGBtoXYZ_f33(C, Y) from Lib.Academy.Utilities.ctl (row-vector convention).
      inline D33 RGBtoXYZ(const Chromaticities& C, double Y)
      {
         const double rx = C.red[0], ry = C.red[1], gx = C.green[0], gy = C.green[1];
         const double bx = C.blue[0], by = C.blue[1], wx = C.white[0], wy = C.white[1];
         const double X = wx * Y / wy;
         const double Z = (1. - wx - wy) * Y / wy;
         const double d = rx * (by - gy) + bx * (gy - ry) + gx * (ry - by);
         const double Sr = (X * (by - gy) - gx * (Y * (by - 1) + by * (X + Z)) + bx * (Y * (gy - 1) + gy * (X + Z))) / d;
         const double Sg = (X * (ry - by) + rx * (Y * (by - 1) + by * (X + Z)) - bx * (Y * (ry - 1) + ry * (X + Z))) / d;
         const double Sb = (X * (gy - ry) - rx * (Y * (gy - 1) + gy * (X + Z)) + gx * (Y * (ry - 1) + ry * (X + Z))) / d;
         D33 M;
         M.m[0][0] = Sr * rx;
         M.m[0][1] = Sr * ry;
         M.m[0][2] = Sr * (1. - rx - ry);
         M.m[1][0] = Sg * gx;
         M.m[1][1] = Sg * gy;
         M.m[1][2] = Sg * (1. - gx - gy);
         M.m[2][0] = Sb * bx;
         M.m[2][1] = Sb * by;
         M.m[2][2] = Sb * (1. - bx - by);
         return M;
      }

      inline bool XYZtoRGB(const Chromaticities& C, double Y, D33& out)
      {
         return invert(RGBtoXYZ(C, Y), out);
      }

      inline bool finite_matrix(const D33& a)
      {
         for (const auto& row : a.m)
            for (double v : row)
               if (!std::isfinite(v))
                  return false;
         return true;
      }

      // ---- CAM functions (float) ----
      inline float _post_adaptation_cone_response_compression_fwd(float Rc)
      {
         const float F_L_Y = std::pow(Rc, 0.42f);
         const float Ra = (F_L_Y) / (cam_nl_offset + F_L_Y);
         return Ra;
      }

      inline float _post_adaptation_cone_response_compression_inv(float Ra)
      {
         const float Ra_lim = ctl_min(Ra, 0.99f);
         const float F_L_Y = (cam_nl_offset * Ra_lim) / (1.f - Ra_lim);
         const float Rc = std::pow(F_L_Y, 1.f / 0.42f);
         return Rc;
      }

      inline float post_adaptation_cone_response_compression_fwd(float v)
      {
         const float abs_v = std::fabs(v);
         const float Ra = _post_adaptation_cone_response_compression_fwd(abs_v);
         return ctl_copysign(Ra, v);
      }

      inline float post_adaptation_cone_response_compression_inv(float v)
      {
         const float abs_v = std::fabs(v);
         const float Rc = _post_adaptation_cone_response_compression_inv(abs_v);
         return ctl_copysign(Rc, v);
      }

      inline float Achromatic_n_to_J(float A, float cz)
      {
         return J_scale * std::pow(A, cz);
      }
      inline float J_to_Achromatic_n(float J, float inv_cz)
      {
         return std::pow(J * (1.f / J_scale), inv_cz);
      }

      inline float Y_to_J(float Y, const JMhParams& p)
      {
         const float abs_Y = std::fabs(Y);
         const float Ra = _post_adaptation_cone_response_compression_fwd(abs_Y * p.F_L_n);
         const float J = Achromatic_n_to_J(Ra * p.inv_A_w_J, p.cz);
         return ctl_copysign(J, Y);
      }

      inline F3 RGB_to_Aab(const F3& RGB, const JMhParams& p)
      {
         const F3 rgb_m = mult_f3_f33(RGB, p.MATRIX_RGB_to_CAM16_c);
         const F3 rgb_a = {{post_adaptation_cone_response_compression_fwd(rgb_m[0]),
            post_adaptation_cone_response_compression_fwd(rgb_m[1]),
            post_adaptation_cone_response_compression_fwd(rgb_m[2])}};
         return mult_f3_f33(rgb_a, p.MATRIX_cone_response_to_Aab);
      }

      inline float wrap_to_360(float hue)
      {
         float y = std::fmod(hue, 360.f);
         if (y < 0.f)
            y = y + 360.f;
         return y;
      }

      inline int hue_position_in_uniform_table(float hue, int table_size)
      {
         const float wrapped_hue = wrap_to_360(hue);
         return int(wrapped_hue / hue_limit * float(table_size));
      }

      inline float base_hue_for_position(int i_lo, int table_size)
      {
         return float(i_lo) * hue_limit / float(table_size);
      }

      inline F3 Aab_to_JMh(const F3& Aab, const JMhParams& p)
      {
         F3 JMh = {{0.f, 0.f, 0.f}};
         if (Aab[0] <= 0.f)
            return JMh;
         const float J = Achromatic_n_to_J(Aab[0], p.cz);
         const float M = std::sqrt(Aab[1] * Aab[1] + Aab[2] * Aab[2]);
         const float h_rad = std::atan2(Aab[2], Aab[1]);
         const float h = wrap_to_360(radians_to_degrees(h_rad));
         JMh[0] = J;
         JMh[1] = M;
         JMh[2] = h;
         return JMh;
      }

      inline F3 RGB_to_JMh(const F3& RGB, const JMhParams& p)
      {
         return Aab_to_JMh(RGB_to_Aab(RGB, p), p);
      }

      inline F3 JMh_to_Aab(const F3& JMh, const JMhParams& p)
      {
         const float h_rad = degrees_to_radians(JMh[2]);
         const float cos_hr = std::cos(h_rad);
         const float sin_hr = std::sin(h_rad);
         const float A = J_to_Achromatic_n(JMh[0], p.inv_cz);
         return {{A, JMh[1] * cos_hr, JMh[1] * sin_hr}};
      }

      inline F3 Aab_to_RGB(const F3& Aab, const JMhParams& p)
      {
         const F3 rgb_a = mult_f3_f33(Aab, p.MATRIX_Aab_to_cone_response);
         const F3 rgb_m = {{post_adaptation_cone_response_compression_inv(rgb_a[0]),
            post_adaptation_cone_response_compression_inv(rgb_a[1]),
            post_adaptation_cone_response_compression_inv(rgb_a[2])}};
         return mult_f3_f33(rgb_m, p.MATRIX_CAM16_c_to_RGB);
      }

      inline F3 JMh_to_RGB(const F3& JMh, const JMhParams& p)
      {
         return Aab_to_RGB(JMh_to_Aab(JMh, p), p);
      }

      inline float compute_compression_vector_slope(float intersect_J, float focus_J, float limit_J_max, float slope_gain)
      {
         const float direction_scalar = intersect_J < focus_J ? intersect_J : limit_J_max - intersect_J;
         return direction_scalar * (intersect_J - focus_J) / (focus_J * slope_gain);
      }

      inline float solve_J_intersect(float J, float M, float focusJ, float maxJ, float slope_gain)
      {
         const float M_scaled = M / slope_gain;
         const float a = M_scaled / focusJ;
         if (J < focusJ)
         {
            const float b = 1.f - M_scaled;
            const float c = -J;
            const float det = b * b - 4.f * a * c;
            const float root = std::sqrt(det);
            return -2.f * c / (b + root);
         }
         const float b = -(1.f + M_scaled + maxJ * a);
         const float c = maxJ * M_scaled + J;
         const float det = b * b - 4.f * a * c;
         const float root = std::sqrt(det);
         return -2.f * c / (b - root);
      }

      inline float smin_scaled(float a, float b, float scale_reference)
      {
         const float s_scaled = smooth_cusps * scale_reference;
         const float h = ctl_max(s_scaled - std::fabs(a - b), 0.0f) / s_scaled;
         return ctl_min(a, b) - h * h * h * s_scaled * (1.f / 6.f);
      }

      inline float estimate_line_and_boundary_intersection_M(float J_axis_intersect, float slope, float inv_gamma, float J_max, float M_max, float J_intersection_reference)
      {
         const float normalised_J = J_axis_intersect / J_intersection_reference;
         const float shifted_intersection = J_intersection_reference * std::pow(normalised_J, inv_gamma);
         return shifted_intersection * M_max / (J_max - slope * M_max);
      }

      inline float find_gamut_boundary_intersection(const float JM_cusp[2], float J_max, float gamma_top_inv, float gamma_bottom_inv, float J_intersect_source, float slope, float J_intersect_cusp)
      {
         const float M_boundary_lower = estimate_line_and_boundary_intersection_M(J_intersect_source, slope, gamma_bottom_inv, JM_cusp[0], JM_cusp[1], J_intersect_cusp);
         const float f_J_intersect_cusp = J_max - J_intersect_cusp;
         const float f_J_intersect_source = J_max - J_intersect_source;
         const float f_JM_cusp_J = J_max - JM_cusp[0];
         const float M_boundary_upper = estimate_line_and_boundary_intersection_M(f_J_intersect_source, -slope, gamma_top_inv, f_JM_cusp_J, JM_cusp[1], f_J_intersect_cusp);
         return smin_scaled(M_boundary_lower, M_boundary_upper, JM_cusp[1]);
      }

      inline float get_focus_gain(float J, float analytical_threshold, float limit_J_max, float focus_dist)
      {
         float gain = limit_J_max * focus_dist;
         if (J > analytical_threshold)
         {
            float gain_adjustment = std::log10((limit_J_max - analytical_threshold) / ctl_max(0.0001f, limit_J_max - J));
            gain_adjustment = gain_adjustment * gain_adjustment + 1.f;
            gain = gain * gain_adjustment;
         }
         return gain;
      }

      inline float compute_focus_J(float cusp_J, float mid_J, float limit_J_max)
      {
         return lerp(cusp_J, mid_J, ctl_min(1.f, cusp_mid_blend - (cusp_J / limit_J_max)));
      }

      // ---- Table building (init_ODTParams and its helpers) ----
      inline bool any_below_zero(const F3& rgb)
      {
         return rgb[0] < 0.f || rgb[1] < 0.f || rgb[2] < 0.f;
      }

      inline bool init_JMhParams(const Chromaticities& prims, JMhParams& p)
      {
         D33 MATRIX_16;
         if (!XYZtoRGB(CAM16_PRI, 1.0, MATRIX_16))
            return false;
         const D33 base_cone_response_to_Aab = {{{2., 1., 1. / 9.}, {1., -12. / 11., 1. / 9.}, {1. / 20., 1. / 11., -2. / 9.}}};

         const D33 RGB_TO_XYZ = RGBtoXYZ(prims, 1.0);
         double XYZ_w[3];
         for (int j = 0; j < 3; ++j)
            XYZ_w[j] = double(ref_luminance) * (RGB_TO_XYZ.m[0][j] + RGB_TO_XYZ.m[1][j] + RGB_TO_XYZ.m[2][j]);
         const double Y_w = XYZ_w[1];
         double RGB_w[3];
         for (int j = 0; j < 3; ++j)
            RGB_w[j] = XYZ_w[0] * MATRIX_16.m[0][j] + XYZ_w[1] * MATRIX_16.m[1][j] + XYZ_w[2] * MATRIX_16.m[2][j];

         const double LA = L_A;
         const double k = 1. / (5. * LA + 1.);
         const double k4 = k * k * k * k;
         const double F_L = 0.2 * k4 * (5. * LA) + 0.1 * std::pow((1. - k4), 2.) * std::pow(5. * LA, 1. / 3.);
         const double F_L_n = F_L / double(ref_luminance);
         const double cz = double(model_gamma());

         double D_RGB[3], RGB_Aw[3];
         for (int j = 0; j < 3; ++j)
         {
            D_RGB[j] = F_L_n * Y_w / RGB_w[j];
            const double RGB_wc = D_RGB[j] * RGB_w[j];
            const double F_L_Y = std::pow(std::fabs(RGB_wc), 0.42);
            RGB_Aw[j] = (RGB_wc < 0. ? -1. : (RGB_wc > 0. ? 1. : 0.)) * (F_L_Y / (double(cam_nl_offset) + F_L_Y));
         }

         D33 cone_response_to_Aab;
         for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
               cone_response_to_Aab.m[i][j] = double(cam_nl_scale) * base_cone_response_to_Aab.m[i][j];
         const double A_w = cone_response_to_Aab.m[0][0] * RGB_Aw[0] + cone_response_to_Aab.m[1][0] * RGB_Aw[1] + cone_response_to_Aab.m[2][0] * RGB_Aw[2];
         const double F_L_Y_w = std::pow(F_L, 0.42);
         const double A_w_J = F_L_Y_w / (double(cam_nl_offset) + F_L_Y_w);

         D33 scale100{};
         scale100.m[0][0] = scale100.m[1][1] = scale100.m[2][2] = double(ref_luminance);
         const D33 MATRIX_RGB_to_CAM16 = mul(mul(RGB_TO_XYZ, MATRIX_16), scale100);
         const D33 MATRIX_RGB_to_CAM16_c = mul(MATRIX_RGB_to_CAM16, diagonal(D_RGB));

         D33 MATRIX_cone_response_to_Aab;
         for (int i = 0; i < 3; ++i)
         {
            MATRIX_cone_response_to_Aab.m[i][0] = cone_response_to_Aab.m[i][0] / A_w;
            MATRIX_cone_response_to_Aab.m[i][1] = cone_response_to_Aab.m[i][1] * 43. * double(surround[2]);
            MATRIX_cone_response_to_Aab.m[i][2] = cone_response_to_Aab.m[i][2] * 43. * double(surround[2]);
         }

         D33 MATRIX_CAM16_c_to_RGB, MATRIX_Aab_to_cone_response;
         if (!invert(MATRIX_RGB_to_CAM16_c, MATRIX_CAM16_c_to_RGB) || !invert(MATRIX_cone_response_to_Aab, MATRIX_Aab_to_cone_response))
            return false;
         if (!finite_matrix(MATRIX_RGB_to_CAM16_c) || !finite_matrix(MATRIX_CAM16_c_to_RGB) || !finite_matrix(MATRIX_cone_response_to_Aab) || !finite_matrix(MATRIX_Aab_to_cone_response))
            return false;

         p.MATRIX_RGB_to_CAM16_c = to_float(MATRIX_RGB_to_CAM16_c);
         p.MATRIX_CAM16_c_to_RGB = to_float(MATRIX_CAM16_c_to_RGB);
         p.MATRIX_cone_response_to_Aab = to_float(MATRIX_cone_response_to_Aab);
         p.MATRIX_Aab_to_cone_response = to_float(MATRIX_Aab_to_cone_response);
         p.F_L_n = float(F_L_n);
         p.cz = float(cz);
         p.inv_cz = float(1. / cz);
         p.A_w_J = float(A_w_J);
         p.inv_A_w_J = float(1. / A_w_J);
         return std::isfinite(p.F_L_n) && std::isfinite(p.A_w_J) && p.F_L_n > 0.f && p.A_w_J > 0.f;
      }

      // init_TSParams (Lib.Academy.Tonescale.ctl), evaluated in double.
      inline TSParams init_TSParams(float peakLuminance, const Tuning& tuning = {})
      {
         const double n = peakLuminance;
         const double n_r = 100.0;
         const double g = tuning.contrast; // 1.15
         const double c = 0.18;
         const double c_d = tuning.grey_nits; // 10.013
         const double w_g = tuning.grey_lift; // 0.14
         const double t_1 = tuning.flare;     // 0.04
         const double r_hit_min = 128.;
         const double r_hit_max = 896.;

         const double r_hit = r_hit_min + (r_hit_max - r_hit_min) * (std::log(n / n_r) / std::log(10000. / 100.));
         const double m_0 = (n / n_r);
         const double m_1 = 0.5 * (m_0 + std::sqrt(m_0 * (m_0 + 4. * t_1)));
         const double u = std::pow((r_hit / m_1) / ((r_hit / m_1) + 1), g);
         const double m = m_1 / u;
         const double w_i = std::log(n / 100.) / std::log(2.);
         const double c_t = c_d / n_r * (1. + w_i * w_g);
         const double g_ip = 0.5 * (c_t + std::sqrt(c_t * (c_t + 4. * t_1)));
         const double g_ipp2 = -(m_1 * std::pow((g_ip / m), (1. / g))) / (std::pow(g_ip / m, 1. / g) - 1.);
         const double w_2 = c / g_ipp2;
         const double s_2 = w_2 * m_1;
         const double u_2 = std::pow((r_hit / m_1) / ((r_hit / m_1) + w_2), g);
         const double m_2 = m_1 / u_2;

         TSParams p;
         p.n = float(n);
         p.n_r = float(n_r);
         p.g = float(g);
         p.t_1 = float(t_1);
         p.c_t = float(c_t);
         p.s_2 = float(s_2);
         p.u_2 = float(u_2);
         p.m_2 = float(m_2);
         p.forward_limit = float(8.0 * r_hit);
         p.inverse_limit = float(n / (u_2 * n_r));
         p.log_peak = float(std::log10(n / n_r));
         return p;
      }

      inline F3 generate_unit_cube_cusp_corners(int corner)
      {
         F3 result;
         result[0] = ((corner + 1) % cuspCornerCount) < 3 ? 1.f : 0.f;
         result[1] = ((corner + 5) % cuspCornerCount) < 3 ? 1.f : 0.f;
         result[2] = ((corner + 3) % cuspCornerCount) < 3 ? 1.f : 0.f;
         return result;
      }

      inline void build_limiting_cusp_corners_tables(F3 RGB_corners[totalCornerCount], F3 JMh_corners[totalCornerCount], const JMhParams& params, float peakLuminance)
      {
         F3 temp_RGB_corners[cuspCornerCount];
         F3 temp_JMh_corners[cuspCornerCount];
         int min_index = 0;
         for (int i = 0; i != cuspCornerCount; ++i)
         {
            temp_RGB_corners[i] = mult_f_f3(peakLuminance / ref_luminance, generate_unit_cube_cusp_corners(i));
            temp_JMh_corners[i] = RGB_to_JMh(temp_RGB_corners[i], params);
            if (temp_JMh_corners[i][2] < temp_JMh_corners[min_index][2])
               min_index = i;
         }
         for (int i = 0; i != cuspCornerCount; ++i)
         {
            RGB_corners[i + 1] = temp_RGB_corners[(i + min_index) % cuspCornerCount];
            JMh_corners[i + 1] = temp_JMh_corners[(i + min_index) % cuspCornerCount];
         }
         RGB_corners[0] = RGB_corners[cuspCornerCount];
         RGB_corners[cuspCornerCount + 1] = RGB_corners[1];
         JMh_corners[0] = JMh_corners[cuspCornerCount];
         JMh_corners[cuspCornerCount + 1] = JMh_corners[1];
         JMh_corners[0][2] = JMh_corners[0][2] - hue_limit;
         JMh_corners[cuspCornerCount + 1][2] = JMh_corners[cuspCornerCount + 1][2] + hue_limit;
      }

      inline void find_reach_corners_table(F3 JMh_corners[totalCornerCount], const JMhParams& params_reach, const ODTParams& p)
      {
         F3 temp_JMh_corners[cuspCornerCount];
         const float limitA = J_to_Achromatic_n(p.limit_J_max, params_reach.inv_cz);
         int min_index = 0;
         for (int i = 0; i != cuspCornerCount; ++i)
         {
            const F3 rgb_vector = generate_unit_cube_cusp_corners(i);
            float lower = 0.0f;
            float upper = p.ts.forward_limit;
            while ((upper - lower) > reach_cusp_tolerance)
            {
               const float test = (lower + upper) / 2.f;
               const F3 test_corner = mult_f_f3(test, rgb_vector);
               const float A = RGB_to_Aab(test_corner, params_reach)[0];
               if (A < limitA)
                  lower = test;
               else
                  upper = test;
            }
            temp_JMh_corners[i] = RGB_to_JMh(mult_f_f3(upper, rgb_vector), params_reach);
            if (temp_JMh_corners[i][2] < temp_JMh_corners[min_index][2])
               min_index = i;
         }
         for (int i = 0; i != cuspCornerCount; ++i)
            JMh_corners[i + 1] = temp_JMh_corners[(i + min_index) % cuspCornerCount];
         JMh_corners[0] = JMh_corners[cuspCornerCount];
         JMh_corners[cuspCornerCount + 1] = JMh_corners[1];
         JMh_corners[0][2] = JMh_corners[0][2] - hue_limit;
         JMh_corners[cuspCornerCount + 1][2] = JMh_corners[cuspCornerCount + 1][2] + hue_limit;
      }

      inline void extract_sorted_cube_hues(const F3 reach_JMh[totalCornerCount], const F3 limit_JMh[totalCornerCount], float sorted_hues[max_sorted_corners])
      {
         for (int k = 0; k < max_sorted_corners; ++k)
            sorted_hues[k] = 0.f;
         int idx = 0;
         int reach_idx = 1;
         int limit_idx = 1;
         while ((reach_idx < (cuspCornerCount + 1)) || (limit_idx < (cuspCornerCount + 1)))
         {
            const float reach_hue = reach_JMh[reach_idx][2];
            const float limit_hue = limit_JMh[limit_idx][2];
            if (reach_hue == limit_hue)
            {
               sorted_hues[idx] = reach_hue;
               reach_idx = reach_idx + 1;
               limit_idx = limit_idx + 1;
            }
            else if (reach_hue < limit_hue)
            {
               sorted_hues[idx] = reach_hue;
               reach_idx = reach_idx + 1;
            }
            else
            {
               sorted_hues[idx] = limit_hue;
               limit_idx = limit_idx + 1;
            }
            idx = idx + 1;
         }
      }

      // Returns false where the CTL would loop forever or write past the table.
      inline bool build_hue_sample_interval(int samples, float lower, float upper, float hue_table[kTotalTableSize], int base)
      {
         if (samples < 0 || base < 0 || base + samples > kTotalTableSize)
            return false;
         const float delta = (upper - lower) / float(samples);
         for (int i = 0; i != samples; ++i)
            hue_table[base + i] = lower + float(i) * delta;
         return true;
      }

      inline bool build_hue_table(const float sorted_hues[max_sorted_corners], float hue_table[kTotalTableSize])
      {
         for (int k = 0; k < kTotalTableSize; ++k)
            hue_table[k] = 0.f;
         const float ideal_spacing = float(kTableSize) / hue_limit;
         int samples_count[2 * cuspCornerCount + 2] = {};
         int last_idx = 0; // uninitialised in the CTL; only matters if the first hue is exactly 0.0
         int min_index = sorted_hues[0] == 0.0f ? 0 : 1;
         for (int hue_idx = 0; hue_idx != max_sorted_corners; ++hue_idx)
         {
            int nominal_idx = int(ctl_min(ctl_max(ctl_round(sorted_hues[hue_idx] * ideal_spacing), float(min_index)), float(kTableSize - 1)));
            if (last_idx == nominal_idx)
            {
               if (hue_idx > 1 && samples_count[hue_idx - 2] != (samples_count[hue_idx - 1] - 1))
                  samples_count[hue_idx - 1] = samples_count[hue_idx - 1] - 1;
               else
                  nominal_idx = nominal_idx + 1;
            }
            samples_count[hue_idx] = int(ctl_min(float(nominal_idx), float(kTableSize - 1)));
            min_index = nominal_idx;
            last_idx = min_index;
         }

         int total_samples = 0;
         int i = 0;
         if (!build_hue_sample_interval(samples_count[i], 0.0f, sorted_hues[i], hue_table, total_samples + 1))
            return false;
         total_samples = total_samples + samples_count[i];
         for (i = i + 1; i != max_sorted_corners; ++i)
         {
            const int samples = samples_count[i] - samples_count[i - 1];
            if (!build_hue_sample_interval(samples, sorted_hues[i - 1], sorted_hues[i], hue_table, total_samples + 1))
               return false;
            total_samples = total_samples + samples;
         }
         if (!build_hue_sample_interval(kTableSize - total_samples, sorted_hues[i - 1], hue_limit, hue_table, total_samples + 1))
            return false;

         hue_table[0] = hue_table[kBaseIndex + kTableSize - 1] - hue_limit;
         hue_table[kBaseIndex + kTableSize] = hue_table[kBaseIndex] + hue_limit;
         return true;
      }

      inline void find_display_cusp_for_hue(float hue, const F3 RGB_corners[totalCornerCount], const F3 JMh_corners[totalCornerCount], const JMhParams& params, float return_JM[2])
      {
         int upper_corner = 1;
         bool found = false;
         for (int i = upper_corner; i != totalCornerCount && !found; ++i)
         {
            if (JMh_corners[i][2] > hue)
            {
               upper_corner = i;
               found = true;
            }
         }
         const int lower_corner = upper_corner - 1;

         if (JMh_corners[lower_corner][2] == hue)
         {
            return_JM[0] = JMh_corners[lower_corner][0];
            return_JM[1] = JMh_corners[lower_corner][1];
            return;
         }

         const F3 cusp_lower = RGB_corners[lower_corner];
         const F3 cusp_upper = RGB_corners[upper_corner];
         F3 sample;
         float sample_t;
         // The CTL's `previous` warm start is never set (its updates are
         // commented out), so the search always starts at 0.
         float lower_t = 0.0f;
         float upper_t = 1.0f;
         F3 JMh;
         // Termination: t lies in [0, 1], where adjacent floats are < 1e-7
         // apart, so the interval always shrinks below the tolerance.
         while ((upper_t - lower_t) > display_cusp_tolerance)
         {
            sample_t = midpoint(lower_t, upper_t);
            sample = lerp_f3(cusp_lower, cusp_upper, sample_t);
            JMh = RGB_to_JMh(sample, params);
            if (JMh[2] < JMh_corners[lower_corner][2])
               upper_t = sample_t;
            else if (JMh[2] >= JMh_corners[upper_corner][2])
               lower_t = sample_t;
            else if (JMh[2] > hue)
               upper_t = sample_t;
            else
               lower_t = sample_t;
         }
         sample_t = midpoint(lower_t, upper_t);
         sample = lerp_f3(cusp_lower, cusp_upper, sample_t);
         JMh = RGB_to_JMh(sample, params);
         return_JM[0] = JMh[0];
         return_JM[1] = JMh[1];
      }

      inline void build_cusp_table(const float hue_table[kTotalTableSize], const F3 RGB_corners[totalCornerCount], const F3 JMh_corners[totalCornerCount], const JMhParams& params, float output_table[kTotalTableSize][3])
      {
         for (int i = kBaseIndex; i != kTotalTableSize; ++i)
         {
            const float hue = hue_table[i];
            float JM[2];
            find_display_cusp_for_hue(hue, RGB_corners, JMh_corners, params, JM);
            output_table[i][0] = JM[0];
            output_table[i][1] = JM[1] * (1.f + smooth_m * smooth_cusps);
            output_table[i][2] = hue;
         }
         output_table[0][0] = output_table[kTableSize][0];
         output_table[0][1] = output_table[kTableSize][1];
         output_table[0][2] = hue_table[0];
         output_table[kBaseIndex + kTableSize][0] = output_table[kBaseIndex][0];
         output_table[kBaseIndex + kTableSize][1] = output_table[kBaseIndex][1];
         output_table[kBaseIndex + kTableSize][2] = hue_table[kBaseIndex + kTableSize];
      }

      inline bool make_uniform_hue_gamut_table(const JMhParams& reach_params, const JMhParams& limit_params, const ODTParams& p, float cusp_JMh_table[kTotalTableSize][3])
      {
         F3 reach_JMh_corners[totalCornerCount];
         F3 limiting_RGB_corners[totalCornerCount];
         F3 limiting_JMh_corners[totalCornerCount];
         find_reach_corners_table(reach_JMh_corners, reach_params, p);
         build_limiting_cusp_corners_tables(limiting_RGB_corners, limiting_JMh_corners, limit_params, p.peakLuminance);
         float sorted_hues[max_sorted_corners];
         extract_sorted_cube_hues(reach_JMh_corners, limiting_JMh_corners, sorted_hues);
         float hue_table[kTotalTableSize];
         if (!build_hue_table(sorted_hues, hue_table))
            return false;
         build_cusp_table(hue_table, limiting_RGB_corners, limiting_JMh_corners, limit_params, cusp_JMh_table);
         return true;
      }

      inline void make_reach_m_table(const JMhParams& params, float limitJmax, float reachTable[kTotalTableSize])
      {
         for (int i = 0; i < kTableSize; ++i)
         {
            const float hue = base_hue_for_position(i, kTableSize);
            const float search_range = 50.f;
            const float search_maximum = 1300.f;
            float low = 0.f;
            float high = low + search_range;
            bool outside = false;
            while ((outside != true) & (high < search_maximum))
            {
               const F3 searchJMh = {{limitJmax, high, hue}};
               const F3 newLimitRGB = JMh_to_RGB(searchJMh, params);
               outside = any_below_zero(newLimitRGB);
               if (outside == false)
               {
                  low = high;
                  high = high + search_range;
               }
            }
            while (high - low > 1e-2f)
            {
               const float sampleM = (high + low) / 2.f;
               const F3 searchJMh = {{limitJmax, sampleM, hue}};
               const F3 newLimitRGB = JMh_to_RGB(searchJMh, params);
               outside = any_below_zero(newLimitRGB);
               if (outside)
                  high = sampleM;
               else
                  low = sampleM;
            }
            reachTable[i + kBaseIndex] = high;
         }
         reachTable[0] = reachTable[kTableSize];
         reachTable[kBaseIndex + kTableSize] = reachTable[kBaseIndex];
      }

      inline bool outside_hull(const F3& rgb, float maxRGBtestVal)
      {
         return rgb[0] > maxRGBtestVal || rgb[1] > maxRGBtestVal || rgb[2] > maxRGBtestVal;
      }

      struct GammaTestData
      {
         float test_JMh[test_count][3];
         float J_intersect_source[test_count];
         float slopes[test_count];
         float J_intersect_cusp[test_count];
      };

      inline void generate_gamma_test_data(const float JMcusp[2], float hue, float limit_J_max, float mid_J, float focus_dist, GammaTestData& d)
      {
         const float analytical_threshold = lerp(JMcusp[0], limit_J_max, focus_gain_blend);
         const float focus_J = compute_focus_J(JMcusp[0], mid_J, limit_J_max);
         for (int testIndex = 0; testIndex != test_count; ++testIndex)
         {
            const float test_J = lerp(JMcusp[0], limit_J_max, testPositions[testIndex]);
            const float slope_gain = get_focus_gain(test_J, analytical_threshold, limit_J_max, focus_dist);
            const float J_intersect = solve_J_intersect(test_J, JMcusp[1], focus_J, limit_J_max, slope_gain);
            const float slope = compute_compression_vector_slope(J_intersect, focus_J, limit_J_max, slope_gain);
            const float J_cusp = solve_J_intersect(JMcusp[0], JMcusp[1], focus_J, limit_J_max, slope_gain);
            d.test_JMh[testIndex][0] = test_J;
            d.test_JMh[testIndex][1] = JMcusp[1];
            d.test_JMh[testIndex][2] = hue;
            d.J_intersect_source[testIndex] = J_intersect;
            d.slopes[testIndex] = slope;
            d.J_intersect_cusp[testIndex] = J_cusp;
         }
      }

      inline bool evaluate_gamma_fit(const float JMcusp[2], const GammaTestData& d, float top_gamma_inv, float peakLuminance, float limit_J_max, float lower_hull_gamma_inv, const JMhParams& limit_params)
      {
         const float luminance_limit = peakLuminance / ref_luminance;
         for (int testIndex = 0; testIndex < test_count; ++testIndex)
         {
            const float approxLimit_M = find_gamut_boundary_intersection(JMcusp, limit_J_max, top_gamma_inv, lower_hull_gamma_inv, d.J_intersect_source[testIndex], d.slopes[testIndex], d.J_intersect_cusp[testIndex]);
            const float approxLimit_J = d.J_intersect_source[testIndex] + d.slopes[testIndex] * approxLimit_M;
            const F3 approximate_JMh = {{approxLimit_J, approxLimit_M, d.test_JMh[testIndex][2]}};
            const F3 newLimitRGB = JMh_to_RGB(approximate_JMh, limit_params);
            if (!outside_hull(newLimitRGB, luminance_limit))
               return false;
         }
         return true;
      }

      inline void make_upper_hull_gamma_table(const float gamutCuspTable[kTotalTableSize][3], const ODTParams& p, float upper_hull_gamma[kTotalTableSize])
      {
         for (int i = kBaseIndex; i != kBaseIndex + kTableSize; ++i)
         {
            const float hue = gamutCuspTable[i][2];
            const float JMcusp[2] = {gamutCuspTable[i][0], gamutCuspTable[i][1]};
            GammaTestData d;
            generate_gamma_test_data(JMcusp, hue, p.limit_J_max, p.mid_J, p.focus_dist, d);

            const float search_range = gamma_search_step;
            float low = gamma_minimum;
            float high = low + search_range;
            bool outside = false;
            while (!(outside) && (high < gamma_maximum))
            {
               const bool gammaFound = evaluate_gamma_fit(JMcusp, d, 1.f / high, p.peakLuminance, p.limit_J_max, p.lower_hull_gamma_inv, p.limit_params);
               if (!gammaFound)
               {
                  low = high;
                  high = high + search_range;
               }
               else
               {
                  outside = true;
               }
            }

            float testGamma = -1.0f;
            while ((high - low) > gamma_accuracy)
            {
               testGamma = midpoint(high, low);
               const bool gammaFound = evaluate_gamma_fit(JMcusp, d, 1.f / testGamma, p.peakLuminance, p.limit_J_max, p.lower_hull_gamma_inv, p.limit_params);
               if (gammaFound)
                  high = testGamma;
               else
                  low = testGamma;
            }
            upper_hull_gamma[i] = 1.f / high;
         }
         upper_hull_gamma[0] = upper_hull_gamma[kTableSize];
         upper_hull_gamma[kTableSize + kBaseIndex] = upper_hull_gamma[kBaseIndex];
      }

      inline void determine_hue_linearity_search_range(const float hue_table[kTotalTableSize], int hue_linearity_search_range[2])
      {
         const int lower_padding = 0;
         const int upper_padding = 1;
         hue_linearity_search_range[0] = lower_padding;
         hue_linearity_search_range[1] = upper_padding;
         for (int i = kBaseIndex; i != kBaseIndex + kTableSize; ++i)
         {
            const int pos = hue_position_in_uniform_table(hue_table[i], kTotalTableSize);
            const int delta = i - pos;
            hue_linearity_search_range[0] = int(ctl_min(float(hue_linearity_search_range[0]), float(delta + lower_padding)));
            hue_linearity_search_range[1] = int(ctl_max(float(hue_linearity_search_range[1]), float(delta + upper_padding)));
         }
      }

      inline void store_rows(const M33& ctl, float rows[3][4])
      {
         // Row r of the packed matrix is column r of the CTL (row-vector) matrix.
         for (int r = 0; r < 3; ++r)
         {
            for (int c = 0; c < 3; ++c)
               rows[r][c] = ctl.m[c][r];
            rows[r][3] = 0.f;
         }
      }
   } // namespace detail

   // init_ODTParams(peakLuminance, limitingPrimaries) plus the matrices the
   // per-pixel path and the caller need. Deterministic; returns false for
   // parameters the transform is not defined for (peak above 10000 nits or
   // below ~46.4 nits, where the tone scale's r_hit is negative; degenerate
   // primaries) or if any table entry is not finite.
   inline bool BuildParams(float peak_nits, const Chromaticities& limiting, ODTParams& p, const Tuning& tuning = {})
   {
      using namespace detail;
      if (!(peak_nits >= 10.f && peak_nits <= 10000.f))
         return false;
      std::memset(&p, 0, sizeof(p));
      p.peakLuminance = peak_nits;

      if (!init_JMhParams(AP0, p.input_params) || !init_JMhParams(AP1, p.reach_params) || !init_JMhParams(limiting, p.limit_params))
         return false;

      // clamp_AP0_to_AP1 matrices, and the limiting -> BT.709 conversion
      // (mult_f33_f33(RGBtoXYZ_f33(limiting, 1.0), XYZtoRGB_f33(Rec709, 1.0))).
      {
         D33 ap0_xyz_to_rgb, ap1_xyz_to_rgb, rec709_xyz_to_rgb;
         if (!XYZtoRGB(AP0, 1.0, ap0_xyz_to_rgb) || !XYZtoRGB(AP1, 1.0, ap1_xyz_to_rgb) || !XYZtoRGB(Rec709, 1.0, rec709_xyz_to_rgb))
            return false;
         p.AP0_TO_AP1 = to_float(mul(RGBtoXYZ(AP0, 1.0), ap1_xyz_to_rgb));
         p.AP1_TO_AP0 = to_float(mul(RGBtoXYZ(AP1, 1.0), ap0_xyz_to_rgb));
         D33 limit_to_rec709 = mul(RGBtoXYZ(limiting, 1.0), rec709_xyz_to_rgb);
         if (tuning.scale_white)
         {
            // apply_white_scale (Lib.Academy.DisplayEncoding.ctl): 1 / largest
            // channel of limiting white (1, 1, 1) in the encoding primaries. A
            // uniform scale, so it is folded into the output matrix.
            D33 encoding_xyz_to_rgb;
            if (!XYZtoRGB(tuning.encoding, 1.0, encoding_xyz_to_rgb))
               return false;
            const D33 limit_to_encoding = mul(RGBtoXYZ(limiting, 1.0), encoding_xyz_to_rgb);
            double largest = 0.0;
            for (int column = 0; column < 3; ++column)
            {
               double white = 0.0;
               for (int row = 0; row < 3; ++row)
                  white += limit_to_encoding.m[row][column];
               largest = largest > white ? largest : white;
            }
            if (!(largest > 0.0))
               return false;
            for (auto& row : limit_to_rec709.m)
               for (double& value : row)
                  value /= largest;
         }
         if (!finite_matrix(limit_to_rec709))
            return false;
         p.limit_to_rec709 = to_float(limit_to_rec709);
      }

      p.ts = init_TSParams(peak_nits, tuning);
      // Below ~46.4 nits r_hit, and with it forward_limit, is not positive:
      // the tone scale is not defined there.
      if (!(p.ts.forward_limit > 0.f))
         return false;

      p.limit_J_max = Y_to_J(peak_nits, p.input_params);
      p.model_gamma_inv = 1.f / model_gamma();
      make_reach_m_table(p.reach_params, p.limit_J_max, p.TABLE_reach_M);

      p.sat = ctl_max(0.2f, chroma_expand - (chroma_expand * chroma_expand_fact) * p.ts.log_peak) * tuning.chroma_expansion;
      p.sat_thr = chroma_expand_thr / peak_nits;
      p.compr = (chroma_compress + (chroma_compress * chroma_compress_fact) * p.ts.log_peak) * tuning.chroma_compression;
      p.chroma_compress_scale = std::pow(0.03379f * peak_nits, 0.30596f) - 0.45135f;

      p.mid_J = Y_to_J(p.ts.c_t * ref_luminance, p.input_params);
      p.focus_dist = focus_distance + focus_distance * focus_distance_scaling * p.ts.log_peak;
      const float lower_hull_gamma = 1.14f + 0.07f * p.ts.log_peak;
      p.lower_hull_gamma_inv = 1.f / lower_hull_gamma;
      if (!make_uniform_hue_gamut_table(p.reach_params, p.limit_params, p, p.TABLE_gamut_cusps))
         return false;
      for (int i = 0; i != kTotalTableSize; ++i)
         p.TABLE_hues[i] = p.TABLE_gamut_cusps[i][2];
      make_upper_hull_gamma_table(p.TABLE_gamut_cusps, p, p.TABLE_upper_hull_gamma);
      determine_hue_linearity_search_range(p.TABLE_hues, p.hue_linearity_search_range);

      // Sanity: finite tables, strictly increasing hues (the lookups assume it).
      for (int i = 0; i < kTotalTableSize; ++i)
      {
         if (!std::isfinite(p.TABLE_reach_M[i]) || !std::isfinite(p.TABLE_hues[i]) || !std::isfinite(p.TABLE_gamut_cusps[i][0]) || !std::isfinite(p.TABLE_gamut_cusps[i][1]) || !std::isfinite(p.TABLE_upper_hull_gamma[i]))
            return false;
         if (i > 0 && !(p.TABLE_hues[i] > p.TABLE_hues[i - 1]))
            return false;
      }
      const float scalars[] = {p.limit_J_max, p.sat, p.sat_thr, p.compr, p.chroma_compress_scale, p.mid_J, p.focus_dist, p.lower_hull_gamma_inv, p.ts.m_2, p.ts.s_2, p.ts.forward_limit};
      for (float v : scalars)
         if (!std::isfinite(v))
            return false;
      return p.chroma_compress_scale > 0.f;
   }

   inline void Pack(const ODTParams& p, Constants& out)
   {
      using namespace detail;
      std::memset(&out, 0, sizeof(out));
      out.peak_luminance = p.peakLuminance;
      out.forward_limit = p.ts.forward_limit;
      out.limit_J_max = p.limit_J_max;
      out.model_gamma_inv = p.model_gamma_inv;
      out.ts_n_r = p.ts.n_r;
      out.ts_g = p.ts.g;
      out.ts_t_1 = p.ts.t_1;
      out.ts_s_2 = p.ts.s_2;
      out.ts_m_2 = p.ts.m_2;
      out.sat = p.sat;
      out.sat_thr = p.sat_thr;
      out.compr = p.compr;
      out.chroma_compress_scale = p.chroma_compress_scale;
      out.mid_J = p.mid_J;
      out.focus_dist = p.focus_dist;
      out.lower_hull_gamma_inv = p.lower_hull_gamma_inv;
      out.in_F_L_n = p.input_params.F_L_n;
      out.in_cz = p.input_params.cz;
      out.in_inv_cz = p.input_params.inv_cz;
      out.in_A_w_J = p.input_params.A_w_J;
      out.in_inv_A_w_J = p.input_params.inv_A_w_J;
      out.limit_inv_cz = p.limit_params.inv_cz;
      out.hue_search_lo = p.hue_linearity_search_range[0];
      out.hue_search_hi = p.hue_linearity_search_range[1];
      store_rows(p.AP0_TO_AP1, out.ap0_to_ap1);
      store_rows(p.AP1_TO_AP0, out.ap1_to_ap0);
      store_rows(p.input_params.MATRIX_RGB_to_CAM16_c, out.in_rgb_to_cam16_c);
      store_rows(p.input_params.MATRIX_cone_response_to_Aab, out.in_cone_response_to_aab);
      store_rows(p.limit_params.MATRIX_Aab_to_cone_response, out.limit_aab_to_cone_response);
      store_rows(p.limit_params.MATRIX_CAM16_c_to_RGB, out.limit_cam16_c_to_rgb);
      store_rows(p.limit_to_rec709, out.limit_to_rec709);
      for (int i = 0; i < kTotalTableSize; ++i)
      {
         out.table[i][0] = p.TABLE_gamut_cusps[i][0];
         out.table[i][1] = p.TABLE_gamut_cusps[i][1];
         out.table[i][2] = p.TABLE_hues[i];
         out.table[i][3] = p.TABLE_upper_hull_gamma[i];
         out.reach_m[i / 4][i % 4] = p.TABLE_reach_M[i];
      }
   }

   // Builds the constant buffer image for a peak (nits) and limiting primaries.
   // On failure `out` is left zeroed and false is returned.
   inline bool Build(float peak_nits, const Chromaticities& limiting, Constants& out, const Tuning& tuning = {})
   {
      ODTParams p;
      if (!BuildParams(peak_nits, limiting, p, tuning))
      {
         std::memset(&out, 0, sizeof(out));
         return false;
      }
      Pack(p, out);
      return true;
   }

} // namespace Battlefront2ACES2
