#ifndef XFONT_RSC_DESCRIPTOR_H
#define XFONT_RSC_DESCRIPTOR_H
#pragma once

#include "xfont_rsc_runtime.h" // for xfont_rsc::output_type - see its own comment further down

// Font - bakes a TTF/OTF into one of three purpose-built glyph atlas resources, picked by
// OutputType. The charset is always user-driven (not a fixed Unicode-block default): point
// SampleTextFiles at your actual game text and the compiler collects every unique character
// actually used - this keeps the atlas small and exactly matched to what you need, with
// ExplicitRanges available to guarantee extra coverage beyond whatever the sample text uses.
//
// Why three output types instead of one "does everything" MSDF mode: block compression (BC7) and
// multi-channel signed-distance data don't mix - small, unequal per-block error across R/G/B breaks
// the median reconstruction MSDF depends on, and no amount of quality/padding/sRGB tuning fixes
// that (confirmed empirically, not just in theory - see the 2026-09-03 session that landed this).
// So each output type gets the compression strategy that's actually safe for ITS data:
//   MTSDF   - scalable, multi-channel distance field. Always uncompressed RGBA8 - this is exactly
//             the data compression can't safely touch. RGB = median-reconstructible MSDF, A = true
//             single-channel SDF (for outline/glow - computed dynamically at render time, nothing
//             about an effect is ever baked into this resource).
//   SDF     - scalable, single-channel true distance field. Safe to compress (BC4): one channel has
//             no cross-channel consistency to break, so block error just locally perturbs one
//             distance value instead of corrupting a median.
//   BITMAP  - traditional pre-rasterized glyphs (FreeType's own AA rasterizer, not a distance field
//             at all) at a fixed set of point sizes, packed into one shared atlas. Not scalable -
//             sharpest choice for UI text at known, fixed sizes. Ordinary image, so ordinary
//             compression (with a real alpha channel for AA coverage) is fine.
// Each is emitted as a single virtual Texture resource (xresource_pipeline_v2's own documented
// example of a virtual resource is literally "a font resource generates a texture resource"),
// compiled by the existing xtexture_compiler.
namespace xfont_rsc
{
    inline static constexpr auto resource_type_guid_v = xresource::type_guid(xresource::guid_generator::Instance64FromString("font"));

    static constexpr wchar_t font_filter_v[] = L"Fonts\0 *.ttf; *.otf\0Any Thing\0 *.*\0";
    static constexpr wchar_t text_filter_v[] = L"Text Files\0 *.txt\0Any Thing\0 *.*\0";

    // output_type itself is declared in xfont_rsc_runtime.h, not here - descriptor and runtime are
    // the SAME plugin (not a cross-plugin boundary like texture_type_guid_v below), and the runtime
    // is the more fundamental of the two (a shipped game loading a compiled font needs it; nothing
    // needs the descriptor past compile time) - so descriptor depends on runtime for this shared
    // enum, not the other way around, and there's exactly one definition.
    static constexpr auto output_type_v = std::array
    { xproperty::settings::enum_item("MTSDF",  output_type::MTSDF,  "Scalable multi-channel distance field (RGB=median MSDF, A=true SDF for effects). Always uncompressed - the data compression can't safely touch.")
    , xproperty::settings::enum_item("SDF",    output_type::SDF,    "Scalable single-channel true distance field. Safe to compress (BC4) - unlike MTSDF, there's no cross-channel median to break.")
    , xproperty::settings::enum_item("BITMAP", output_type::BITMAP, "Traditional pre-rasterized glyphs at fixed point sizes - not scalable, but sharpest choice for UI text at known sizes, and compresses fine since it's an ordinary image.")
    };

    // Deliberately a small, self-contained enum (not xtexture_rsc::compression_format directly) -
    // matches this codebase's own established cross-plugin convention (see xfont_rsc_runtime.h's
    // comment on re-deriving texture_type_guid_v locally rather than including xtexture's header):
    // each resource plugin stays a self-contained DLL/exe. The compiler maps this to the real
    // xtexture_rsc::compression_format when constructing the virtual texture descriptor - same
    // pattern the old CompressAtlas/CompressSDF bools already used.
    enum class bitmap_compression : std::uint8_t { UNCOMPRESSED, BC1_ALPHA, BC3_ALPHA };
    static constexpr auto bitmap_compression_v = std::array
    { xproperty::settings::enum_item("UNCOMPRESSED", bitmap_compression::UNCOMPRESSED, "32bpp, full precision. Largest, use for debugging or if compression artifacts are ever visible.")
    , xproperty::settings::enum_item("BC1_ALPHA",    bitmap_compression::BC1_ALPHA,    "4bpp, 1-bit alpha - smallest, but AA edges will look hard/aliased (no partial coverage).")
    , xproperty::settings::enum_item("BC3_ALPHA",    bitmap_compression::BC3_ALPHA,    "8bpp, full 8-bit alpha - the right default for real antialiasing coverage.")
    };

    // A manual addition to the baked charset - independent of whatever the sample text files
    // happen to contain, so a range can be guaranteed present even if no sample text uses it yet.
    struct codepoint_range
    {
        std::uint32_t m_First{ 0x20 };
        std::uint32_t m_Last { 0x7E };

