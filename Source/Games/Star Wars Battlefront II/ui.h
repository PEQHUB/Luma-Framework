#pragma once
// The settings menu.

#include <string>
#include <unordered_map>
#include <cfloat>
#include <algorithm>
#include <memory>
#include <mutex>
#include <array>
#include <cstdio>
#include <atomic>
#include <cmath>
#include <utility>
#include "swapchain.h"
#include "tone_mapping.h"
#include "ssr.h"
#include "xegtao.h"

namespace Battlefront2UI
{
   // The game's menus use the Roboto family, so the menu uses Roboto Condensed SemiBold (fonts/, SIL Open Font
   // License), embedded as resource 101 (fonts.rc) and added to the overlay's font atlas once per ImGui context.
   inline ImFont* MenuFont()
   {
      static const auto resource = []() -> std::pair<const void*, uint32_t>
      {
         static const int anchor = 0;
         HMODULE module = nullptr;
         if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&anchor), &module))
            return {nullptr, 0};
         const HRSRC info = FindResourceW(module, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10) /*RT_RCDATA*/);
         const HGLOBAL data = info ? LoadResource(module, info) : nullptr;
         return {data ? LockResource(data) : nullptr, data ? static_cast<uint32_t>(SizeofResource(module, info)) : 0u};
      }();
      if (!resource.first) return nullptr;
      static ImGuiContext* context = nullptr;
      static ImFont* font = nullptr;
      ImFontAtlas* atlas = ImGui::GetIO().Fonts;
      if (context != ImGui::GetCurrentContext() || !font || !atlas->Fonts.contains(font))
      {
         context = ImGui::GetCurrentContext();
         ImFontConfig config;
         config.FontDataOwnedByAtlas = false; // The resource stays mapped with the module.
         std::snprintf(config.Name, sizeof(config.Name), "%s", "Battlefront II Luma");
         font = atlas->AddFontFromMemoryTTF(const_cast<void*>(resource.first), static_cast<int>(resource.second), 0.f, &config);
      }
      return font;
   }
   // Draws everything in its scope with the menu font.
   struct MenuFontScope
   {
      ImFont* const font = MenuFont();
      MenuFontScope()
      {
         if (font) ImGui::PushFont(font, 0.f);
      }
      ~MenuFontScope()
      {
         if (font) ImGui::PopFont();
      }
      MenuFontScope(const MenuFontScope&) = delete;
      MenuFontScope& operator=(const MenuFontScope&) = delete;
   };

   // Battlefront's settings use uppercase labels. Widget IDs keep the original strings.
   inline const char* MenuText(const char* source)
   {
      static std::unordered_map<std::string, std::string> cache;
      if (const auto it = cache.find(source); it != cache.end()) return it->second.c_str();
      std::string text = source;
      for (char& c : text)
         if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
      return cache.emplace(source, std::move(text)).first->second.c_str();
   }
   inline std::string MenuLabel(const char* source)
   {
      if (source[0] == '#') return source;
      return std::string(MenuText(source)) + "###" + source;
   }

   // Menu widgets: uppercase labels, the two-column row layout and reset on right-click.
   // They are only drawn inside DrawPanel (below).
   inline bool BeginRows(const char* id)
   {
      if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerH)) return false;
      ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, .45f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, .55f);
      return true;
   }
   inline void Row(const char* label)
   {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      ImGui::TextWrapped("%s", MenuText(label));
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
   }
   inline void Hint(const char* text)
   {
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
      {
         ImGui::BeginTooltip();
         ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.f);
         ImGui::TextUnformatted(text);
         ImGui::PopTextWrapPos();
         ImGui::EndTooltip();
      }
   }
   template <class... A>
   inline void TextDisabled(const char* fmt, A... args)
   {
      ImGui::TextDisabled(fmt, args...);
   }
   template <class... A>
   inline void TextColored(ImVec4 color, const char* fmt, A... args)
   {
      ImGui::TextColored(color, fmt, args...);
   }
   inline bool CollapsingHeader(const char* label)
   {
      return ImGui::CollapsingHeader(MenuLabel(label).c_str());
   }
   // A muted status line under a group, aligned with its subheading.
   template <class... A>
   inline void Status(const char* fmt, A... args)
   {
      const float u = ImGui::GetFontSize();
      ImGui::Indent(u * .6f);
      ImGui::TextDisabled(fmt, args...);
      ImGui::Unindent(u * .6f);
   }
   inline bool Button(const char* label)
   {
      return ImGui::Button(label);
   }
   inline bool SmallButton(const char* label)
   {
      return ImGui::SmallButton(label);
   }
   inline bool Checkbox(const char* label, bool* value)
   {
      return ImGui::Checkbox(label, value);
   }
   inline bool Selectable(const char* label, bool selected)
   {
      return ImGui::Selectable(MenuLabel(label).c_str(), selected);
   }
   inline bool BeginCombo(const char* label, const char* preview)
   {
      return ImGui::BeginCombo(label, MenuText(preview));
   }
   inline bool Combo(const char* label, int* selected, const char* const* items, int count)
   {
      std::vector<const char*> upper;
      upper.reserve(count);
      for (int i = 0; i < count; ++i)
         upper.push_back(MenuText(items[i]));
      return ImGui::Combo(label, selected, upper.data(), count);
   }
   inline bool Combo(const char* label, int* selected, const char* items)
   {
      std::vector<const char*> list;
      for (const char* p = items; *p; p += std::strlen(p) + 1)
         list.push_back(p);
      return Combo(label, selected, list.data(), int(list.size()));
   }
   // Right-click menu on the last item, plus a reset button after it when show_button is set.
   inline bool ResetItem(bool at_default, bool show_button = true)
   {
      const ImGuiID id = ImGui::GetItemID();
      const float height = ImGui::GetItemRectSize().y;
      bool reset = false;
      if (ImGui::BeginPopupContextItem())
      {
         reset = ImGui::MenuItem("Reset to default");
         ImGui::EndPopup();
      }
      if (show_button)
      {
         ImGui::SameLine();
         ImGui::PushID(int(id));
         ImGui::BeginDisabled(at_default);
         reset |= ImGui::Button("##reset", {height, height});
         const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
         const ImVec2 center{(a.x + b.x) * .5f, (a.y + b.y) * .5f};
         const float r = ImGui::GetFontSize() * .28f;
         const auto color = ImGui::GetColorU32(at_default ? ImGuiCol_TextDisabled : ImGuiCol_Text);
         auto* draw = ImGui::GetWindowDrawList();
         draw->PathArcTo(center, r, -1.570796f, 3.141593f, 20);
         draw->PathStroke(color, 0, ImGui::GetFontSize() * .08f);
         draw->AddTriangleFilled({center.x - r, center.y - r * .55f},
            {center.x - r * 1.55f, center.y + r * .2f}, {center.x - r * .45f, center.y + r * .2f}, color);
         ImGui::EndDisabled();
         Hint("Reset to default");
         ImGui::PopID();
      }
      return reset;
   }
   // A saber-shaped slider next to an editable number. The slider keeps ImGui's
   // interaction and navigation; dragging, double-click and Ctrl-click entry are
   // in the number box.
   inline bool SliderFloat(const char* label, float* v, float lo, float hi, float default_value, const char* fmt = "%.3f", const char* hint = nullptr)
   {
      const float u = ImGui::GetFontSize(), height = ImGui::GetFrameHeight(), gap = ImGui::GetStyle().ItemSpacing.x;
      const float width = ImGui::CalcItemWidth();
      const float value_width = u * 5.2f;
      ImGui::BeginGroup();
      ImGui::PushID(label);
      const ImVec2 hilt = ImGui::GetCursorScreenPos();
      const float hilt_width = u * .85f;
      ImGui::Dummy({hilt_width, height});
      ImGui::SameLine(0, 0);
      ImGui::SetNextItemWidth((std::max)(u * 1.5f, width - value_width - height - gap * 2.f - hilt_width));
      ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, u * .48f);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
      ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
      bool changed = ImGui::SliderFloat("##track", v, lo, hi, fmt, ImGuiSliderFlags_NoInput);
      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar(2);
      const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      // Match ImGui's native grab travel (2px padding + half the grab size).
      const float left = a.x + 2.f + u * .24f, right = b.x - 2.f - u * .24f, y = (a.y + b.y) * .5f;
      const float t = hi > lo ? (std::clamp)((*v - lo) / (hi - lo), 0.f, 1.f) : 0.f;
      const float x = left + (right - left) * t;
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddLine({left, y}, {right, y}, ImGui::GetColorU32(ImGuiCol_Separator), u * .10f);
      const auto blade = ImGui::IsItemActive() ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab;
      // Hilt, colored blade, white core and tip; the glow stays inside the hit area.
      if (t > 0.f)
      {
         draw->AddLine({left, y}, {x, y}, ImGui::GetColorU32(blade, .12f), u * .65f);
         draw->AddLine({left, y}, {x, y}, ImGui::GetColorU32(blade, .28f), u * .40f);
         draw->AddLine({left, y}, {x, y}, ImGui::GetColorU32(blade), u * .20f);
         draw->AddLine({left, y}, {x, y}, ImGui::GetColorU32(ImVec4(.96f, .99f, 1.f, 1.f)), u * .065f);
      }
      draw->AddCircleFilled({x, y}, u * .20f, ImGui::GetColorU32(blade));
      draw->AddCircleFilled({x, y}, u * .09f, ImGui::GetColorU32(ImGuiCol_Text));
      const float grip_left = hilt.x + u * .10f, grip_right = left - u * .13f;
      draw->AddRectFilled({grip_left, y - u * .15f}, {grip_right, y + u * .15f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), u * .05f);
      draw->AddLine({grip_left, y - u * .15f}, {grip_right, y - u * .15f}, ImGui::GetColorU32(ImGuiCol_Text), u * .04f);
      for (int i = 1; i <= 3; ++i)
      {
         const float grip = grip_left + (grip_right - grip_left) * i / 4.f;
         draw->AddLine({grip, y - u * .13f}, {grip, y + u * .13f}, ImGui::GetColorU32(ImVec4(.04f, .05f, .06f, 1.f)), u * .055f);
      }
      draw->AddRectFilled({grip_right - u * .08f, y - u * .22f}, {grip_right + u * .04f, y + u * .22f}, ImGui::GetColorU32(ImGuiCol_Text));
      if (hint) Hint(hint);
      if (ResetItem(*v == default_value, false))
      {
         *v = default_value;
         changed = true;
      }
      ImGui::SameLine();
      ImGui::SetNextItemWidth(value_width);
      changed |= ImGui::DragFloat("##number", v, (hi - lo) * .002f, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
      if (hint) Hint(hint);
      if (ResetItem(*v == default_value))
      {
         *v = default_value;
         changed = true;
      }
      ImGui::PopID();
      ImGui::EndGroup();
      return changed;
   }

   // Panel-local styling: the overlay's other windows and saved theme are untouched.
   // All dimensions follow the player's current font scale.
   struct PanelStyle
   {
      int colors = 0, vars = 0;
      inline static constexpr ImVec4 ink{.94f, .96f, .97f, 1.f};
      inline static constexpr ImVec4 muted{.48f, .55f, .58f, 1.f};
      inline static constexpr ImVec4 accent{1.f, .78f, .16f, 1.f};
      void Color(ImGuiCol id, ImVec4 value)
      {
         ImGui::PushStyleColor(id, value);
         ++colors;
      }
      void Var(ImGuiStyleVar id, float value)
      {
         ImGui::PushStyleVar(id, value);
         ++vars;
      }
      void Var(ImGuiStyleVar id, ImVec2 value)
      {
         ImGui::PushStyleVar(id, value);
         ++vars;
      }
      PanelStyle()
      {
         const float u = ImGui::GetFontSize();
         Color(ImGuiCol_Text, ink);
         Color(ImGuiCol_TextDisabled, muted);
         Color(ImGuiCol_ChildBg, {.018f, .023f, .029f, 1.f});
         Color(ImGuiCol_PopupBg, {.025f, .032f, .038f, 1.f});
         Color(ImGuiCol_Border, {.40f, .46f, .49f, .7f});
         Color(ImGuiCol_BorderShadow, {0, 0, 0, 0});
         Color(ImGuiCol_FrameBg, {.09f, .11f, .12f, .75f});
         Color(ImGuiCol_FrameBgHovered, {.20f, .19f, .13f, 1.f});
         Color(ImGuiCol_FrameBgActive, {.29f, .25f, .13f, 1.f});
         Color(ImGuiCol_CheckMark, ink);
         Color(ImGuiCol_SliderGrab, {.24f, .70f, 1.f, 1.f});
         Color(ImGuiCol_SliderGrabActive, {.52f, .84f, 1.f, 1.f});
         Color(ImGuiCol_Button, {.08f, .095f, .105f, 1.f});
         Color(ImGuiCol_ButtonHovered, {.24f, .22f, .14f, 1.f});
         Color(ImGuiCol_ButtonActive, {.34f, .28f, .12f, 1.f});
         Color(ImGuiCol_Header, {0, 0, 0, 0});
         Color(ImGuiCol_HeaderHovered, {.23f, .20f, .12f, 1.f});
         Color(ImGuiCol_HeaderActive, {.31f, .25f, .10f, 1.f});
         Color(ImGuiCol_Separator, {.38f, .44f, .47f, .7f});
         Color(ImGuiCol_TableBorderLight, {.35f, .40f, .43f, .45f});
         Color(ImGuiCol_TableBorderStrong, {.40f, .46f, .49f, .7f});
         Color(ImGuiCol_SeparatorHovered, accent);
         Color(ImGuiCol_SeparatorActive, accent);
         Color(ImGuiCol_ResizeGrip, {1.f, .78f, .16f, .15f});
         Color(ImGuiCol_NavCursor, accent);
         Color(ImGuiCol_TextSelectedBg, {1.f, .78f, .16f, .25f});
         Color(ImGuiCol_ScrollbarBg, {0, 0, 0, 0});
         Color(ImGuiCol_ScrollbarGrab, {.24f, .29f, .33f, 1.f});
         Color(ImGuiCol_ScrollbarGrabHovered, {.35f, .39f, .41f, 1.f});
         Color(ImGuiCol_ScrollbarGrabActive, accent);
         Var(ImGuiStyleVar_WindowPadding, {u * .8f, u * .55f});
         Var(ImGuiStyleVar_FramePadding, {u * .5f, u * .24f});
         Var(ImGuiStyleVar_ItemSpacing, {u * .5f, u * .18f});
         Var(ImGuiStyleVar_CellPadding, {u * .6f, u * .20f});
         Var(ImGuiStyleVar_FrameRounding, u * .04f);
         Var(ImGuiStyleVar_ChildRounding, 0.f);
         Var(ImGuiStyleVar_PopupRounding, u * .04f);
         Var(ImGuiStyleVar_GrabRounding, u * .12f);
         Var(ImGuiStyleVar_GrabMinSize, u * .22f);
         Var(ImGuiStyleVar_FrameBorderSize, 0.f);
         Var(ImGuiStyleVar_ScrollbarSize, u * .42f);
         Var(ImGuiStyleVar_IndentSpacing, u * .8f);
      }
      ~PanelStyle()
      {
         ImGui::PopStyleVar(vars);
         ImGui::PopStyleColor(colors);
      }
   };

   enum class Category
   {
      Graphics,
      Display,
      Effects,
      Advanced
   };
   struct CategoryStyle
   {
      CategoryStyle(Category category)
      {
         // Only the saber blades carry the category color; the rest follows the
         // game's monochrome settings with gold highlights.
         const ImVec4 color = category == Category::Effects ? ImVec4(.27f, .93f, .52f, 1.f) : category == Category::Advanced ? ImVec4(.75f, .86f, 1.f, 1.f)
                                                                                                                             : ImVec4(.24f, .70f, 1.f, 1.f);
         ImGui::PushStyleColor(ImGuiCol_SliderGrab, color);
         ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, {(color.x + 1.f) * .5f, (color.y + 1.f) * .5f, (color.z + 1.f) * .5f, 1.f});
      }
      ~CategoryStyle()
      {
         ImGui::PopStyleColor(2);
      }
   };

   inline void PanelHeading(bool hdr)
   {
      const float u = ImGui::GetFontSize();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float width = ImGui::GetContentRegionAvail().x;
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddText(ImGui::GetFont(), u * 1.4f, p, ImGui::GetColorU32(PanelStyle::ink), "LUMA");
      const char* mode = hdr ? "HDR" : "SDR";
      const ImVec2 text = ImGui::CalcTextSize(mode);
      draw->AddText({p.x + width - text.x - u * .6f, p.y + u * .25f}, ImGui::GetColorU32(PanelStyle::muted), mode);
      ImGui::Dummy({width, u * 1.9f});
   }

   inline void SectionTitle(const char* title)
   {
      const auto p = ImGui::GetCursorScreenPos();
      const float u = ImGui::GetFontSize(), width = ImGui::GetContentRegionAvail().x;
      auto* draw = ImGui::GetWindowDrawList();
      draw->AddText(ImGui::GetFont(), u * 1.1f, {p.x + u * .6f, p.y + u * .24f}, ImGui::GetColorU32(PanelStyle::ink), MenuText(title));
      draw->AddLine({p.x, p.y + u * 1.65f}, {p.x + width, p.y + u * 1.65f}, ImGui::GetColorU32(ImGuiCol_Border));
      ImGui::Dummy({width, u * 1.75f});
   }
   inline bool CategoryHeader(const char* title)
   {
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetFontSize() * .6f, ImGui::GetFontSize() * .4f));
      const bool open = CollapsingHeader(title);
      ImGui::PopStyleVar();
      const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddLine({a.x, b.y}, {b.x, b.y}, ImGui::GetColorU32(ImGuiCol_Border));
      return open;
   }
   inline bool BeginSection(const char* id, const char* title = nullptr)
   {
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, ImGui::GetFontSize() * .25f));
      const bool visible = ImGui::BeginChild(id, {0, 0}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AlwaysUseWindowPadding,
         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      if (visible && title) SectionTitle(title);
      return visible;
   }
   inline void EndSection()
   {
      ImGui::EndChild();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      ImGui::Spacing();
   }
} // namespace Battlefront2UI

