#ifndef XFONT_EDITOR_PREVIEW_H
#define XFONT_EDITOR_PREVIEW_H
#pragma once

// The Font editor's preview: the compiled atlas as a zoomable picture, and live text (any string, laid out with the font's own advances and
// kerning, drawn through the MSDF shader with the effects: outline, bold, shadow, bevel, glow, italic) rendered into an offscreen texture.
// Draw runs before the frame's UI is rendered (it opens its own render pass on the window); the panel then shows the texture.
#include "source/xGPU.h"
#include "dependencies/xproperty/source/xcore/my_properties.h"
#include "dependencies/xproperty/source/examples/imgui/xPropertyImGuiInspector.h"
#include "source/tools/xgpu_xcore_bitmap_helpers.h"
#include "plugins/xfont.plugin/source/xfont_rsc_descriptor.h"
#include "plugins/xfont.plugin/source/xfont_xgpu_rsc_loader.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace xfont_editor
{
// Compiled SPIR-V for the MSDF text-preview shader pair - generated at build time from the two
// .glsl files sitting next to this .cpp (see the top-level CMakeLists.txt's FRAG/VERT_SHADER_SOURCES
// lists), same mechanism every other example's shader pair uses (e.g. E22_FramebufferTarget's
// draw_vert.h/draw_frag.h).
//
// Declared as std::uint32_t[] (NOT std::array{...} CTAD) - each generated .h is a flat list of plain
// hex word literals, and a SPIR-V word's top bit is legitimately set whenever the shader has ANY
// negative float constant (its IEEE-754 bit pattern has the sign bit on) - a hex literal like that is
// typed `unsigned int` by the language, not `int`. std::array{...}'s CTAD locks onto the FIRST
// literal's type (`int`, since every header starts with the small 0x07230203 magic number), so the
// moment a later word needs the sign bit, list-initialization fails as a narrowing conversion. This
// bit me for real adding E28_msdf_frag.glsl's kBevelLightDir = vec2(-1.0, -0.5). uint32_t sidesteps
// it entirely: every 32-bit literal - whichever of int/unsigned int the compiler picked for it -
// converts to uint32_t without narrowing, since the VALUE always fits.
inline std::span<const std::int32_t> AsShaderSpan(const std::uint32_t* pWords, std::size_t Count) noexcept
{
    // xgpu::shader::setup::raw_data wants int32_t specifically - reinterpret, not convert, since
    // these are raw bits (SPIR-V words), not values with sign semantics.
    return { reinterpret_cast<const std::int32_t*>(pWords), Count };
}
inline constexpr std::uint32_t g_MsdfVertSPVWords[] =
{
    #include "E28_msdf_vert.h"
};
inline constexpr std::uint32_t g_MsdfFragSPVWords[] =
{
    #include "E28_msdf_frag.h"
};
// Dedicated shader pair for the glyph-bounds debug overlay - see E28_wire_vert.glsl's own comment.
inline constexpr std::uint32_t g_WireVertSPVWords[] =
{
    #include "E28_wire_vert.h"
};
inline constexpr std::uint32_t g_WireFragSPVWords[] =
{
    #include "E28_wire_frag.h"
};

    //---------------------------------------------------------------------------

    // Per-panel pan/zoom state for ShowZoomableImage - mirrors E10_TextureResourcePipeline's own 2D
    // viewer (m_2DMouseScale/m_2DMouseTranslate: wheel zooms toward the cursor, drag pans, a
    // "Recenter" button resets pan only). Two independent instances are kept - one for the raw
    // Texture preview, one for Live Text - so zooming into the atlas doesn't also zoom the text.
    struct pan_zoom
    {
        float        m_Zoom { 1.0f };
        xmath::fvec2 m_Pan  { 0.0f, 0.0f };
    };

    // Shows an already-rendered texture inside a scrollable/zoomable canvas: mouse wheel zooms
    // toward the cursor, left-drag pans, "Recenter" resets pan back to centered (zoom untouched) -
    // same interaction model as the Texture Editor's own 2D preview. TexW/TexH are the pixel
    // dimensions the image should be drawn at, at Zoom=1 (UVMin/UVMax let the caller show a sub-rect,
    // e.g. text_renderer's scene texture, which is bucket-snapped larger than its actual content).
    // pOverlayRectsPx (optional): each ImVec4{x,y,w,h} is one glyph's packed atlas rect, in the SAME
    // texture-pixel space as TexW/TexH (i.e. relative to the full un-cropped texture, matching
    // UVMin=0/UVMax=1's own space) - drawn as red outlines using the same pan/zoom transform as the
    // image itself, so they land exactly on the glyph they describe at any zoom. Lets RenderSettings/
    // Debug/ShowGlyphBounds double as a packer-overlap check: two rects visibly overlapping here means
    // the atlas packer placed them on top of each other, not a LayoutText/shader bug.
    inline void ShowZoomableImage(const char* pID, void* pTextureHandle, int TexW, int TexH, pan_zoom& View, ImVec2 UVMin = ImVec2(0, 0), ImVec2 UVMax = ImVec2(1, 1), const std::vector<ImVec4>* pOverlayRectsPx = nullptr)
    {
        if (pTextureHandle == nullptr || TexW <= 0 || TexH <= 0)
        {
            ImGui::TextDisabled("(not available)");
            return;
        }

        ImGui::PushID(pID);
        ImGui::Text("%d x %d", TexW, TexH);
        ImGui::SameLine();
        if (ImGui::SmallButton("Recenter")) View.m_Pan = { 0.0f, 0.0f };
        ImGui::SameLine();
        ImGui::Text("Zoom:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        ImGui::DragFloat("##Zoom", &View.m_Zoom, 0.01f, 0.05f, 40.0f, "%.2fx");

        ImGui::BeginChild("##canvas", ImGui::GetContentRegionAvail(), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove);

        const ImVec2 CanvasP0 = ImGui::GetCursorScreenPos();
        // InvisibleButton below asserts on a zero-size argument - GetContentRegionAvail() can
        // legitimately be (0,0) here for a frame or two (window collapsed, still settling right after
        // a font reload swaps which preview windows are visible) so clamp rather than pass it through raw.
        const ImVec2 Avail        = ImGui::GetContentRegionAvail();
        const ImVec2 CanvasSize   = ImVec2(std::max(Avail.x, 1.0f), std::max(Avail.y, 1.0f));
        const ImVec2 CanvasCenter = ImVec2(CanvasP0.x + CanvasSize.x * 0.5f, CanvasP0.y + CanvasSize.y * 0.5f);

        ImGui::InvisibleButton("##canvas_btn", CanvasSize);
        const bool bHovered = ImGui::IsItemHovered();

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            const ImVec2 Delta = ImGui::GetIO().MouseDelta;
            View.m_Pan.m_X += Delta.x;
            View.m_Pan.m_Y += Delta.y;
        }

        if (bHovered && ImGui::GetIO().MouseWheel != 0.0f)
        {
            const float OldZoom = View.m_Zoom;
            View.m_Zoom = std::clamp(View.m_Zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.1f), 0.05f, 40.0f);

            // Zoom toward the cursor, not the canvas center - keep whatever point is currently under
            // the mouse fixed on screen by rescaling its offset from the pan origin by the same ratio.
            const ImVec2 Mouse = ImGui::GetIO().MousePos;
            const ImVec2 MouseRelCenter { Mouse.x - CanvasCenter.x, Mouse.y - CanvasCenter.y };
            const float  Ratio = View.m_Zoom / OldZoom;
            View.m_Pan.m_X = MouseRelCenter.x - (MouseRelCenter.x - View.m_Pan.m_X) * Ratio;
            View.m_Pan.m_Y = MouseRelCenter.y - (MouseRelCenter.y - View.m_Pan.m_Y) * Ratio;
        }

        const float DrawW = TexW * View.m_Zoom;
        const float DrawH = TexH * View.m_Zoom;
        const ImVec2 ImgMin(CanvasCenter.x + View.m_Pan.m_X - DrawW * 0.5f, CanvasCenter.y + View.m_Pan.m_Y - DrawH * 0.5f);
        const ImVec2 ImgMax(ImgMin.x + DrawW, ImgMin.y + DrawH);
        ImGui::GetWindowDrawList()->AddImage(pTextureHandle, ImgMin, ImgMax, UVMin, UVMax);

        if (pOverlayRectsPx != nullptr)
        {
            auto* pDrawList = ImGui::GetWindowDrawList();
            for (const auto& R : *pOverlayRectsPx)
            {
                const ImVec2 RMin(ImgMin.x + R.x * View.m_Zoom, ImgMin.y + R.y * View.m_Zoom);
                const ImVec2 RMax(ImgMin.x + (R.x + R.z) * View.m_Zoom, ImgMin.y + (R.y + R.w) * View.m_Zoom);
                pDrawList->AddRect(RMin, RMax, IM_COL32(255, 0, 0, 255));
            }
        }

        ImGui::EndChild();
        ImGui::PopID();
    }

    //---------------------------------------------------------------------------

    // Every glyph's packed atlas rect, in texture-pixel space - for the Atlas view's own
    // RenderSettings/Debug/ShowGlyphBounds overlay (see ShowZoomableImage's own comment on why: two
    // rects visibly overlapping here is a packer bug, not a rendering one). BITMAP has its glyphs
    // split across size_groups with no single flat array; MTSDF/SDF already expose one via Glyphs().
    inline std::vector<ImVec4> CollectGlyphAtlasRects(const xfont_rsc::font& Font)
    {
        std::vector<ImVec4> Rects;
        auto AddIfInk = [&](const xfont_rsc::glyph& G)
        {
            if (G.m_AtlasW > 0 && G.m_AtlasH > 0)
                Rects.emplace_back(static_cast<float>(G.m_AtlasX), static_cast<float>(G.m_AtlasY), static_cast<float>(G.m_AtlasW), static_cast<float>(G.m_AtlasH));
        };

        if (Font.m_OutputType == xfont_rsc::output_type::BITMAP)
        {
            const char* pRegionStart = Font.SizeGroupRegionStart();
            for (std::uint32_t g = 0; g < Font.m_nSizeGroups; ++g)
            {
                const auto& Group  = Font.SizeGroups()[g];
                const auto* pGlyph = xfont_rsc::font::GlyphsInSizeGroup(pRegionStart, Group);
                for (std::uint32_t i = 0; i < Group.m_nGlyphs; ++i) AddIfInk(pGlyph[i]);
            }
        }
        else
        {
            const auto* pGlyph = Font.Glyphs();
            for (std::uint32_t i = 0; i < Font.m_nGlyphs; ++i) AddIfInk(pGlyph[i]);
        }
        return Rects;
    }

    //---------------------------------------------------------------------------

    // Editor-wide preview controls - NOT per-font asset state (unlike font_state::m_Descriptor),
    // so binding this inspector once at startup is safe: it never transitions from an empty/
    // default object to a later-populated one the way an unselected font_state::m_Descriptor did,
    // and its own property paths ("RenderSettings/...") are unique to this struct, so there's no
    // cross-binding cache collision risk either (see LoadFont's own comment on that bug).
    struct render_settings
    {
        // Mirrors the CURRENTLY SELECTED font's own OutputType - render_settings itself has no idea
        // which font is selected (that's tracked in font_state, outside this struct - see this
        // struct's own top comment), but several Effects below only make sense for a real distance
        // field (BITMAP is plain rasterized coverage, no signed distance to threshold-shift or take a
        // gradient of) - the main loop sets this once per frame before the inspector renders, purely
        // so THIS struct's own dynamic_flags lambdas (which only ever see a render_settings&) have
        // something to check.
        xfont_rsc::output_type m_CurrentFontOutputType{ xfont_rsc::output_type::MTSDF };

        std::string m_Text            { "Hello, World!" };
        // The px-per-em size Live Text renders at before the view's own pan/zoom multiplies it - shared
        // across every OutputType so switching between them at the same Zoom keeps the SAME on-screen
        // text size (previously MTSDF/SDF used a hardcoded 64px reference while BITMAP used this
        // property on its own default of 16px, so an identical Zoom value produced a ~4x size jump and
        // a shifted position when switching output types - this unifies them onto one control). For
        // BITMAP specifically this still snaps to whichever baked size group is closest to the
        // requested value (see xfont_rsc::font::FindClosestSizeGroup) since BITMAP can only render its
        // actually-compiled pixel sizes; MTSDF/SDF are true distance fields and use the value directly.
        float       m_PreviewTextSize{ 16.0f };
        bool        m_bShowOutline    { false };  // only meaningful when the selected font's SDF companion exists (StoreSDF was on at compile time)
        float       m_OutlineWidth    { 0.08f };  // em units
        bool        m_bBold           { false };  // synthetic bold - shifts the fill threshold, no separate bold glyph data needed
        float       m_FontWeight      { 0.03f };  // em-equivalent units (converted to screen px the same way OutlineWidth is)
        bool        m_bShowShadow     { false };  // drop shadow - redraws the same real glyph mesh translated, then the normal fill on top
        float       m_ShadowOffsetX   { 0.04f };  // em units
        float       m_ShadowOffsetY   { -0.05f }; // em units
        bool        m_bBevel          { false };  // pseudo-lit edge bevel, from the SDF's own screen-space gradient - MTSDF/SDF only, no-op on BITMAP
        float       m_BevelWeight     { 0.06f };  // em-equivalent units (converted to screen px the same way OutlineWidth is)
        bool        m_bGlow           { false };  // soft colored halo fading outward from the edge - see https://www.redblobgames.com/articles/sdf-fonts/'s own "glow" section; MTSDF/SDF only, no-op on BITMAP (same reasoning as Bevel/Outline - no distance field outside the glyph to fade from)
        float       m_GlowRadius      { 0.15f };  // em-equivalent units (converted to screen px the same way OutlineWidth is) - how far outward the halo reaches before fading to nothing
        float       m_GlowIntensity   { 0.8f };   // 0-1 opacity multiplier; color itself is a shader constant (kGlowColor in E28_msdf_frag.glsl), same precedent as the shadow's own fixed color
        bool        m_bItalic          { false }; // synthetic italic - vertex-level shear (real skew, not a UV trick), see E28_msdf_vert.glsl
        float       m_ItalicShear      { 0.2f };  // slope (dx per unit y), dimensionless
        bool        m_bShowGlyphBounds{ false };  // debug: draws the real glyph mesh in red wireframe, on top of the rendered text

        XPROPERTY_DEF
        ( "RenderSettings", render_settings
        , obj_member<"Text",  &render_settings::m_Text,  member_help<"The string to render in the Font Preview window.">>
        , obj_member<"PreviewTextSize", &render_settings::m_PreviewTextSize
            , member_ui<float>::drag_bar<0.5f, 4.0f, 256.0f>
            , member_help<"Live Text's px-per-em size at Zoom 1x, before the view's own pan/zoom. BITMAP fonts bake fixed pixel sizes, so this snaps to whichever was actually compiled closest to the requested value; MTSDF/SDF render at the exact value.">>
        , obj_scope<"Effects"
            // ShowOutline/Bold/Bevel all need a real distance field to threshold/shade from - BITMAP
            // is plain rasterized alpha coverage, so the shader's own BITMAP branch returns before any
            // of them would apply (see E28_msdf_frag.glsl) - hidden here to match, rather than leaving
            // a toggle that silently does nothing. Shadow and Italic DO still work on BITMAP (shadow
            // just resamples the same real alpha coverage translated; italic is a pure vertex shear) -
            // left visible/functional for it.
            , obj_member<"ShowOutline",  &render_settings::m_bShowOutline, member_help<"Draws an outline using the font's companion SDF texture - only available when the selected font's StoreSDF was on at compile time.">
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>>
            , obj_member<"OutlineWidth", &render_settings::m_OutlineWidth
                // drag_bar's template order is <Speed, Min, Max> - the old <0.0f, 0.5f> actually meant
                // Speed=0 (frozen drag) and Min=0.5 (floor), not Min=0/Max=0.5 as intended. See
                // xfont_rsc_descriptor.h's PixelRange/AngleThreshold/GlyphSize for the same fix.
                , member_ui<float>::drag_bar<0.005f, 0.0f, 0.5f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bShowOutline || O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>
                , member_help<"Outline thickness, in em units.">>
            , obj_member<"Bold", &render_settings::m_bBold, member_help<"Synthetic bold - thickens the fill by shifting the SDF threshold, no separate bold font needed.">
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>>
            , obj_member<"FontWeight", &render_settings::m_FontWeight
                , member_ui<float>::drag_bar<0.005f, -0.1f, 0.2f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bBold || O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>
                , member_help<"Bold amount, in em units - positive thickens, negative thins.">>
            , obj_member<"ShowShadow", &render_settings::m_bShowShadow, member_help<"Draws a drop shadow by redrawing the same glyph mesh translated behind the normal fill - correctly shaped/antialiased since it samples the real glyph SDF (or, for BITMAP, the real alpha coverage), just offset.">>
            , obj_member<"ShadowOffsetX", &render_settings::m_ShadowOffsetX
                , member_ui<float>::drag_bar<0.005f, -0.3f, 0.3f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bShowShadow; return F; }>
                , member_help<"Shadow offset, in em units (+X = right).">>
            , obj_member<"ShadowOffsetY", &render_settings::m_ShadowOffsetY
                , member_ui<float>::drag_bar<0.005f, -0.3f, 0.3f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bShowShadow; return F; }>
                , member_help<"Shadow offset, in em units (+Y = up).">>
            , obj_member<"Bevel", &render_settings::m_bBevel, member_help<"Pseudo-lit edge bevel, shaded from the SDF's own screen-space gradient - MTSDF/SDF only, no effect on BITMAP fonts (no distance field to shade from).">
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>>
            , obj_member<"BevelWeight", &render_settings::m_BevelWeight
                , member_ui<float>::drag_bar<0.005f, 0.0f, 0.3f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bBevel || O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>
                , member_help<"How far the bevel band reaches inward from the edge, in em units.">>
            , obj_member<"Glow", &render_settings::m_bGlow, member_help<"Soft colored halo fading outward from the glyph edge - MTSDF/SDF only, no effect on BITMAP fonts (no distance field outside the glyph to fade from).">
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>>
            , obj_member<"GlowRadius", &render_settings::m_GlowRadius
                , member_ui<float>::drag_bar<0.005f, 0.0f, 0.6f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bGlow || O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>
                , member_help<"How far the glow halo reaches outward from the edge before fading to nothing, in em units.">>
            , obj_member<"GlowIntensity", &render_settings::m_GlowIntensity
                , member_ui<float>::drag_bar<0.005f, 0.0f, 1.0f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bGlow || O.m_CurrentFontOutputType == xfont_rsc::output_type::BITMAP; return F; }>
                , member_help<"Glow opacity multiplier.">>
            , obj_member<"Italic", &render_settings::m_bItalic, member_help<"Synthetic italic - shears the actual glyph geometry (not just the texture sampling), no separate italic font needed.">>
            , obj_member<"ItalicShear", &render_settings::m_ItalicShear
                , member_ui<float>::drag_bar<0.005f, -0.6f, 0.6f>
                , member_dynamic_flags<+[](const render_settings& O) { xproperty::flags::type F{}; F.m_bDontShow = !O.m_bItalic; return F; }>
                , member_help<"Shear slope - how far X shifts per unit Y (positive leans right at the top).">>
            >
        , obj_scope<"Debug"
            , obj_member<"ShowGlyphBounds", &render_settings::m_bShowGlyphBounds, member_help<"Draws the actual triangles used to render each glyph, in red wireframe, on top of the rendered text - useful for spotting atlas clamping, UV bleeding, and padding issues.">>
            >
        )
    };
    XPROPERTY_REG(render_settings)

    //---------------------------------------------------------------------------

    // Decodes a UTF-8 std::string into an ORDERED codepoint sequence (unlike the compiler's own
    // CollectCodepointsFromUtf8File, which collects a unique SET - here order and repeats matter).
    inline std::vector<std::uint32_t> DecodeUtf8(const std::string& Text)
    {
        std::vector<std::uint32_t> Out;
        std::size_t i = 0;
        while (i < Text.size())
        {
            const unsigned char c0 = static_cast<unsigned char>(Text[i]);
            std::uint32_t Cp; int Len;
            if (c0 < 0x80)                                     { Cp = c0;        Len = 1; }
            else if ((c0 & 0xE0) == 0xC0 && i + 1 < Text.size()) { Cp = c0 & 0x1F; Len = 2; }
            else if ((c0 & 0xF0) == 0xE0 && i + 2 < Text.size()) { Cp = c0 & 0x0F; Len = 3; }
            else if ((c0 & 0xF8) == 0xF0 && i + 3 < Text.size()) { Cp = c0 & 0x07; Len = 4; }
            else { ++i; continue; }

            bool bValid = true;
            for (int k = 1; k < Len; ++k)
            {
                const unsigned char Ck = static_cast<unsigned char>(Text[i + k]);
                if ((Ck & 0xC0) != 0x80) { bValid = false; break; }
                Cp = (Cp << 6) | (Ck & 0x3F);
            }
            if (bValid) Out.push_back(Cp);
            i += bValid ? static_cast<std::size_t>(Len) : 1;
        }
        return Out;
    }

    // One glyph quad ready for the GPU: NDC-agnostic local-space corners (em units, pen-relative,
    // Y-up) and the matching UV rect (normalized [0,1], atlas pixel bounds divided by atlas size).
    struct text_quad
    {
        xmath::fvec2 m_Min, m_Max;
        xmath::fvec2 m_UVMin, m_UVMax;
        // Size of ONE atlas texel, in em-units, for this specific glyph (X/Y separately, though
        // they're normally equal) - computed once here in LayoutText, where the glyph's own
        // AtlasW/H is at hand, so callers like Draw()'s debug wireframe can inset by "1 texel" without
        // needing to re-derive it from UV span + a runtime texture-dimension query.
        xmath::fvec2 m_TexelEm;
    };

    // Standard advance/bearing/kerning text layout (TextMeshPro's own model, nothing MSDF-specific -
    // see xfont_rsc_descriptor.h's own top comment) - walks Text, looks up each codepoint via the
    // font's perfect hash, accumulates the pen position, and emits one quad per non-whitespace glyph.
    // AtlasWidth/AtlasHeight convert the glyph's pixel-space atlas bounds into normalized UVs.
    // RequestedPixelSize (only consulted for BITMAP fonts - ignored otherwise) is the caller's own
    // render_settings::m_PreviewTextSize - used to automatically pick whichever baked size_group is
    // closest, same automatic-by-OutputType behavior the fill/outline shader branch already has, just
    // for glyph lookup instead of sampling.
    inline void LayoutText(const xfont_rsc::font& Font, int AtlasWidth, int AtlasHeight, const std::string& Text, std::vector<text_quad>& OutQuads, float& OutAdvance, float RequestedPixelSize)
    {
        OutQuads.clear();
        OutAdvance = 0.0f;
        if (AtlasWidth <= 0 || AtlasHeight <= 0) return;

        const auto Codepoints = DecodeUtf8(Text);
        float PenX = 0.0f;
        std::uint32_t PrevCodepoint = 0;

        if (Font.m_OutputType == xfont_rsc::output_type::BITMAP)
        {
            const auto* pGroup = Font.FindClosestSizeGroup(RequestedPixelSize);
            if (pGroup == nullptr || pGroup->m_nGlyphs == 0) return;
            const char* pRegionStart = Font.SizeGroupRegionStart();
            // The compiler already normalizes each glyph's plane bounds/advance by this group's own
            // baked pixel size before packing them into fixed-point (see xfont_compiler.cpp's own
            // comment) - so FromFixed() alone yields the same "fraction of an em" units MTSDF/SDF
            // already use, no further scaling needed here. Draw()'s own PxPerEm multiplies back up to
            // screen pixels afterward, same as MTSDF/SDF, so Zoom behaves consistently across every
            // output type even though BITMAP itself can't truly scale (it just snaps to whichever
            // baked size is closest, via FindClosestSizeGroup above).
            for (auto Cp : Codepoints)
            {
                const auto* pGlyph = xfont_rsc::font::FindGlyphInSizeGroup(pRegionStart, *pGroup, Cp);
                if (pGlyph == nullptr) continue; // missing glyph - skip, no tofu box baked (yet); no kerning for BITMAP

                if (pGlyph->m_AtlasW > 0 && pGlyph->m_AtlasH > 0)
                {
                    text_quad Q;
                    const float PLeft   = xfont_rsc::FromFixed(pGlyph->m_PlaneLeft);
                    const float PRight  = xfont_rsc::FromFixed(pGlyph->m_PlaneRight);
                    const float PBottom = xfont_rsc::FromFixed(pGlyph->m_PlaneBottom);
                    const float PTop    = xfont_rsc::FromFixed(pGlyph->m_PlaneTop);
                    // Extend half a texel OUTWARD on every edge - see the MTSDF/SDF branch below for
                    // why (same fix, needed here too since BITMAP's own coverage alpha is sampled the
                    // same way). Geometry grows by the same half-texel, converted to em-units via this
                    // glyph's own texel-per-em ratio, so the extra sampled fringe actually becomes
                    // visible extra pixels rather than being squeezed into the original box (which
                    // would just resample/blur, not reveal anything new).
                    const float HalfTexelEmX = pGlyph->m_AtlasW > 0 ? 0.5f * (PRight - PLeft) / pGlyph->m_AtlasW : 0.0f;
                    const float HalfTexelEmY = pGlyph->m_AtlasH > 0 ? 0.5f * (PTop - PBottom) / pGlyph->m_AtlasH : 0.0f;
                    Q.m_Min = xmath::fvec2{ PenX + PLeft - HalfTexelEmX, PBottom - HalfTexelEmY };
                    Q.m_Max = xmath::fvec2{ PenX + PRight + HalfTexelEmX, PTop + HalfTexelEmY };
                    Q.m_UVMin = xmath::fvec2{ (pGlyph->m_AtlasX - 0.5f) / static_cast<float>(AtlasWidth), (pGlyph->m_AtlasY + pGlyph->m_AtlasH + 0.5f) / static_cast<float>(AtlasHeight) };
                    Q.m_UVMax = xmath::fvec2{ (pGlyph->m_AtlasX + pGlyph->m_AtlasW + 0.5f) / static_cast<float>(AtlasWidth), (pGlyph->m_AtlasY - 0.5f) / static_cast<float>(AtlasHeight) };
                    Q.m_TexelEm = xmath::fvec2{ HalfTexelEmX * 2.0f, HalfTexelEmY * 2.0f };
                    OutQuads.push_back(Q);
                }
                PenX += xfont_rsc::FromFixed(pGlyph->m_Advance);
            }
            OutAdvance = PenX;
            return;
        }

        for (std::size_t i = 0; i < Codepoints.size(); ++i)
        {
            const auto Cp = Codepoints[i];
            const auto* pGlyph = Font.FindGlyph(Cp);
            if (pGlyph == nullptr) { PrevCodepoint = 0; continue; } // missing glyph - skip, no tofu box baked (yet)

            if (i > 0 && PrevCodepoint != 0)
                PenX += xfont_rsc::FromFixed(Font.FindKernAdjust(PrevCodepoint, Cp));

            if (pGlyph->m_AtlasW > 0 && pGlyph->m_AtlasH > 0) // whitespace glyphs carry no ink - advance only, no quad
            {
                text_quad Q;
                const float PLeft   = xfont_rsc::FromFixed(pGlyph->m_PlaneLeft);
                const float PRight  = xfont_rsc::FromFixed(pGlyph->m_PlaneRight);
                const float PBottom = xfont_rsc::FromFixed(pGlyph->m_PlaneBottom);
                const float PTop    = xfont_rsc::FromFixed(pGlyph->m_PlaneTop);
                // Extend half a texel OUTWARD on every edge, geometry and UV together - sampling
                // exactly at a texel BOUNDARY (the un-extended math) let the GPU's bilinear filter
                // blend 50/50 with whatever sits just outside this glyph's own rect (padding, or a
                // neighbor), diluting the outermost row/column of real coverage toward transparent -
                // most visible as "the edge looks cut off" at high zoom. Growing the quad by the same
                // half-texel (converted to em-units via this glyph's own texel-per-em ratio) means that
                // extra sampled fringe becomes actual extra visible pixels instead of being squeezed
                // into the original box, which would just resample/blur rather than fix anything.
                const float HalfTexelEmX = pGlyph->m_AtlasW > 0 ? 0.5f * (PRight - PLeft) / pGlyph->m_AtlasW : 0.0f;
                const float HalfTexelEmY = pGlyph->m_AtlasH > 0 ? 0.5f * (PTop - PBottom) / pGlyph->m_AtlasH : 0.0f;
                Q.m_Min = xmath::fvec2{ PenX + PLeft - HalfTexelEmX, PBottom - HalfTexelEmY };
                Q.m_Max = xmath::fvec2{ PenX + PRight + HalfTexelEmX, PTop + HalfTexelEmY };
                Q.m_UVMin = xmath::fvec2{ (pGlyph->m_AtlasX - 0.5f) / static_cast<float>(AtlasWidth), (pGlyph->m_AtlasY + pGlyph->m_AtlasH + 0.5f) / static_cast<float>(AtlasHeight) };
                Q.m_UVMax = xmath::fvec2{ (pGlyph->m_AtlasX + pGlyph->m_AtlasW + 0.5f) / static_cast<float>(AtlasWidth), (pGlyph->m_AtlasY - 0.5f) / static_cast<float>(AtlasHeight) };
                Q.m_TexelEm = xmath::fvec2{ HalfTexelEmX * 2.0f, HalfTexelEmY * 2.0f };
                OutQuads.push_back(Q);
            }

            PenX += xfont_rsc::FromFixed(pGlyph->m_Advance);
            PrevCodepoint = Cp;
        }

        OutAdvance = PenX;
    }

    //---------------------------------------------------------------------------

    struct msdf_vert
    {
        float m_X, m_Y, m_U, m_V;
    };

    // Layout must match E28_msdf_vert.glsl/E28_msdf_frag.glsl's own PC block exactly (field order,
    // no vec3/mat fields so the default push_constant packing needs no manual padding).
    struct msdf_push_constants
    {
        xmath::fvec2   m_Scale;
        xmath::fvec2   m_Translate;
        float          m_PixelRange;
        std::uint32_t  m_Color;
        std::uint32_t  m_bOutline;
        std::uint32_t  m_OutlineColor;
        float          m_OutlineWidthPx;
        std::uint32_t  m_OutputType; // mirrors xfont_rsc::output_type: 0=MTSDF, 1=SDF, 2=BITMAP
        float          m_FontWeightPx;
        float          m_BevelWeightPx;
        float          m_GlowRadiusPx;   // 0 = off; else how far the soft glow halo extends outward, in screen px
        float          m_GlowIntensity;  // 0-1, glow opacity multiplier (color itself is a shader constant, same precedent as the fill/outline/shadow colors below)
        float          m_ItalicShear; // slope (dx per unit y) - see E28_msdf_vert.glsl's own comment on why field order/size here must stay in sync with that shader's own (shorter) PC block
    };

    // Push constants for the glyph-bounds debug overlay's OWN dedicated pipeline (E28_wire_vert/frag)
    // - see E28_wire_vert.glsl's own comment on why this is a separate shader pair rather than a
    // branch in msdf_push_constants/the MSDF shader. Color is fixed (opaque red) in the shader itself.
    struct wire_push_constants
    {
        xmath::fvec2 m_Scale;
        xmath::fvec2 m_Translate;
    };

    //---------------------------------------------------------------------------

    // Renders LayoutText's quads into an offscreen texture using the MSDF shader pair, for display
    // via ImGui::Image - same offscreen-render-to-texture pattern E22_FramebufferTarget uses, sized
    // to fit whatever text is currently laid out rather than a fixed viewport. Effects (outline) are
    // computed here, per-frame, from the companion SDF texture - never baked, per the descriptor's
    // own design note.
    struct text_renderer
    {
        static constexpr int MaxQuads = 1024;
        static constexpr int MaxSceneDim = 4096;

        // The scene texture is sized to exactly fit the current content (like a normal render-to-
        // texture setup) and is destroyed/recreated whenever that size changes - but only past a
        // hysteresis threshold (see Draw()'s SizeChangedEnough check), not on every pixel of difference.
        // Recreating on literally every frame (e.g. while continuously dragging the Zoom slider, where
        // the requested size changes slightly every frame) raced the GPU still using the previous
        // frame's texture/renderpass/pipeline_instance and crashed the app with invalid Vulkan image
        // layouts. Snapping to fixed size buckets keeps recreation rare during a drag while still
        // tracking the real content size closely enough that the display never looks visibly padded.
        xgpu::vertex_descriptor  m_VertexDescriptor;
        xgpu::pipeline           m_Pipeline;
        // Glyph-bounds debug overlay's own pipeline (E28_wire_vert/frag) - see that shader's own
        // comment on why it's separate rather than a branch in the main MSDF pipeline. Has no texture
        // samplers and no dependency on the atlas/SDF texture, so unlike m_PipelineInstance below it's
        // created once here and never needs rebuilding.
        xgpu::pipeline           m_WirePipeline;
        xgpu::pipeline_instance  m_WirePipelineInstance;
        xgpu::buffer             m_VertexBuffer;
        xgpu::buffer             m_IndexBuffer;
        xgpu::texture            m_DummyTexture;      // bound to the SDF slot when the font has no companion
        xgpu::texture            m_SceneTexture;
        xgpu::renderpass         m_RenderPass;
        xgpu::pipeline_instance  m_PipelineInstance;
        // Identity of the underlying GPU handle actually bound (xgpu::texture::m_Private.get()), NOT
        // the outer xfont::rt::m_pAtlasTexture pointer - when the user edits the descriptor (e.g.
        // toggling CompressAtlas) and hits Compile, the live file-watcher recompiles the font+texture
        // and xresource::mgr reloads it, which can reuse the SAME pooled xgpu::texture* address for a
        // genuinely NEW underlying Vulkan image. Comparing the outer pointer alone would then wrongly
        // conclude "nothing changed" and keep drawing with a pipeline_instance bound to the now-destroyed
        // old image view - exactly what crashed with "imageView is invalid or has been destroyed".
        void*                    m_pCachedAtlasHandle = nullptr;
        void*                    m_pCachedSDFHandle   = nullptr;
        int                      m_SceneW = 0, m_SceneH = 0; // the scene texture's own current allocated size
        int                      m_UsedW = 0, m_UsedH = 0;   // the sub-rect of it actually drawn into last Draw() call
        int                      m_WireQuadCount = 0, m_WireIndexStart = 0; // last Draw() call's debug wireframe range - see Draw()'s own comment

        int Create(xgpu::device& Device)
        {
            {
                auto Attributes = std::array
                {
                    xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(msdf_vert, m_X), .m_Format = xgpu::vertex_descriptor::format::FLOAT_2D }
                ,   xgpu::vertex_descriptor::attribute{ .m_Offset = offsetof(msdf_vert, m_U), .m_Format = xgpu::vertex_descriptor::format::FLOAT_2D }
                };
                auto Setup = xgpu::vertex_descriptor::setup{ .m_VertexSize = sizeof(msdf_vert), .m_Attributes = Attributes };
                if (auto Err = Device.Create(m_VertexDescriptor, Setup); Err) return xgpu::getErrorInt(Err);
            }

            xgpu::shader FragShader, VertShader;
            {
                xgpu::shader::setup Setup{ .m_Type = xgpu::shader::type::bit::FRAGMENT, .m_Sharer = xgpu::shader::setup::raw_data{ AsShaderSpan(g_MsdfFragSPVWords, std::size(g_MsdfFragSPVWords)) } };
                if (auto Err = Device.Create(FragShader, Setup); Err) return xgpu::getErrorInt(Err);
            }
            {
                xgpu::shader::setup Setup{ .m_Type = xgpu::shader::type::bit::VERTEX, .m_Sharer = xgpu::shader::setup::raw_data{ AsShaderSpan(g_MsdfVertSPVWords, std::size(g_MsdfVertSPVWords)) } };
                if (auto Err = Device.Create(VertShader, Setup); Err) return xgpu::getErrorInt(Err);
            }

            {
                auto Shaders  = std::array<const xgpu::shader*, 2>{ &FragShader, &VertShader };
                auto Samplers = std::array{ xgpu::pipeline::sampler{}, xgpu::pipeline::sampler{} };
                auto Setup = xgpu::pipeline::setup
                {
                    .m_VertexDescriptor  = m_VertexDescriptor
                ,   .m_Shaders           = Shaders
                ,   .m_PushConstantsSize = sizeof(msdf_push_constants)
                ,   .m_Samplers          = Samplers
                ,   .m_DepthStencil      = { .m_bDepthTestEnable = false, .m_bDepthWriteEnable = false }
                ,   .m_Blend             = xgpu::pipeline::blend::getAlphaOriginal()
                };
                if (auto Err = Device.Create(m_Pipeline, Setup); Err) return xgpu::getErrorInt(Err);
            }

            {
                xgpu::shader WireFragShader, WireVertShader;
                {
                    xgpu::shader::setup Setup{ .m_Type = xgpu::shader::type::bit::FRAGMENT, .m_Sharer = xgpu::shader::setup::raw_data{ AsShaderSpan(g_WireFragSPVWords, std::size(g_WireFragSPVWords)) } };
                    if (auto Err = Device.Create(WireFragShader, Setup); Err) return xgpu::getErrorInt(Err);
                }
                {
                    xgpu::shader::setup Setup{ .m_Type = xgpu::shader::type::bit::VERTEX, .m_Sharer = xgpu::shader::setup::raw_data{ AsShaderSpan(g_WireVertSPVWords, std::size(g_WireVertSPVWords)) } };
                    if (auto Err = Device.Create(WireVertShader, Setup); Err) return xgpu::getErrorInt(Err);
                }

                auto Shaders = std::array<const xgpu::shader*, 2>{ &WireFragShader, &WireVertShader };
                auto Setup = xgpu::pipeline::setup
                {
                    .m_VertexDescriptor  = m_VertexDescriptor // same aPos/aUV layout as the MSDF pipeline - aUV simply unused here
                ,   .m_Shaders           = Shaders
                ,   .m_PushConstantsSize = sizeof(wire_push_constants)
                // WIRELINE = the real fix: this draws the SAME triangles as the real glyph quads (see
                // Draw()'s own comment), just rasterized as polygon edges instead of filled - showing
                // the actual mesh, diagonal included, not a synthetic bounding-box border. Cull::NONE
                // since we want every edge regardless of the quads' winding order.
                ,   .m_Primitive         = { .m_Raster = xgpu::pipeline::primitive::raster::WIRELINE, .m_Cull = xgpu::pipeline::primitive::cull::NONE }
                ,   .m_DepthStencil      = { .m_bDepthTestEnable = false, .m_bDepthWriteEnable = false }
                ,   .m_Blend             = xgpu::pipeline::blend::getAlphaOriginal()
                };
                if (auto Err = Device.Create(m_WirePipeline, Setup); Err) return xgpu::getErrorInt(Err);
                if (auto Err = Device.Create(m_WirePipelineInstance, { .m_PipeLine = m_WirePipeline }); Err) return xgpu::getErrorInt(Err);
            }

            {
                std::array<std::byte, 4> Pixels{};
                if (auto Err = Device.Create(m_DummyTexture, { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_AdressModes = {}, .m_Width = 1, .m_Height = 1, .m_AllFacesData = Pixels, .m_isGamma = false }); Err)
                    return xgpu::getErrorInt(Err);
            }

            if (auto Err = Device.Create(m_VertexBuffer, { .m_Type = xgpu::buffer::type::VERTEX, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(msdf_vert), .m_EntryCount = MaxQuads * 4 }); Err)
                return xgpu::getErrorInt(Err);
            if (auto Err = Device.Create(m_IndexBuffer, { .m_Type = xgpu::buffer::type::INDEX, .m_Usage = xgpu::buffer::setup::usage::CPU_WRITE_GPU_READ, .m_EntryByteSize = sizeof(std::uint32_t), .m_EntryCount = MaxQuads * 6 }); Err)
                return xgpu::getErrorInt(Err);

            return 0;
        }

        // Snaps a requested dimension up to the nearest 25%-growth bucket (min 64px) so the scene
        // texture only needs recreating when the requested size crosses a bucket boundary - not on
        // every single-pixel change while a slider is being dragged continuously.
        static int SnapToBucket(int Value)
        {
            int Bucket = 64;
            while (Bucket < Value && Bucket < MaxSceneDim) Bucket = Bucket + Bucket / 4 + 1;
            return std::min(Bucket, MaxSceneDim);
        }

        int UpdateRenderTarget(xgpu::device& Device, int PixelW, int PixelH)
        {
            const int BucketW = SnapToBucket(PixelW);
            const int BucketH = SnapToBucket(PixelH);

            if (m_SceneTexture.m_Private && BucketW == m_SceneW && BucketH == m_SceneH)
                return 0;

            if (m_SceneTexture.m_Private)
            {
                xgpu::tools::imgui::ClearTexture(m_SceneTexture);
                Device.Destroy(std::move(m_SceneTexture));
            }
            if (m_RenderPass.m_Private)        Device.Destroy(std::move(m_RenderPass));
            if (m_PipelineInstance.m_Private)  Device.Destroy(std::move(m_PipelineInstance));

            m_SceneW = BucketW;
            m_SceneH = BucketH;

            if (auto Err = Device.Create(m_SceneTexture, { .m_Format = xgpu::texture::format::R8G8B8A8_UNORM, .m_Width = BucketW, .m_Height = BucketH, .m_isGamma = false }); Err)
                return xgpu::getErrorInt(Err);

            std::array<xgpu::renderpass::attachment, 1> Attachments{ m_SceneTexture };
            auto RPSetup = xgpu::renderpass::setup{ .m_Attachments = Attachments, .m_ClearColorR = 0, .m_ClearColorG = 0, .m_ClearColorB = 0, .m_ClearColorA = 0 };
            if (auto Err = Device.Create(m_RenderPass, RPSetup); Err) return xgpu::getErrorInt(Err);

            m_pCachedAtlasHandle = nullptr; // pipeline_instance doesn't reference the renderpass, but was destroyed above - force it to rebuild
            return 0;
        }

        int m_SkipFrames = 0; // set by Reset() after a live recompile - see its own comment

        // Called whenever the selected font is about to reload (a recompile completed) - a font's
        // atlas texture reference resolves to the SAME virtual-texture GUID across recompiles, so the
        // resource manager reloads it in place rather than handing back a new identity; the compile-
        // completion notification that triggers this also arrives on the background compile-worker
        // thread (see CompilingThreadWorker/m_OnCompilationState), not the render thread. Dropping our
        // own cached pipeline_instance immediately - rather than trusting the handle-identity check in
        // UpdatePipelineInstance to catch it - and skipping a few frames before touching the texture
        // again gives that reload time to actually land before we bind it into a new descriptor set.
        // Without this, toggling a compression setting and recompiling while the preview is visible
        // reliably crashed with "imageView is invalid or has been destroyed".
        void Reset(xgpu::device& Device)
        {
            if (m_PipelineInstance.m_Private) Device.Destroy(std::move(m_PipelineInstance));
            m_pCachedAtlasHandle = nullptr;
            m_pCachedSDFHandle   = nullptr;
            m_SkipFrames         = 3;
        }

        int UpdatePipelineInstance(xgpu::device& Device, xgpu::texture* pAtlas, xgpu::texture* pSDF)
        {
            void* pAtlasHandle = pAtlas ? pAtlas->m_Private.get() : nullptr;
            void* pSDFHandle   = pSDF   ? pSDF->m_Private.get()   : nullptr;

            if (pAtlasHandle == m_pCachedAtlasHandle && pSDFHandle == m_pCachedSDFHandle && m_PipelineInstance.m_Private)
                return 0;

            if (m_PipelineInstance.m_Private) Device.Destroy(std::move(m_PipelineInstance));

            auto& SDFTex   = pSDF ? *pSDF : m_DummyTexture;
            auto  Bindings = std::array{ xgpu::pipeline_instance::sampler_binding{ *pAtlas }, xgpu::pipeline_instance::sampler_binding{ SDFTex } };
            auto  Setup    = xgpu::pipeline_instance::setup{ .m_PipeLine = m_Pipeline, .m_SamplersBindings = Bindings };
            if (auto Err = Device.Create(m_PipelineInstance, Setup); Err) return xgpu::getErrorInt(Err);

            m_pCachedAtlasHandle = pAtlasHandle;
            m_pCachedSDFHandle   = pSDFHandle;
            return 0;
        }

        // Camera-driven viewport, like a 3D scene panel (e.g. E25_SkinGeomEditor's own view) - NOT
        // "render small, then stretch a picture". The render target is sized to the ACTUAL on-screen
        // panel (ViewW x ViewH, caller-provided, straight from ImGui::GetContentRegionAvail()) and
        // stays that size regardless of zoom; PxPerEm (the caller folds its own view-zoom multiplier
        // into this) and PanPx (pixel-space offset of the pen origin) are the camera. Zooming in
        // therefore renders MORE detail at the SAME output resolution - text can render larger than
        // the viewport and get panned/clipped, exactly like moving a camera - rather than rendering
        // once at a fixed size and blowing up the resulting bitmap for display. This is also why
        // m_LineWidth=1 on the glyph-bounds wire pipeline below is a genuine 1 SCREEN pixel at any
        // zoom: the wireframe is rasterized at final display resolution, never magnified afterward.
        int Draw
        ( xgpu::device&                    Device
        , xgpu::window&                    MainWindow
        , const xfont_rsc::font&           Font
        , xgpu::texture*                   pAtlas
        , xgpu::texture*                   pSDF
        , const std::vector<text_quad>&    Quads
        , float                            TextWidthEm
        , int                              ViewW
        , int                              ViewH
        , float                            PxPerEm
        , xmath::fvec2                     PanPx
        , bool                             bOutline
        , float                            OutlineWidthEm
        , bool                             bBold
        , float                            FontWeightEm
        , bool                             bShowShadow
        , xmath::fvec2                     ShadowOffsetEm
        , bool                             bBevel
        , float                            BevelWeightEm
        , bool                             bGlow
        , float                            GlowRadiusEm
        , float                            GlowIntensity
        , bool                             bItalic
        , float                            ItalicShear
        , bool                             bShowBounds = false
        )
        {
            if (pAtlas == nullptr) return 0;

            if (m_SkipFrames > 0) { --m_SkipFrames; return 0; } // settling after Reset() - see its own comment

            const int PixelW = std::clamp(ViewW, 8, MaxSceneDim);
            const int PixelH = std::clamp(ViewH, 8, MaxSceneDim);
            m_UsedW = PixelW;
            m_UsedH = PixelH;

            if (auto Err = UpdateRenderTarget(Device, PixelW, PixelH); Err) return Err;
            if (auto Err = UpdatePipelineInstance(Device, pAtlas, pSDF); Err) return Err;

            const int QuadCount = std::min<int>(static_cast<int>(Quads.size()), MaxQuads);

            if (QuadCount > 0)
            {
                (void)m_VertexBuffer.MemoryMap(0, QuadCount * 4, [&](void* pData)
                {
                    auto* pV = static_cast<msdf_vert*>(pData);
                    for (int i = 0; i < QuadCount; ++i)
                    {
                        const auto& Q = Quads[i];
                        pV[i * 4 + 0] = { Q.m_Min.x(), Q.m_Min.y(), Q.m_UVMin.x(), Q.m_UVMin.y() };
                        pV[i * 4 + 1] = { Q.m_Max.x(), Q.m_Min.y(), Q.m_UVMax.x(), Q.m_UVMin.y() };
                        pV[i * 4 + 2] = { Q.m_Max.x(), Q.m_Max.y(), Q.m_UVMax.x(), Q.m_UVMax.y() };
                        pV[i * 4 + 3] = { Q.m_Min.x(), Q.m_Max.y(), Q.m_UVMin.x(), Q.m_UVMax.y() };
                    }
                });

                // Round the mapped range up to a multiple of 16 indices (16*4 bytes = 64 bytes) - the
                // GPU's nonCoherentAtomSize requires flush/invalidate ranges to be 64-byte aligned, and
                // QuadCount*6 isn't one in general. The extra slots (never referenced by Draw() below,
                // which still only draws the real QuadCount*6) are harmless to write.
                const int AlignedIndexCount = std::min(MaxQuads * 6, ((QuadCount * 6) + 15) / 16 * 16);
                (void)m_IndexBuffer.MemoryMap(0, AlignedIndexCount, [&](void* pData)
                {
                    auto* pI = static_cast<std::uint32_t*>(pData);
                    for (int i = 0; i < QuadCount; ++i)
                    {
                        const std::uint32_t Base = static_cast<std::uint32_t>(i * 4);
                        *pI++ = Base + 0; *pI++ = Base + 1; *pI++ = Base + 2;
                        *pI++ = Base + 0; *pI++ = Base + 2; *pI++ = Base + 3;
                    }
                });

                // Debug glyph-bounds overlay (RenderSettings/Debug/ShowGlyphBounds) - a SEPARATE,
                // slightly-inset copy of each glyph's quad, appended right after the real glyphs in
                // the same vertex/index buffers (capped to whatever's left of MaxQuads) and drawn
                // through the wire pipeline's WIRELINE mode below. Inset is debug-display-only - the
                // REAL fill quads above are untouched, so this never affects actual rendering, only
                // how tightly the overlay hugs each glyph. Inset by exactly 1 atlas TEXEL (each
                // quad's own m_TexelEm, computed once in LayoutText from that glyph's real
                // AtlasW/H) - not a screen-pixel amount, since the point of this overlay is comparing
                // against the atlas's own packed rects (e.g. spotting packer overlaps), which are
                // texel-relative, not tied to whatever zoom the viewport happens to be at.
                m_WireQuadCount = bShowBounds ? std::min(QuadCount, MaxQuads - QuadCount) : 0;
                if (m_WireQuadCount > 0)
                {
                    (void)m_VertexBuffer.MemoryMap(QuadCount * 4, m_WireQuadCount * 4, [&](void* pData)
                    {
                        auto* pV = static_cast<msdf_vert*>(pData);
                        for (int i = 0; i < m_WireQuadCount; ++i)
                        {
                            const auto& Q = Quads[i];
                            const float MinX = Q.m_Min.x() + Q.m_TexelEm.x(), MinY = Q.m_Min.y() + Q.m_TexelEm.y();
                            const float MaxX = Q.m_Max.x() - Q.m_TexelEm.x(), MaxY = Q.m_Max.y() - Q.m_TexelEm.y();
                            pV[i * 4 + 0] = { MinX, MinY, 0.0f, 0.0f };
                            pV[i * 4 + 1] = { MaxX, MinY, 0.0f, 0.0f };
                            pV[i * 4 + 2] = { MaxX, MaxY, 0.0f, 0.0f };
                            pV[i * 4 + 3] = { MinX, MaxY, 0.0f, 0.0f };
                        }
                    });

                    m_WireIndexStart = AlignedIndexCount; // already 16-index (64-byte) aligned
                    const int WireIndexCountAligned = std::min(MaxQuads * 6 - m_WireIndexStart, ((m_WireQuadCount * 6) + 15) / 16 * 16);
                    (void)m_IndexBuffer.MemoryMap(m_WireIndexStart, WireIndexCountAligned, [&](void* pData)
                    {
                        auto* pI = static_cast<std::uint32_t*>(pData);
                        for (int i = 0; i < m_WireQuadCount; ++i)
                        {
                            const std::uint32_t Base = static_cast<std::uint32_t>(i * 4);
                            *pI++ = Base + 0; *pI++ = Base + 1; *pI++ = Base + 2;
                            *pI++ = Base + 0; *pI++ = Base + 2; *pI++ = Base + 3;
                        }
                    });
                }
            }
            else
            {
                m_WireQuadCount = 0;
            }

            auto CmdBuffer = MainWindow.StartRenderPass(m_RenderPass);
            // The scene texture is bucket-snapped (see SnapToBucket) and so is usually somewhat larger
            // than the actual content - constrain the viewport/scissor to exactly [PixelW x PixelH] (the
            // size the NDC transform below assumes) rather than letting it default to the full, larger
            // attachment, or the content renders squashed to fill the whole bucket and then gets cropped
            // on top of that. Display reads back this same [PixelW x PixelH] sub-rect via UV.
            CmdBuffer.setViewport(0, 0, static_cast<float>(PixelW), static_cast<float>(PixelH));
            CmdBuffer.setScissor(0, 0, PixelW, PixelH);
            CmdBuffer.setPipelineInstance(m_PipelineInstance);
            CmdBuffer.setBuffer(m_VertexBuffer);
            CmdBuffer.setBuffer(m_IndexBuffer);

            // Anchor the pen origin (em-space 0,0 - where LayoutText starts) at the viewport's own
            // center MINUS half the text's current on-screen width, so the text as a whole sits
            // centered - both axes - offset by the camera's own pan. Still NOT "fit the whole text's
            // bounding box to the viewport" like the old content-driven SCALING did (that re-scaled
            // PxPerEm itself whenever PenXFinal changed, every keystroke) - PxPerEm/PanPx stay purely
            // camera-driven; only the anchor's OWN position reacts to content width, same as centering
            // any other camera-framed object without changing the camera's zoom.
            const xmath::fvec2 AnchorPx{ static_cast<float>(PixelW) * 0.5f - TextWidthEm * PxPerEm * 0.5f + PanPx.x(), PixelH * 0.5f + PanPx.y() };

            msdf_push_constants PC{};
            const float ScaleX = 2.0f * PxPerEm / static_cast<float>(PixelW);
            const float ScaleY = -2.0f * PxPerEm / static_cast<float>(PixelH); // flip: +Y (ascender) renders toward the top of the image
            PC.m_Scale          = xmath::fvec2{ ScaleX, ScaleY };
            PC.m_Translate      = xmath::fvec2{ AnchorPx.x() * 2.0f / static_cast<float>(PixelW) - 1.0f, AnchorPx.y() * 2.0f / static_cast<float>(PixelH) - 1.0f };
            PC.m_PixelRange     = Font.m_PixelRange;
            PC.m_Color          = 0xFFFFFFFFu; // opaque white fill
            // BITMAP has no distance field to compute an outline from - see the shader's own early-out.
            PC.m_bOutline       = (bOutline && pSDF != nullptr && Font.m_OutputType != xfont_rsc::output_type::BITMAP) ? 1u : 0u;
            PC.m_OutlineColor   = 0xFF000000u; // opaque black outline
            PC.m_OutlineWidthPx = OutlineWidthEm * PxPerEm;
            PC.m_OutputType     = static_cast<std::uint32_t>(Font.m_OutputType);
            PC.m_FontWeightPx   = bBold ? FontWeightEm * PxPerEm : 0.0f;
            // Bevel needs a real SDF gradient to shade from - no-op (and left at 0, the shader's own
            // off switch) for BITMAP, same reasoning as outline just above.
            PC.m_BevelWeightPx  = (bBevel && Font.m_OutputType != xfont_rsc::output_type::BITMAP) ? BevelWeightEm * PxPerEm : 0.0f;
            // Glow needs distance OUTSIDE the glyph, same as outline/bevel - no-op for BITMAP.
            PC.m_GlowRadiusPx   = (bGlow && Font.m_OutputType != xfont_rsc::output_type::BITMAP) ? GlowRadiusEm * PxPerEm : 0.0f;
            PC.m_GlowIntensity  = GlowIntensity;
            PC.m_ItalicShear    = bItalic ? ItalicShear : 0.0f; // a dimensionless slope, not an em length - no PxPerEm conversion needed

            if (bShowShadow && QuadCount > 0)
            {
                // Drop shadow = the SAME real glyph mesh (same vertex/index range, offset 0), redrawn
                // BEFORE the fill pass with the pen origin translated by ShadowOffsetEm and a flat dark
                // color - no outline/bevel/glow on the shadow itself. Because it's the real mesh sampling
                // its own correct UV rect (not a same-texture UV-shift trick), there's no risk of
                // bleeding into a neighboring glyph's atlas cell regardless of offset size, and the
                // shadow comes out correctly shaped/antialiased for free since it's the real SDF.
                msdf_push_constants ShadowPC = PC;
                ShadowPC.m_Translate    = xmath::fvec2{ PC.m_Translate.x() + ShadowOffsetEm.x() * ScaleX, PC.m_Translate.y() + ShadowOffsetEm.y() * ScaleY };
                ShadowPC.m_Color        = 0xC0000000u; // translucent black
                ShadowPC.m_bOutline     = 0u;
                ShadowPC.m_BevelWeightPx = 0.0f;
                ShadowPC.m_GlowRadiusPx  = 0.0f;
                CmdBuffer.setPushConstants(ShadowPC);
                CmdBuffer.Draw(QuadCount * 6);
            }

            CmdBuffer.setPushConstants(PC);

            if (QuadCount > 0)
                CmdBuffer.Draw(QuadCount * 6);

            if (m_WireQuadCount > 0)
            {
                // Draws the inset copy built above (own dedicated pipeline - see E28_wire_vert.glsl's
                // own comment), NOT the real glyph triangles - shows each glyph's own mesh shape
                // (diagonal included), just inset by a debug-only margin so it isn't swallowed by
                // msdf-atlas-gen's own antialiasing padding on the real quad. Same Scale/Translate as
                // the glyphs above (unchanged), so it lands in the same em-space transform.
                CmdBuffer.setPipelineInstance(m_WirePipelineInstance);
                wire_push_constants WirePC{ .m_Scale = PC.m_Scale, .m_Translate = PC.m_Translate };
                CmdBuffer.setPushConstants(WirePC);
                CmdBuffer.Draw(m_WireQuadCount * 6, m_WireIndexStart, QuadCount * 4);
            }

            return 0;
        }
    };
}

#endif // XFONT_EDITOR_PREVIEW_H