        void Validate(std::vector<std::string>& Errors) const noexcept
        {
            if (m_Last < m_First) Errors.push_back("A codepoint range's Last must be >= First");
        }

        XPROPERTY_DEF
        ( "CodepointRange", codepoint_range
        , obj_member<"First", &codepoint_range::m_First, member_help<"First Unicode codepoint in this range (inclusive).">>
        , obj_member<"Last",  &codepoint_range::m_Last,  member_help<"Last Unicode codepoint in this range (inclusive).">>
        )
    };
    XPROPERTY_REG(codepoint_range)

    struct descriptor final : xresource_pipeline::descriptor::base
    {
        std::wstring                 m_FontFile            {};
        std::vector<std::wstring>    m_SampleTextFiles     { {} };
        std::vector<codepoint_range> m_ExplicitRanges      {};
        bool                         m_bAlwaysIncludeAscii { true };

        output_type m_OutputType { output_type::MTSDF };

        // MTSDF/SDF only (a distance-field concept - meaningless for BITMAP's direct rasterization).
        float m_PixelRange     { 4.0f };
        float m_AngleThreshold { 3.0f }; // MTSDF only - edge coloring has no meaning without a median to reconstruct
        // Pixels-per-em each glyph is baked at - the only size knob MTSDF/SDF needs. The atlas
        // texture's own WxH is NOT set here: it's always auto-derived by the packer as the smallest
        // fitting rectangle for every baked glyph at this size (see xfont_compiler.cpp's own comment
        // on TightAtlasPacker - dimensions are left unset so it computes the minimum itself).
        float m_GlyphSize      { 32.0f };
        // Empty atlas pixels left between adjacent packed glyphs. Exists so a block-compression
        // format (BC7/BC4, 4x4 pixel blocks) never has a block straddling two unrelated glyphs -
        // with zero padding that mixes their distance-field values and BC7/BC4 can't represent the
        // discontinuity, producing visible color fringing right at glyph edges (confirmed 2026-09-03
        // via a direct A/B compressed-texture comparison). 4 (one full block) is a safe default, not
        // a hard requirement - PixelRange already adds its own margin around each glyph, so a
        // smaller value may already be enough; larger charsets/GlyphSize pay for more padding with a
        // bigger atlas, so it's worth tuning down if you verify it still looks clean.
        int   m_PixelPadding   { 4 };

        // BITMAP only.
        std::vector<int>    m_BitmapSizes       { 16 };  // point sizes (px) to bake - all packed into one shared atlas
        bitmap_compression  m_BitmapCompression { bitmap_compression::BC3_ALPHA };

        void SetupFromSource(std::string_view) override {}

        void Validate(std::vector<std::string>& Errors) const noexcept override
        {
            if (m_FontFile.empty()) Errors.push_back("You must select a Font File (.ttf/.otf)");

            bool bHasAnySource = m_bAlwaysIncludeAscii || !m_ExplicitRanges.empty();
            for (auto& F : m_SampleTextFiles) if (!F.empty()) bHasAnySource = true;
            if (!bHasAnySource) Errors.push_back("You must provide at least one Sample Text File, one Explicit Range, or enable AlwaysIncludeAscii - otherwise the atlas would be empty");

            for (auto& R : m_ExplicitRanges) R.Validate(Errors);

            if (m_OutputType == output_type::BITMAP)
            {
                if (m_BitmapSizes.empty()) Errors.push_back("BitmapSizes must have at least one size when OutputType is BITMAP");
                for (auto S : m_BitmapSizes) if (S < 4) Errors.push_back("Every BitmapSizes entry must be at least 4 pixels");
            }
            else if (m_GlyphSize < 4.0f) Errors.push_back("GlyphSize must be at least 4 pixels");
        }