namespace Battlefront2Reflex
{
   inline std::mutex runtime_mutex;
   inline Library runtime_library;
   inline std::unique_ptr<DriverMode> driver_mode;
   inline IUnknown* attached_device = nullptr;
   inline int requested_mode = 2;
   inline void Shutdown(IUnknown* device = nullptr)
   {
      std::lock_guard lock(runtime_mutex);
      if (device && device != attached_device) return;
      driver_mode.reset();
      attached_device = nullptr;
   }
   inline void Initialize(ID3D11Device* device)
   {
      std::lock_guard lock(runtime_mutex);
      if (attached_device == device) return;
      driver_mode.reset();
      attached_device = nullptr;
      Config::Get(NAME, "BattlefrontIIReflexDriverMode", requested_mode);
      requested_mode = std::clamp(requested_mode, 0, 2);
      if (!runtime_library.Load()) return;
      driver_mode = std::make_unique<DriverMode>(runtime_library.GetApi(), device);
      attached_device = device;
      if (!driver_mode->Configure(static_cast<Mode>(requested_mode)) && requested_mode != static_cast<int>(Mode::off))
         Luma::Utils::Log::Write(Luma::Utils::Log::Level::Warning, "[Battlefront II Luma] NVIDIA low latency: not applied (another program owns the driver's sleep mode, or the driver refused it).");
   }
   inline void DrawControl()
   {
      std::lock_guard lock(runtime_mutex);
      Battlefront2UI::Row("NVIDIA low latency");
      ImGui::BeginDisabled(!driver_mode);
      const char* modes[] = {"Off", "On", "On + Boost"};
      int selection = requested_mode;
      if (Battlefront2UI::Combo("##reflex", &selection, modes, 3) && driver_mode && driver_mode->Configure(static_cast<Mode>(selection)))
      {
         requested_mode = selection;
         Config::Set(NAME, "BattlefrontIIReflexDriverMode", selection);
      }
      Battlefront2UI::Hint(driver_mode ? "Reduces input lag. Boost uses more GPU power." : "Unavailable on this device or driver.");
      ImGui::EndDisabled();
   }
} // namespace Battlefront2Reflex