        XPROPERTY_VDEF
        ( "Font", descriptor
        , obj_member<"FontFile"
            , &descriptor::m_FontFile
            , member_ui<std::wstring>::file_dialog<font_filter_v, true, 1>
            , member_help<"The TTF/OTF source font file this resource compiles from.">>
        , obj_member<"SampleTextFiles"
            , &descriptor::m_SampleTextFiles
            , member_ui_open<true>
            , member_ui<std::wstring>::file_dialog<text_filter_v, true, 1>
            , member_ui_list_size::drag_bar<0, 100>
            , member_help<"UTF-8 text files to scan - every unique character actually used across all of them is what gets baked into the atlas. This is the primary way to choose a charset: point it at your game's actual dialogue/UI text rather than guessing which Unicode ranges you might need.">>
        , obj_member<"AlwaysIncludeAscii"
            , &descriptor::m_bAlwaysIncludeAscii
            , member_help<"Always bake the basic ASCII printable range (space through ~) regardless of what the sample text files contain - convenient since it's almost always wanted.">>
        , obj_member<"ExplicitRanges"
            , &descriptor::m_ExplicitRanges
            , member_ui_open<true>
            , member_ui_list_size::drag_bar<0, 100>
            , member_help<"Additional Unicode codepoint ranges to bake regardless of the sample text files - use this to guarantee coverage of characters that might not appear in your current sample text yet.">>
        , obj_member<"OutputType"
            , &descriptor::m_OutputType
            , member_enum_span<output_type_v>
            , member_help<"MTSDF: scalable, always uncompressed (the safe choice for distance-field data). SDF: scalable, single-channel, safe to compress. BITMAP: fixed point sizes, not scalable, ordinary image compression.">>
        , obj_scope<"Distance Field"
            , obj_member<"PixelRange"
                , &descriptor::m_PixelRange
                // member_ui<float>::drag_bar's template order is <Speed, Min, Max> (Speed comes
                // first, unlike scroll_bar's <Min, Max>) - all 3 must be given explicitly or the
                // "Min" you meant to pass becomes the drag Speed and the real Min silently defaults
                // to whatever you put in that slot, clamping the value there. Confirmed live: an
                // earlier <1.0f, 16.0f> here actually meant Speed=1, Min=16, Max=unbounded.
                , member_ui<float>::drag_bar<0.1f, 1.0f, 16.0f>
                , member_dynamic_flags<+[](const descriptor& O)
                { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType == output_type::BITMAP; return F; }>
                , member_help<"Width, in atlas pixels, of the signed-distance band around each glyph's outline. Must match what the runtime shader assumes for anti-aliasing - larger values give smoother scaling and more room for outline/glow effects, at the cost of atlas resolution.">>
            , obj_member<"AngleThreshold"
                , &descriptor::m_AngleThreshold
                , member_ui<float>::drag_bar<0.05f, 1.0f, 5.0f>
                , member_dynamic_flags<+[](const descriptor& O)
                { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType != output_type::MTSDF; return F; }>
                , member_help<"Corner-detection threshold (radians) used when assigning MSDF edge colors. Only meaningful for MTSDF - a plain SDF has no median/edge-color reconstruction to protect. Lower values treat more bends as sharp corners; too low over-splits smooth curves, too high rounds off real corners.">>
            , obj_member<"GlyphSize"
                , &descriptor::m_GlyphSize
                , member_ui<float>::drag_bar<2.0f, 4.0f, 512.0f>
                , member_dynamic_flags<+[](const descriptor& O)
                { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType == output_type::BITMAP; return F; }>
                , member_help<"Pixels-per-em each glyph is baked at - this is the actual size of the rendered characters. The atlas texture is always auto-sized to the smallest fitting rectangle for every baked glyph at this size, so there's nothing to tune there.">>
            , obj_member<"PixelPadding"
                , &descriptor::m_PixelPadding
                , member_ui<int>::drag_bar<1, 0, 32>
                , member_help<"Empty atlas pixels between adjacent glyphs, to keep block compression (4x4-pixel blocks) from ever mixing two unrelated glyphs' data into one block - which shows up as color fringing at glyph edges. Applies to every output type (BITMAP's coverage-alpha can bleed too, just usually less visibly than a distance field). 4 (one full block) is a safe default; try smaller if you want a tighter atlas and verify a compressed compile still looks clean.">>
            >
        , obj_scope<"Output"
            , member_dynamic_flags<+[](const descriptor& O)
              { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType != output_type::BITMAP; return F; }>
            , obj_member<"BitmapSizes"
                , &descriptor::m_BitmapSizes
                , member_ui_open<true>
                , member_ui<int>::drag_bar<1, 4, 256>
                , member_ui_list_size::drag_bar<1, 16>
                , member_dynamic_flags<+[](const descriptor& O)
                { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType != output_type::BITMAP; return F; }>
                , member_help<"Point sizes (in pixels) to rasterize and bake, all packed into one shared atlas - e.g. 8, 12, 16 for a UI that needs several fixed text sizes. Each is a real FreeType rasterization at that exact size, not a scaled distance field.">>
            , obj_member<"BitmapCompression"
                , &descriptor::m_BitmapCompression
                , member_enum_span<bitmap_compression_v>
                , member_dynamic_flags<+[](const descriptor& O)
                { xproperty::flags::type F{}; F.m_bDontShow = O.m_OutputType != output_type::BITMAP; return F; }>
                , member_help<"Compression format for the baked bitmap atlas - an ordinary image, so ordinary compression is fine. Pick one with real alpha (BC3_ALPHA) if you want smooth antialiased edges.">>
            >
        )
    };
    XPROPERTY_VREG(descriptor)

    struct factory final : xresource_pipeline::factory_base
    {
        using xresource_pipeline::factory_base::factory_base;

        std::unique_ptr<xresource_pipeline::descriptor::base> CreateDescriptor(void) const noexcept override
        {
            return std::make_unique<descriptor>();
        }

        xresource::type_guid ResourceTypeGUID(void) const noexcept override { return resource_type_guid_v; }
        const char* ResourceTypeName(void) const noexcept override { return "Font"; }

        const xproperty::type::object& ResourceXPropertyObject(void) const noexcept override
        {
            return *xproperty::getObjectByType<descriptor>();
        }
    };
    inline static factory g_Factory{};
}
#endif