namespace Battlefront2Psycho
{
   // Version of the saved grading profile. Before version 1 the menu had a
   // slider for every stage of the game's LUT and "PsychoLUTStrength" faded the
   // grade to the identity. Now the stages are config-only and the slider keeps
   // the game's tone curve, so older stage values are not loaded.
   inline constexpr unsigned GradingProfileVersion = 1;
   inline bool GradingStage(Field field)
   {
      return field >= NativeLUTMatrix && field <= NativeLUTPostExposure;
   }
   inline void Save(Settings value)
   {
      Sanitize(value); // Store() sanitizes its own copy; the config must get the same values.
      Store(value);
      for (const auto& c : controls)
         Config::Set(NAME, c.key, value.values[c.field]);
      Config::Set(NAME, "BattlefrontIIGradingProfileVersion", GradingProfileVersion);
   }
   inline void SaveOutputMode(OutputMode mode)
   {
      output_mode.store(mode);
      Config::Set(NAME, "LumaOutputMode", int(mode));
   }
   inline void Load()
   {
      int mode = int(OutputMode::Auto);
      if (!Config::Get(NAME, "LumaOutputMode", mode))
      {
         bool legacy_hdr = false;
         if (Config::Get(NAME, "LumaHDRRendering", legacy_hdr))
            mode = int(legacy_hdr ? OutputMode::Auto : OutputMode::SDR);
      }
      output_mode = mode == int(OutputMode::SDR) ? OutputMode::SDR : OutputMode::Auto;
      auto value = Defaults();
      unsigned grading_profile = 0;
      Config::Get(NAME, "BattlefrontIIGradingProfileVersion", grading_profile);
      const bool current_grading = grading_profile == GradingProfileVersion;
      for (const auto& c : controls)
      {
         if (!current_grading && GradingStage(c.field)) continue;
         Config::Get(NAME, c.key, value.values[c.field]);
      }
      // Loading never writes: the version is saved with the next change.
      Store(value);
   }
   enum class Page
   {
      Display,
      ToneMapping,
      Effects,
      Advanced,
      Screenshots
   };
   inline const Control& Definition(Field field)
   {
      for (const auto& c : controls)
         if (c.field == field) return c;
      return controls[0];
   }
   inline const char* ControlHint(Field field)
   {
      switch (field)
      {
      case HDRPeak:
         return "Set this to your display's peak brightness.";
      case HDRPaper:
      case GT7PaperWhite:
         return "Brightness of ordinary scene whites.";
      case NativeUI:
         return "Turn off to adjust UI brightness separately.";
      case DistortionStrength:
         return "Heat haze and ability distortion.";
      case LUTStrength:
         return "The game's colors: map tint, saturation and the whitening of bright lights. 0% shows the scene's own colors. 100% is the game's grading.";
      case GlareStrength:
         return "Glow around very bright lights such as the sun, lightsabers and explosions, as the eye would see it. 1.00 is physical.";
      case LightGamutExpansion:
         return "Expands the colour of blaster bolts and lightsabers from BT.709 toward BT.2020, keeping brightness and hue. 0 is the game's colour. HDR with DLSS only.";
      default:
         return nullptr;
      }
   }
   inline const char* ControlFormat(Field field)
   {
      switch (field)
      {
      case HDRPeak:
      case HDRPaper:
      case HDRUI:
      case SDRWhite:
      case GT7PaperWhite:
         return "%.0f nits";
      case ExposureEV:
      case GT7Exposure:
      case ACES2Exposure:
         return "%+.2f EV";
      default:
         return "%.2f";
      }
   }
   // One menu row. Only the fields the menu shows are handled; the rest are config-only.
   inline bool DrawControl(Settings& value, Field field, const char* label, bool disabled = false)
   {
      const auto& c = Definition(field);
      float& v = value.values[field];
      bool changed = false;
      const char* hint = ControlHint(field);
      ImGui::PushID(int(field));
      Battlefront2UI::Row(label);
      ImGui::BeginDisabled(disabled);
      if (field == NativeUI || field == MotionBlurEnabled)
      {
         bool enabled = v != 0.f;
         if (Battlefront2UI::Checkbox("##value", &enabled))
         {
            v = enabled ? 1.f : 0.f;
            changed = true;
         }
         if (hint) Battlefront2UI::Hint(hint);
         if (Battlefront2UI::ResetItem(v == c.value, false))
         {
            v = c.value;
            changed = true;
         }
      }
      else if (field == LUTStrength)
      {
         float percent = v * 100.f;
         if (Battlefront2UI::SliderFloat("##value", &percent, 0.f, 100.f, c.value * 100.f, "%.0f%%", hint))
         {
            v = percent / 100.f;
            changed = true;
         }
      }
      else
         changed = Battlefront2UI::SliderFloat("##value", &v, c.minimum, c.maximum, c.value, ControlFormat(field), hint);
      ImGui::EndDisabled();
      ImGui::PopID();
      return changed;
   }
   inline void DrawMenu(const State& state, Page page)
   {
      auto value = Snapshot();
      bool changed = false;
      const Mapper mapper = MapperOf(value);
      const bool output_hdr = HDROutput(state.hdr_output, value.values[Enabled] != 0.f);
      if (page == Page::Display)
      {
         if (Battlefront2UI::BeginRows("display"))
         {
            Battlefront2UI::Row("Tone mapper");
            int selected = 0;
            for (int i = 0; i < int(mappers.size()); ++i)
               if (mappers[i] == mapper) selected = i;
            if (Battlefront2UI::Combo("##mapper", &selected, "Native Frostbite\0PsychoV (Psychovisual)\0GT7\0ACES 2.0\0"))
            {
               value.values[Enabled] = float(int(mappers[selected]));
               changed = true;
            }
            // Native always shows the game's full grade.
            changed |= DrawControl(value, LUTStrength, "Game color grading", mapper == Mapper::Native);
            changed |= DrawControl(value, GlareStrength, "Eye glare", mapper == Mapper::Native);
            Battlefront2UI::Row("Output mode");
            int output = int(output_mode.load());
            ImGui::BeginDisabled(value.values[Enabled] == 0.f);
            if (Battlefront2UI::Combo("##hdr", &output, "Auto (Windows HDR)\0SDR\0")) SaveOutputMode(OutputMode(output));
            Battlefront2UI::Hint("Auto outputs HDR while Windows HDR is on, also when it is switched during play.");
            ImGui::EndDisabled();
            if (output_hdr)
            {
               changed |= DrawControl(value, HDRPeak, "Peak brightness");
               // PsychoV and ACES 2.0 share HDR scene white; GT7 has its own
               // (Polyphony's 250-nit paper white). Native has none.
               if (mapper == Mapper::GT7)
                  changed |= DrawControl(value, GT7PaperWhite, "Scene brightness");
               else if (mapper != Mapper::Native)
                  changed |= DrawControl(value, HDRPaper, "Scene brightness");
               // SDR draws the UI at SDR white, so this only exists in HDR.
               if (value.values[NativeUI] == 0.f) changed |= DrawControl(value, HDRUI, "UI brightness", !state.linear_output);
            }
            // SDR white places SDR output on an HDR display; an SDR display shows its own white.
            else if (state.hdr_output)
               changed |= DrawControl(value, SDRWhite, "SDR white");
            ImGui::EndTable();
         }
         // "Game color grading" rebuilds the game's LUT. Say so when this scene's
         // LUT generator cannot be rebuilt (the slider then does nothing).
         if (mapper != Mapper::Native && !LUTBypassed(value) && !LUTNative(value))
         {
            const auto lut_status = Battlefront2NativeLUT::status.load();
            if (lut_status >= Battlefront2NativeLUT::Status::UnknownGenerator)
               Battlefront2UI::Status("%s", Battlefront2NativeLUT::StatusText(lut_status));
         }
         // Native shows the game's own output, and the game renders HDR only in its Fullscreen display mode with its
         // HDR option on (a borderless window unless exclusive fullscreen is opted into). In Borderless or Windowed
         // it renders SDR, which an HDR display shows at the Windows SDR content brightness like the unmodified game.
         if (mapper == Mapper::Native && output_hdr && !state.native_hdr_display)
            Battlefront2UI::TextColored(ImVec4(1.f, 0.68f, 0.25f, 1.f), "%s", "Native shows SDR: set the game to Fullscreen with HDR on (Video settings)");
      }
      else if (page == Page::Screenshots)
      {
         if (Battlefront2UI::BeginRows("screenshots"))
         {
            Battlefront2UI::Row("Compare tone mappers");
            ImGui::BeginDisabled(Battlefront2Compare::Busy());
            if (Battlefront2UI::Button("Save comparison")) Battlefront2Compare::Request();
            Battlefront2UI::Hint("Renders the current frame once per tone mapper and saves each image for side-by-side comparison.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (Battlefront2UI::Button("Open folder")) Battlefront2Compare::OpenFolder();
            Battlefront2UI::Hint("Opens the latest comparison in Explorer.");
            ImGui::EndTable();
         }
         const float u = ImGui::GetFontSize();
         ImGui::Indent(u * .6f);
         switch (Battlefront2Compare::status.load())
         {
         case Battlefront2Compare::Status::Pending:
         case Battlefront2Compare::Status::Saving:
            Battlefront2UI::TextDisabled("Saving comparison...");
            break;
         case Battlefront2Compare::Status::Saved:
            Battlefront2UI::TextDisabled("Comparison saved to %s", Battlefront2Compare::Folder().c_str());
            break;
         case Battlefront2Compare::Status::Failed:
            Battlefront2UI::TextDisabled("Comparison failed. Try again during gameplay.");
            break;
         default:
            break;
         }
         ImGui::Unindent(u * .6f);
      }
      else if (page == Page::ToneMapping)
      {
         if (mapper == Mapper::Native) return;
         if (Battlefront2UI::BeginRows("tone_main"))
         {
            // GT7's and ACES 2.0's curve parameters and PsychoV's gamut,
            // compression and anchors are config-only.
            changed |= DrawControl(value, ExposureField(mapper), "Exposure compensation");
            if (mapper == Mapper::PsychoV)
            {
               changed |= DrawControl(value, Contrast, "Contrast");
               changed |= DrawControl(value, Highlights, "Highlights");
               changed |= DrawControl(value, Shadows, "Shadows");
               changed |= DrawControl(value, Purity, "Color purity");
            }
            ImGui::EndTable();
         }
      }
      else if (page == Page::Effects)
      {
         if (Battlefront2UI::BeginRows("effects"))
         {
            changed |= DrawControl(value, BloomStrength, "Bloom");
            changed |= DrawControl(value, LightGamutExpansion, "Bolt and saber gamut", mapper == Mapper::Native || !output_hdr || value.values[HDRGamut] == 0.f || !state.used_dlss);
            changed |= DrawControl(value, Grain, "Film grain");
            changed |= DrawControl(value, VignetteStrength, "Vignette");
            changed |= DrawControl(value, MotionBlurEnabled, "Motion blur");
            changed |= DrawControl(value, DistortionStrength, "Screen distortion");
            ImGui::EndTable();
         }
      }
      else
      {
         if (Battlefront2UI::BeginRows("scene"))
         {
            changed |= DrawControl(value, NativeUI, "Native UI brightness");
            ImGui::EndTable();
         }
      }
      if (changed) Save(value);
   }
} // namespace Battlefront2Psycho

// Saved as BattlefrontIIDLSSMode; do not renumber.
enum class Battlefront2DLSSMode : int
{
   Off = 0,
   Auto = 1,
   Performance = 2,
   Balanced = 3,
   Quality = 4,
   UltraPerformance = 5,
   UltraQuality = 6,
   DLAA = 7,
};

namespace Battlefront2UI
{
   // The whole Luma menu. The callbacks draw the sections owned by main.cpp.
   template <class Graphics, class Effects, class Advanced, class About>
   inline void DrawPanel(const Battlefront2Psycho::State& state, Graphics graphics, Effects effects, Advanced advanced, About about)
   {
      const MenuFontScope font;
      namespace P = Battlefront2Psycho;
      const auto open = [](Category category, const char* id, const char* title, auto&& draw)
      {
         CategoryStyle colors(category);
         if (BeginSection(id, title)) draw();
         EndSection();
      };
      // Closed by default.
      const auto collapsed = [](Category category, const char* title, const char* id, auto&& draw)
      {
         CategoryStyle colors(category);
         if (!CategoryHeader(title)) return;
         if (BeginSection(id)) draw();
         EndSection();
      };
      PanelStyle style;
      if (ImGui::BeginChild("##LumaPanel", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding))
      {
         // What is on screen: Luma's tone mappers follow Output mode, Native shows the game's own output.
         const bool native = P::Snapshot().values[P::Enabled] == 0.f;
         PanelHeading(native ? state.hdr_output && state.native_hdr_display : P::HDROutput(state.hdr_output, true));
         open(Category::Graphics, "graphics_section", "Graphics", [&]
            {
               if (BeginRows("graphics"))
               {
                  graphics();
                  ImGui::EndTable();
               } });
         open(Category::Display, "display_section", "Display", [&]
            { P::DrawMenu(state, P::Page::Display); });
         if (P::MapperOf(P::Snapshot()) != P::Mapper::Native)
            collapsed(Category::Display, "Tone mapping", "tone_mapping_section", [&]
               { P::DrawMenu(state, P::Page::ToneMapping); });
         collapsed(Category::Effects, "Effects", "effects_section", [&]
            {
               P::DrawMenu(state, P::Page::Effects);
               effects(); });
         collapsed(Category::Advanced, "Advanced", "advanced_section", [&]
            {
               P::DrawMenu(state, P::Page::Advanced);
               advanced(); });
         if (Battlefront2Compare::menu)
            collapsed(Category::Advanced, "Screenshots", "screenshots_section", [&]
               { P::DrawMenu(state, P::Page::Screenshots); });
         collapsed(Category::Advanced, "About", "about_section", about);
      }
      ImGui::EndChild();
   }

   // NGX quality order; also used by the renderer when NGX has no recommendation.
   inline constexpr std::array dlss_quality_scales{.5f, .58f, 2.f / 3.f, 1.f / 3.f, .77f, 1.f};
   struct DLSSMenuOption
   {
      Battlefront2DLSSMode mode;
      const char* name;
      int quality;
   };
   inline constexpr DLSSMenuOption dlss_menu_options[]{
      {Battlefront2DLSSMode::Off, "Off", -1},
      {Battlefront2DLSSMode::Auto, "Auto", 2},
      {Battlefront2DLSSMode::UltraPerformance, "Ultra Performance", 3},
      {Battlefront2DLSSMode::Performance, "Performance", 0},
      {Battlefront2DLSSMode::Balanced, "Balanced", 1},
      {Battlefront2DLSSMode::Quality, "Quality", 2},
      {Battlefront2DLSSMode::UltraQuality, "Ultra Quality", 4},
      {Battlefront2DLSSMode::DLAA, "DLAA", 5},
   };
   inline std::string DLSSModeLabel(const DLSSMenuOption& option, float native_scale)
   {
      std::string label = option.name;
      if (option.mode == Battlefront2DLSSMode::Auto) label += " / Quality";
      const float scale = option.quality < 0 ? native_scale : dlss_quality_scales[option.quality];
      if (scale > 0.f)
      {
         char percent[32];
         std::snprintf(percent, sizeof(percent), " (%.0f%%)", scale * 100.f);
         label += percent;
      }
      return label;
   }
   inline bool DrawDLSSQuality(int& selected_mode, bool available, float native_scale)
   {
      const auto previous = selected_mode;
      const auto* selected = &dlss_menu_options[0];
      for (const auto& option : dlss_menu_options)
         if (int(option.mode) == selected_mode) selected = &option;
      Row("DLSS quality");
      if (BeginCombo("##dlss", DLSSModeLabel(*selected, native_scale).c_str()))
      {
         for (const auto& option : dlss_menu_options)
         {
            const bool enabled = option.mode == Battlefront2DLSSMode::Off || available;
            ImGui::BeginDisabled(!enabled);
            const bool active = int(option.mode) == selected_mode;
            const auto label = DLSSModeLabel(option, native_scale);
            // The ID stays stable when the game's resolution scale changes the label.
            const auto widget = std::string(MenuText(label.c_str())) + "###dlss-mode-" + std::to_string(int(option.mode));
            if (ImGui::Selectable(widget.c_str(), active) && enabled) selected_mode = int(option.mode);
            if (active) ImGui::SetItemDefaultFocus();
            ImGui::EndDisabled();
         }
         ImGui::EndCombo();
      }
      Battlefront2UI::Hint("Percentage of output width and height. Lower values favor performance.");
      return selected_mode != previous;
   }
} // namespace Battlefront2UI

namespace Battlefront2SSR
{
   inline std::mutex settings_mutex;
   inline Settings user_settings;
   inline Settings Snapshot()
   {
      std::lock_guard lock(settings_mutex);
      return user_settings;
   }
   inline void Save(const Settings& s)
   {
      Config::Set(NAME, "BattlefrontIISSRNativeProfileVersion", 1u);
      Config::Set(NAME, "BattlefrontIISSRVelocityTemporal", s.velocity_temporal);
      Config::Set(NAME, "BattlefrontIISSRSpatialFilter", s.spatial_filter);
      Config::Set(NAME, "BattlefrontIISSRFullResolution", s.full_resolution);
      Config::Set(NAME, "BattlefrontIISSRFilteredUpsampling", s.filtered_upsampling);
      Config::Set(NAME, "BattlefrontIISSRThicknessScale", s.thickness);
      Config::Set(NAME, "BattlefrontIISSRResolveSamplesScale", s.resolve_samples);
      Config::Set(NAME, "BattlefrontIISSRImportanceScale", s.importance);
      Config::Set(NAME, "BattlefrontIISSRResponsivenessScale", s.responsiveness);
   }
   inline void Load()
   {
      Settings s;
      unsigned native_profile_version = 0;
      Config::Get(NAME, "BattlefrontIISSRNativeProfileVersion", native_profile_version);
      Config::Get(NAME, "BattlefrontIISSRVelocityTemporal", s.velocity_temporal);
      Config::Get(NAME, "BattlefrontIISSRSpatialFilter", s.spatial_filter);
      Config::Get(NAME, "BattlefrontIISSRFullResolution", s.full_resolution);
      Config::Get(NAME, "BattlefrontIISSRFilteredUpsampling", s.filtered_upsampling);
      Config::Get(NAME, "BattlefrontIISSRThicknessScale", s.thickness);
      Config::Get(NAME, "BattlefrontIISSRResolveSamplesScale", s.resolve_samples);
      Config::Get(NAME, "BattlefrontIISSRImportanceScale", s.importance);
      Config::Get(NAME, "BattlefrontIISSRResponsivenessScale", s.responsiveness);
      const bool migrate = native_profile_version != 1 || !s.NativeValid();
      s = NativeProfile(s, native_profile_version);
      if (migrate) Save(s);
      std::lock_guard lock(settings_mutex);
      user_settings = s;
   }
   inline void DrawMenu()
   {
      Settings s = Snapshot();
      if (!Battlefront2UI::BeginRows("native-ssr-tuning")) return;
      Battlefront2UI::Row("Full-resolution reflections");
      const bool changed = Battlefront2UI::Checkbox("##Full-resolution reflections", &s.full_resolution);
      Battlefront2UI::Hint("Sharper reflections at a performance cost.");
      ImGui::EndTable();
      if (changed)
      {
         {
            std::lock_guard lock(settings_mutex);
            user_settings = s;
         }
         Save(s);
      }
   }
} // namespace Battlefront2SSR

namespace Battlefront2XeGTAO
{
   inline std::mutex settings_mutex;
   struct Settings
   {
      bool enabled = true;            // XeGTAO or VB-GTAO replaces the game's AO
      bool visibility_bitmask = true; // VB-GTAO
      float radius = 1.f;
      unsigned quality = 1;
   };
   // "BattlefrontIIAOMethod": 0 Native, 1 XeGTAO, 3 VB-GTAO. 2 was a Luma "Off", which now loads as Native: the game's
   // own Ambient Occlusion setting turns AO off.
   inline int Method(const Settings& s)
   {
      return !s.enabled ? 0 : s.visibility_bitmask ? 3
                                                   : 1;
   }
   inline Settings Snapshot()
   {
      std::lock_guard lock(settings_mutex);
      return {enabled.load(), visibility_bitmask.load(), radius_scale.load(), quality.load()};
   }
   inline void Store(const Settings& s)
   {
      std::lock_guard lock(settings_mutex);
      enabled = s.enabled;
      visibility_bitmask = s.visibility_bitmask;
      radius_scale = s.radius;
      quality = s.quality;
   }
   // Radius and quality are config-only.
   inline void Load()
   {
      Settings s;
      const bool saved = Config::Get(NAME, "BattlefrontIIXeGTAOEnabled", s.enabled);
      int method = saved ? (s.enabled ? 1 : 0) : 3;
      Config::Get(NAME, "BattlefrontIIAOMethod", method);
      if (method >= 0 && method <= 3)
      {
         s.enabled = method == 1 || method == 3;
         s.visibility_bitmask = method == 3;
      }
      Config::Get(NAME, "BattlefrontIIXeGTAORadiusScale", s.radius);
      Config::Get(NAME, "BattlefrontIIXeGTAOQuality", s.quality);
      // An invalid value resets only itself, never the saved method.
      if (!std::isfinite(s.radius) || s.radius < .5f || s.radius > 2.f) s.radius = 1;
      if (s.quality > 1) s.quality = 1;
      Store(s);
   }
   inline void DrawControl()
   {
      auto s = Snapshot();
      // Menu order: Native, XeGTAO, VB-GTAO.
      int selected = !s.enabled ? 0 : s.visibility_bitmask ? 2
                                                           : 1;
      Battlefront2UI::Row("Ambient occlusion");
      bool changed = Battlefront2UI::Combo("##ao", &selected, "Native\0XeGTAO\0VB-GTAO\0");
      Battlefront2UI::Hint("Shading in corners and where objects meet. The game's own Ambient Occlusion setting turns it off.");
      if (Battlefront2UI::ResetItem(selected == 2, false))
      {
         selected = 2;
         changed = true;
      }
      if (!changed) return;
      s.enabled = selected != 0;
      s.visibility_bitmask = selected == 2;
      Store(s);
      Config::Set(NAME, "BattlefrontIIXeGTAOEnabled", s.enabled);
      Config::Set(NAME, "BattlefrontIIAOMethod", Method(s));
      Config::Set(NAME, "BattlefrontIIXeGTAORadiusScale", s.radius);
      Config::Set(NAME, "BattlefrontIIXeGTAOQuality", s.quality);
   }
} // namespace Battlefront2XeGTAO
