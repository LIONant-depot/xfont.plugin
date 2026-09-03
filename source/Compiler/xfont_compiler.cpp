#include "xfont_compiler.h"
#include "../xfont_rsc_descriptor.h"
#include "../xfont_rsc_runtime.h"
#include "../bridges/xserializer/xfont_to_xserializer.h"

// Reused directly rather than hand-formatting Descriptor.txt/Info.txt text ourselves - guarantees
// byte-perfect compatibility with what xtexture_compiler itself expects (same xproperty::Serialize
// code path), at the cost of a lightweight, dependency-free header include across the plugin
// boundary. xtexture_rsc_descriptor.h has no runtime/GPU coupling, only xproperty reflection.
#include "../../../xtexture.plugin/source/xtexture_rsc_descriptor.h"

#include <msdfgen.h>
#include <msdfgen-ext.h>
#include "msdf-atlas-gen/Charset.h"
#include "msdf-atlas-gen/FontGeometry.h"
#include "msdf-atlas-gen/TightAtlasPacker.h"
#include "msdf-atlas-gen/ImmediateAtlasGenerator.h"
#include "msdf-atlas-gen/BitmapAtlasStorage.h"
#include "msdf-atlas-gen/glyph-generators.h"
#include "msdf-atlas-gen/RectanglePacker.h" // BITMAP mode's own packing - plain rectangles, no distance-field/scale concept

// msdfgen's own FreetypeHandle/FontHandle are opaque wrappers - BITMAP mode needs FreeType's real
// rasterizer (FT_Load_Glyph/FT_Render_Glyph) directly, bypassing msdfgen/msdf-atlas-gen entirely
// (there is no distance field to generate). FetchContent-built alongside msdfgen (see build/
// dependency/CMakeLists.txt), so its headers are already on this target's include path.
#include <ft2build.h>
#include FT_FREETYPE_H

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "dependencies/xproperty/source/xcore/my_properties.cpp"

#include <fstream>
#include <set>
#include <map>
#include <thread>
#include <algorithm>

namespace
{
    //-----------------------------------------------------------------------------------------
    // Decodes a UTF-8 text file into the set of unique Unicode codepoints it contains - this is
    // the primary, user-driven way to choose a charset (see xfont_rsc_descriptor.h's own top
    // comment): point it at real game text rather than guessing Unicode block ranges up front.
    //-----------------------------------------------------------------------------------------
    void CollectCodepointsFromUtf8File(const std::wstring& Path, std::set<std::uint32_t>& Out)
    {
        std::ifstream File(Path, std::ios::binary);
        if (!File) return;
        const std::string Bytes((std::istreambuf_iterator<char>(File)), std::istreambuf_iterator<char>());

        std::size_t i = 0;
        while (i < Bytes.size())
        {
            const unsigned char c0 = static_cast<unsigned char>(Bytes[i]);
            std::uint32_t Cp; int Len;
            if (c0 < 0x80)                                        { Cp = c0;          Len = 1; }
            else if ((c0 & 0xE0) == 0xC0 && i + 1 < Bytes.size())  { Cp = c0 & 0x1F;   Len = 2; }
            else if ((c0 & 0xF0) == 0xE0 && i + 2 < Bytes.size())  { Cp = c0 & 0x0F;   Len = 3; }
            else if ((c0 & 0xF8) == 0xF0 && i + 3 < Bytes.size())  { Cp = c0 & 0x07;   Len = 4; }
            else { ++i; continue; } // stray/invalid byte - skip it

            bool bValid = true;
            for (int k = 1; k < Len; ++k)
            {
                const unsigned char Ck = static_cast<unsigned char>(Bytes[i + k]);
                if ((Ck & 0xC0) != 0x80) { bValid = false; break; }
                Cp = (Cp << 6) | (Ck & 0x3F);
            }
            if (bValid && Cp != '\r' && Cp != '\n' && Cp != '\t') Out.insert(Cp);
            i += bValid ? static_cast<std::size_t>(Len) : 1;
        }
    }

    //-----------------------------------------------------------------------------------------
    // Writes an interleaved 8-bit RGB or single-channel buffer to disk as a PNG via stb_image_write
    // (MIT, header-only) - avoids taking on msdfgen/msdf-atlas-gen's own libpng-based PNG support
    // (which would need libpng+zlib discoverable at CMake configure time) just to write one file.
    //-----------------------------------------------------------------------------------------
    bool WritePng(const std::wstring& Path, int Width, int Height, int Channels, const std::vector<unsigned char>& Pixels)
    {
        const auto Utf8Path = xstrtool::To(std::wstring_view(Path));
        return stbi_write_png(Utf8Path.c_str(), Width, Height, Channels, Pixels.data(), Width * Channels) != 0;
    }
}

//---------------------------------------------------------------------------------------------

struct implementation final : xfont_compiler::instance
{
    using state    = xresource_pipeline::state;
    using msg_type = xresource_pipeline::msg_type;

    xfont_rsc::descriptor m_Descriptor;

    //---------------------------------------------------------------------------------------------

    xerr onCompile(void) override
    {
        //
        // Read the descriptor file...
        //
        {
            xproperty::settings::context Context{};
            auto DescriptorFileName = std::format(L"{}/{}/Descriptor.txt", m_ProjectPaths.m_Project, m_InputSrcDescriptorPath);
            if (auto Err = m_Descriptor.Serialize(true, DescriptorFileName, Context); Err)
                return Err;
        }

        //
        // Validate
        //
        {
            std::vector<std::string> Errors;
            m_Descriptor.Validate(Errors);
            if (Errors.empty() == false)
            {
                for (auto& E : Errors) LogMessage(msg_type::ERROR, E);
                return xerr::create_f<state, "The descriptor has validation errors">();
            }
        }

        displayProgressBar("Collecting charset", 0.0f);

        //
        // Build the final codepoint set - union of AlwaysIncludeAscii, ExplicitRanges, and every
        // unique character actually found in the sample text files.
        //
        std::set<std::uint32_t> CodepointSet;
        if (m_Descriptor.m_bAlwaysIncludeAscii)
            for (std::uint32_t Cp = 0x20; Cp <= 0x7E; ++Cp) CodepointSet.insert(Cp);
        for (auto& R : m_Descriptor.m_ExplicitRanges)
            for (std::uint32_t Cp = R.m_First; Cp <= R.m_Last; ++Cp) CodepointSet.insert(Cp);
        for (auto& F : m_Descriptor.m_SampleTextFiles)
            if (F.empty() == false)
                CollectCodepointsFromUtf8File(std::format(L"{}/{}", m_ProjectPaths.m_Project, F), CodepointSet);

        if (CodepointSet.empty())
            return xerr::create_f<state, "No codepoints were collected - the baked charset would be empty">();

        LogMessage(msg_type::INFO, std::format("Baking {} unique codepoints", CodepointSet.size()));

        displayProgressBar("Loading font", 0.1f);

        //
        // Load the font via FreeType (through msdfgen's own thin wrapper)
        //
        msdfgen::FreetypeHandle* pFreetype = msdfgen::initializeFreetype();
        if (pFreetype == nullptr)
            return xerr::create_f<state, "Failed to initialize FreeType">();

        const auto FontPathUtf8 = xstrtool::To(std::wstring_view(std::format(L"{}/{}", m_ProjectPaths.m_Project, m_Descriptor.m_FontFile)));
        msdfgen::FontHandle* pFont = msdfgen::loadFont(pFreetype, FontPathUtf8.c_str());
        if (pFont == nullptr)
        {
            msdfgen::deinitializeFreetype(pFreetype);
            return xerr::create_f<state, "Failed to load the font file - check FontFile points at a valid TTF/OTF">();
        }

        msdf_atlas::Charset Charset;
        for (auto Cp : CodepointSet) Charset.add(Cp);

        std::vector<msdf_atlas::GlyphGeometry> Glyphs;
        msdf_atlas::FontGeometry FontGeometry(&Glyphs);
        // preprocessGeometry=false: Skia (the alternative, more robust overlap-preprocessing path)
        // is intentionally not built in (see build/dependency/CMakeLists.txt) - overlapSupport +
        // scanlinePass on the generator side (below) is msdfgen's own documented fallback for
        // handling self-intersecting/overlapping contours without it.
        const int GlyphsLoaded = FontGeometry.loadCharset(pFont, 1.0, Charset, false, true);
        if (GlyphsLoaded <= 0)
        {
            msdfgen::destroyFont(pFont);
            msdfgen::deinitializeFreetype(pFreetype);
            return xerr::create_f<state, "Failed to load any glyphs from the font for the requested charset">();
        }
        if (GlyphsLoaded < static_cast<int>(Charset.size()))
            LogMessage(msg_type::WARNING, std::format("Only {} of {} requested codepoints exist in this font", GlyphsLoaded, Charset.size()));

        // Edge coloring assigns per-edge MSDF channel colors - meaningless for a plain single-channel
        // SDF (nothing to reconstruct a median from) and for BITMAP (not a distance field at all).
        if (m_Descriptor.m_OutputType == xfont_rsc::output_type::MTSDF)
            for (auto& G : Glyphs)
                G.edgeColoring(msdfgen::edgeColoringInkTrap, m_Descriptor.m_AngleThreshold, 0);

        //
        // BITMAP: a completely separate path from MTSDF/SDF below - direct FreeType rasterization,
        // not msdfgen's shape/distance-field pipeline, so it doesn't share any of that code. Packs
        // every (size,codepoint) glyph into one shared atlas and builds one CHD hash + glyph table
        // per requested size (see xfont_rsc_runtime.h's size_group_header/SizeGroups() layout).
        //
        if (m_Descriptor.m_OutputType == xfont_rsc::output_type::BITMAP)
        {
            // msdfgen's font/freetype wrappers aren't useful here - open a fresh, independent
            // FT_Library/FT_Face for direct rasterization instead.
            msdfgen::destroyFont(pFont);
            msdfgen::deinitializeFreetype(pFreetype);

            FT_Library FTLib{};
            if (FT_Init_FreeType(&FTLib))
                return xerr::create_f<state, "Failed to initialize FreeType for BITMAP rasterization">();

            FT_Face Face{};
            if (FT_New_Face(FTLib, FontPathUtf8.c_str(), 0, &Face))
            {
                FT_Done_FreeType(FTLib);
                return xerr::create_f<state, "Failed to load the font file via FreeType - check FontFile points at a valid TTF/OTF">();
            }

            displayProgressBar("Rasterizing glyphs", 0.25f);

            // One raw rasterized glyph, before packing - kept alongside its own pixel buffer
            // (FreeType reuses face->glyph->bitmap.buffer on every load, so this must be a copy)
            // until the packer has decided where every glyph across every requested size lands.
            struct raw_glyph
            {
                std::uint32_t              m_Codepoint{};
                int                         m_W{}, m_H{};               // rasterized bitmap size, pixels
                int                         m_BearingX{}, m_BearingY{}; // FreeType's own pen-relative bearing, pixels
                float                       m_Advance{};                 // pixels
                std::vector<unsigned char>  m_Coverage;                  // W*H, one byte per pixel
            };

            std::vector<raw_glyph> RawGlyphs;
            std::vector<std::pair<int,int>> SizeGroupRanges; // [FirstIndexInRawGlyphs, Count), one per m_BitmapSizes entry, same order

            // Em-relative ascender/descender/line-height (ratio to pixel size, so it's valid for
            // EVERY size group even though only captured once) - captured from whichever size
            // happens to be processed first. Needed so the editor's own bounding-box math (which
            // treats Font.m_Ascender/m_Descender as "1 em = this many units", same convention as
            // MTSDF/SDF) doesn't collapse BITMAP text to a near-zero-height box. Font{} itself isn't
            // constructed until after this loop, so stashed in locals and copied in below.
            float EmAscender = 0.0f, EmDescender = 0.0f, EmLineHeight = 0.0f;
            bool bHaveEmMetrics = false;

            for (int Size : m_Descriptor.m_BitmapSizes)
            {
                if (FT_Set_Pixel_Sizes(Face, 0, static_cast<FT_UInt>(Size)))
                {
                    FT_Done_Face(Face);
                    FT_Done_FreeType(FTLib);
                    return xerr::create_f<state, "FreeType rejected a requested BitmapSizes value">();
                }

                if (!bHaveEmMetrics && Size > 0)
                {
                    const float InvSize = 1.0f / static_cast<float>(Size);
                    EmAscender     = (static_cast<float>(Face->size->metrics.ascender)  / 64.0f) * InvSize;
                    EmDescender    = (static_cast<float>(Face->size->metrics.descender) / 64.0f) * InvSize;
                    EmLineHeight   = (static_cast<float>(Face->size->metrics.height)    / 64.0f) * InvSize;
                    bHaveEmMetrics = true;
                }

                const int FirstIndex = static_cast<int>(RawGlyphs.size());
                for (auto Cp : CodepointSet)
                {
                    const FT_UInt GlyphIndex = FT_Get_Char_Index(Face, Cp);
                    if (GlyphIndex == 0) continue; // this font doesn't have this codepoint - same tolerance as MTSDF/SDF's own "only N of M exist"

                    if (FT_Load_Glyph(Face, GlyphIndex, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL))
                        continue; // skip glyphs FreeType itself can't rasterize rather than failing the whole compile

                    const auto& Bmp = Face->glyph->bitmap;
                    raw_glyph RG;
                    RG.m_Codepoint = Cp;
                    RG.m_W         = static_cast<int>(Bmp.width);
                    RG.m_H         = static_cast<int>(Bmp.rows);
                    RG.m_BearingX  = Face->glyph->bitmap_left;
                    RG.m_BearingY  = Face->glyph->bitmap_top;
                    RG.m_Advance   = static_cast<float>(Face->glyph->advance.x) / 64.0f; // 26.6 fixed point

                    if (RG.m_W > 0 && RG.m_H > 0)
                    {
                        RG.m_Coverage.resize(static_cast<std::size_t>(RG.m_W) * RG.m_H);
                        // Row by row, not one memcpy - FT's own pitch can exceed width (row padding),
                        // and a negative pitch means a bottom-up bitmap.
                        const int Pitch = std::abs(Bmp.pitch);
                        for (int y = 0; y < RG.m_H; ++y)
                            std::memcpy(&RG.m_Coverage[static_cast<std::size_t>(y) * RG.m_W], Bmp.buffer + static_cast<std::size_t>(y) * Pitch, static_cast<std::size_t>(RG.m_W));
                    }
                    RawGlyphs.push_back(std::move(RG));
                }
                SizeGroupRanges.emplace_back(FirstIndex, static_cast<int>(RawGlyphs.size()) - FirstIndex);
            }

            FT_Done_Face(Face);
            FT_Done_FreeType(FTLib);

            if (RawGlyphs.empty())
                return xerr::create_f<state, "No glyphs could be rasterized for the requested charset/sizes">();

            displayProgressBar("Packing atlas", 0.4f);

            // Plain rectangle pack - no scale/distance-field concept here, unlike TightAtlasPacker
            // below. Non-ink glyphs (space: W==0 or H==0) need no atlas space - zero-size boxes,
            // skipped by codepoint (m_W/m_H) rather than by atlas footprint downstream.
            std::vector<msdf_atlas::Rectangle> PackRects(RawGlyphs.size());
            for (std::size_t i = 0; i < RawGlyphs.size(); ++i)
                PackRects[i] = { 0, 0
                    , RawGlyphs[i].m_W > 0 ? RawGlyphs[i].m_W + m_Descriptor.m_PixelPadding : 0
                    , RawGlyphs[i].m_H > 0 ? RawGlyphs[i].m_H + m_Descriptor.m_PixelPadding : 0 };

            // Upper-bound starting size (square, from total padded area), doubled until it actually
            // fits - then the same binary-search tightening shape as the MTSDF/SDF path below.
            std::uint64_t TotalArea = 0;
            for (auto& R : PackRects) TotalArea += static_cast<std::uint64_t>(R.w) * static_cast<std::uint64_t>(R.h);
            int UpperDim = std::max(4, static_cast<int>(std::sqrt(static_cast<double>(TotalArea))) + 4);
            for (;;)
            {
                std::vector<msdf_atlas::Rectangle> Probe = PackRects;
                if (msdf_atlas::RectanglePacker(UpperDim, UpperDim).pack(Probe.data(), static_cast<int>(Probe.size())) == 0)
                    break;
                UpperDim *= 2;
                if (UpperDim > 8192)
                    return xerr::create_f<state, "Could not fit every glyph into the atlas - reduce BitmapSizes or the charset">();
            }

            auto FitsBitmap = [&](int W, int H) -> bool
            {
                std::vector<msdf_atlas::Rectangle> Probe = PackRects;
                return msdf_atlas::RectanglePacker(W, H).pack(Probe.data(), static_cast<int>(Probe.size())) == 0;
            };
            // Lo/Hi bracket a binary search for the smallest passing multiple of 4, which REQUIRES Hi
            // itself be a verified-passing value going in - Hi=UpperBound/4 (truncating) violated that:
            // UpperBound is only verified to fit AT UpperBound itself, and integer division can put
            // Hi*4 strictly below it, so a still-untested (and potentially failing) size could get
            // accepted as if it had passed. Ceiling division keeps Hi*4 >= UpperBound, which - by
            // packing monotonicity (anything that fits a smaller container also fits a same-or-larger
            // one, same rects, same positions) - really IS guaranteed to pass. Silently accepting an
            // untested, too-small atlas size here is what let glyphs' packed rects end up overlapping
            // (a later glyph's compositing pass overwriting part of an earlier one) with no error at
            // all - exactly the kind of bug that only shows up as a glyph looking "cut off" later.
            auto TightenBitmapAxis = [&](int UpperBound, auto&& FitsAt) -> int
            {
                int Lo = 1, Hi = std::max(1, (UpperBound + 3) / 4);
                while (Lo < Hi)
                {
                    const int Mid = Lo + (Hi - Lo) / 2;
                    if (FitsAt(Mid * 4)) Hi = Mid; else Lo = Mid + 1;
                }
                return Lo * 4;
            };
            const int AtlasWidth  = TightenBitmapAxis(UpperDim, [&](int W) { return FitsBitmap(W, UpperDim); });
            const int AtlasHeight = TightenBitmapAxis(UpperDim, [&](int H) { return FitsBitmap(AtlasWidth, H); });

            // Lock in final placement (PackRects.x/y) at the tightened size - checked now (unlike
            // before), since a silent failure here means every glyph's stored m_AtlasX/Y/W/H is
            // reporting a rect that was never actually reserved for it alone.
            if (msdf_atlas::RectanglePacker(AtlasWidth, AtlasHeight).pack(PackRects.data(), static_cast<int>(PackRects.size())) != 0)
                return xerr::create_f<state, "Internal error: final BITMAP atlas packing failed after tightening reported it would fit">();

            LogMessage(msg_type::INFO, std::format("Atlas dimensions: {} x {} ({} glyphs across {} size(s))", AtlasWidth, AtlasHeight, RawGlyphs.size(), m_Descriptor.m_BitmapSizes.size()));

            displayProgressBar("Compositing atlas", 0.55f);

            // White RGB (color is applied at render time via push-constant tint, see the shader) -
            // coverage lives in alpha.
            std::vector<unsigned char> TexturePixels(static_cast<std::size_t>(AtlasWidth) * AtlasHeight * 4, 0);
            for (std::size_t i = 0; i < RawGlyphs.size(); ++i)
            {
                auto& RG = RawGlyphs[i];
                if (RG.m_W <= 0 || RG.m_H <= 0) continue;
                const auto& R = PackRects[i];
                for (int y = 0; y < RG.m_H; ++y)
                    for (int x = 0; x < RG.m_W; ++x)
                    {
                        auto* pOut = &TexturePixels[(static_cast<std::size_t>(R.y + y) * AtlasWidth + static_cast<std::size_t>(R.x + x)) * 4];
                        pOut[0] = 255; pOut[1] = 255; pOut[2] = 255;
                        pOut[3] = RG.m_Coverage[static_cast<std::size_t>(y) * RG.m_W + x];
                    }
            }

            displayProgressBar("Writing atlas assets", 0.65f);

            const auto FontGuidHex   = std::format("{:016X}", m_ResourceGuid.m_Value);
            const auto InstanceGuid = xresource::instance_guid::GenerateGUIDCopy(std::format("{}_bitmap", FontGuidHex).c_str());
            const auto GuidValue    = InstanceGuid.m_Value;
            const auto Byte0        = std::format("{:02X}", (GuidValue) & 0xFF);
            const auto Byte1        = std::format("{:02X}", (GuidValue >> 8) & 0xFF);
            const auto GuidHex      = std::format("{:016X}", GuidValue);

            // Same reasoning as the MTSDF/SDF EmitVirtualTexture lambda further down this file: the
            // PNG lives directly inside this virtual resource's own descriptor folder (GUID-sharded,
            // same scheme every resource uses) rather than a shared Cache/Temp/Font dumping ground,
            // since nothing but this one virtual resource ever references it.
            const auto DescDir             = std::format(L"{}/Texture/{}/{}/{}.desc", m_ProjectPaths.m_CachedDescriptors, xstrtool::To(std::string_view(Byte0)), xstrtool::To(std::string_view(Byte1)), xstrtool::To(std::string_view(GuidHex)));
            CreatePath(DescDir);
            const auto DescDirRelToProject = DescDir.substr(m_ProjectPaths.m_Project.length() + 1);
            const auto AssetRelPath        = std::format(L"{}/bitmap.png", DescDirRelToProject);
            const auto AssetFullPath       = std::format(L"{}/{}", m_ProjectPaths.m_Project, AssetRelPath);
            WritePng(AssetFullPath, AtlasWidth, AtlasHeight, 4, TexturePixels);

            {
                xtexture_rsc::descriptor TexDesc;
                TexDesc.m_UsageType     = xtexture_rsc::usage_type::COLOR_AND_ALPHA;
                TexDesc.m_InputVariant  = xtexture_rsc::single_input{ AssetRelPath };
                TexDesc.m_Compression   =
                      m_Descriptor.m_BitmapCompression == xfont_rsc::bitmap_compression::BC3_ALPHA ? xtexture_rsc::compression_format::RGBA_BC3_A8
                    : m_Descriptor.m_BitmapCompression == xfont_rsc::bitmap_compression::BC1_ALPHA ? xtexture_rsc::compression_format::RGBA_BC1_A1
                    : xtexture_rsc::compression_format::RGBA_UNCOMPRESSED;
                TexDesc.m_bSRGB          = false; // ordinary coverage data, not display color - runtime tint applies its own color
                TexDesc.m_bGenerateMips = false;  // a bitmap font isn't meant to be minified - each baked size IS its own "mip"
                TexDesc.m_Quality        = 1.0f;
                TexDesc.m_UWrap          = xtexture_rsc::wrap_type::CLAMP_TO_EDGE;
                TexDesc.m_VWrap          = xtexture_rsc::wrap_type::CLAMP_TO_EDGE;

                xproperty::settings::context Context{};
                TexDesc.Serialize(false, DescDir + L"/Descriptor.txt", Context);

                xresource_pipeline::info Info{ xresource::full_guid{ InstanceGuid, xtexture_rsc::resource_type_guid_v } };
                Info.m_Name = std::format("{} (bitmap)", xstrtool::To(std::wstring_view(m_Descriptor.m_FontFile)));
                // Same mechanism a regular asset uses to record which folder it lives in - see the
                // MTSDF/SDF EmitVirtualTexture lambda's own comment on this further down this file.
                Info.m_RscLinks.push_back(xresource::full_guid{ m_ResourceGuid, xfont_rsc::resource_type_guid_v });
                Info.Serialize(false, DescDir + L"/Info.txt", Context);

                // The PNG itself is NOT recorded here as one of the font's own m_VirtualAssets: it's
                // not something the font depends on (the font never reads it back), it's an input the
                // virtual texture resource depends on - xtexture_compiler already records it as ITS
                // OWN m_Dependencies.m_Assets when it compiles this descriptor, which is where an
                // inspector on the texture resource itself will correctly show it.
                m_Dependencies.m_VirtualResources.push_back(xresource::full_guid{ InstanceGuid, xtexture_rsc::resource_type_guid_v });
            }

            xrsc::texture_ref TextureRef{};
            TextureRef.m_Instance = InstanceGuid;

            displayProgressBar("Building glyph tables", 0.8f);

            // One CHD hash + glyph table PER size group, all appended into one contiguous blob
            // after the size_group_header array (see xfont_rsc_runtime.h's own layout comment).
            std::vector<xfont_rsc::size_group_header> Headers(SizeGroupRanges.size());
            std::vector<char> GroupBlobs;

            for (std::size_t GroupIndex = 0; GroupIndex < SizeGroupRanges.size(); ++GroupIndex)
            {
                const auto [First, Count] = SizeGroupRanges[GroupIndex];
                if (Count == 0)
                {
                    Headers[GroupIndex] = { static_cast<float>(m_Descriptor.m_BitmapSizes[GroupIndex]), 0u, 0u, 0u, static_cast<std::uint64_t>(GroupBlobs.size()) };
                    continue;
                }

                std::vector<std::uint32_t> GroupCodepoints(static_cast<std::size_t>(Count));
                for (int i = 0; i < Count; ++i) GroupCodepoints[static_cast<std::size_t>(i)] = RawGlyphs[static_cast<std::size_t>(First + i)].m_Codepoint;

                xfont::perfect_hash_build HashBuild;
                if (xfont::BuildPerfectHash(GroupCodepoints, HashBuild) == false)
                    return xerr::create_f<state, "Failed to build a perfect hash for one of the BITMAP size groups">();

                std::vector<xfont_rsc::glyph> GroupGlyphs(static_cast<std::size_t>(Count));
                std::vector<std::uint32_t> SlotToGlyph(HashBuild.m_SlotCount, 0xFFFFFFFFu);
                // ToFixed() packs into a Q4.12 int16_t - only +-8.0 of integer range (see its own
                // comment in xfont_rsc_runtime.h). Raw FreeType pixel values (bearings/advance can
                // run well past 8px even at modest baked sizes) silently overflow that encoding, so
                // - exactly as xfont_rsc_runtime.h's own comment on size_group already documents -
                // everything here must be normalized by this group's own baked pixel size FIRST,
                // matching how MTSDF/SDF's plane bounds are already em-fractions, not raw pixels.
                const float InvSize = 1.0f / static_cast<float>(m_Descriptor.m_BitmapSizes[GroupIndex]);
                for (int i = 0; i < Count; ++i)
                {
                    auto& RG = RawGlyphs[static_cast<std::size_t>(First + i)];
                    auto& G  = GroupGlyphs[static_cast<std::size_t>(i)];
                    G.m_Codepoint = RG.m_Codepoint;
                    if (RG.m_W > 0 && RG.m_H > 0)
                    {
                        const auto& R = PackRects[static_cast<std::size_t>(First + i)];
                        G.m_AtlasX = static_cast<std::uint16_t>(R.x);
                        G.m_AtlasY = static_cast<std::uint16_t>(R.y);
                        G.m_AtlasW = static_cast<std::uint16_t>(RG.m_W);
                        G.m_AtlasH = static_cast<std::uint16_t>(RG.m_H);
                    }
                    // bitmap_left/bitmap_top are FreeType's own pen-relative bearing, in pixels.
                    G.m_PlaneLeft   = xfont_rsc::ToFixed(RG.m_BearingX * InvSize);
                    G.m_PlaneTop    = xfont_rsc::ToFixed(RG.m_BearingY * InvSize);
                    G.m_PlaneRight  = xfont_rsc::ToFixed((RG.m_BearingX + RG.m_W) * InvSize);
                    G.m_PlaneBottom = xfont_rsc::ToFixed((RG.m_BearingY - RG.m_H) * InvSize);
                    G.m_Advance     = xfont_rsc::ToFixed(RG.m_Advance * InvSize);

                    const auto Bucket = xfont::PerfectHashBucket(G.m_Codepoint, HashBuild.m_BucketCount);
                    const auto Slot   = xfont::PerfectHashSlot(G.m_Codepoint, HashBuild.m_Displacement[Bucket], HashBuild.m_SlotCount);
                    SlotToGlyph[Slot] = static_cast<std::uint32_t>(i);
                }

                const std::uint64_t GroupOffset     = GroupBlobs.size();
                const std::size_t DisplacementBytes = sizeof(std::uint16_t) * HashBuild.m_Displacement.size();
                const std::size_t SlotBytes         = sizeof(std::uint32_t) * SlotToGlyph.size();
                const std::size_t GlyphBytes        = sizeof(xfont_rsc::glyph) * GroupGlyphs.size();
                GroupBlobs.resize(GroupBlobs.size() + DisplacementBytes + SlotBytes + GlyphBytes);
                char* pWrite = GroupBlobs.data() + GroupOffset;
                std::memcpy(pWrite, HashBuild.m_Displacement.data(), DisplacementBytes); pWrite += DisplacementBytes;
                std::memcpy(pWrite, SlotToGlyph.data(), SlotBytes);                     pWrite += SlotBytes;
                std::memcpy(pWrite, GroupGlyphs.data(), GlyphBytes);

                Headers[GroupIndex] = { static_cast<float>(m_Descriptor.m_BitmapSizes[GroupIndex]), static_cast<std::uint32_t>(Count), HashBuild.m_SlotCount, HashBuild.m_BucketCount, GroupOffset };
            }

            displayProgressBar("Serializing", 0.9f);

            xfont_rsc::font Font{};
            Font.m_OutputType         = xfont_rsc::output_type::BITMAP;
            Font.m_Texture            = TextureRef;
            Font.m_PixelRange         = 0.0f; // not a distance field
            Font.m_Ascender           = EmAscender;
            Font.m_Descender          = EmDescender;
            Font.m_LineHeight         = EmLineHeight;
            Font.m_UnderlineY         = 0.0f; // not tracked - a known, narrow gap (underline isn't drawn yet regardless of output type)
            Font.m_UnderlineThickness = 0.0f;
            Font.m_nGlyphs            = 0;
            Font.m_nKernPairs         = 0;
            Font.m_HashSlotCount      = 0;
            Font.m_HashBucketCount    = 0;
            Font.m_nSizeGroups        = static_cast<std::uint32_t>(Headers.size());

            const std::uint64_t HeaderBytes = sizeof(xfont_rsc::size_group_header) * Headers.size();
            Font.m_DataByteCount = HeaderBytes + GroupBlobs.size();
            std::vector<char> Blob(Font.m_DataByteCount);
            std::memcpy(Blob.data(), Headers.data(), HeaderBytes);
            if (!GroupBlobs.empty())
                std::memcpy(Blob.data() + HeaderBytes, GroupBlobs.data(), GroupBlobs.size());
            Font.m_pData = Blob.data();

            for (auto& T : m_Target)
            {
                if (T.m_bValid == false) continue;
                xserializer::stream Serializer;
                if (auto Err = Serializer.Save(T.m_DataPath, Font,
                        m_OptimizationType == optimization_type::O0 ? xserializer::compression_level::FAST
                      : m_OptimizationType == optimization_type::O1 ? xserializer::compression_level::MEDIUM
                      : xserializer::compression_level::HIGH); Err)
                    return Err;
            }

            displayProgressBar("Serializing", 1.0f);
            return {};
        }

        displayProgressBar("Packing atlas", 0.25f);

        //
        // Pack
        //
        // Dimensions are deliberately left unset (no setDimensions() call) - with a FIXED glyph
        // scale (setScale, not setMinimumScale - see below) and unset dimensions,
        // TightAtlasPacker::pack() computes the smallest fitting dimensions for every glyph at that
        // scale on its own (see its own pack()/tryPack() - width/height start at -1 and get filled
        // in by the search). There is nothing to size here: the user only controls how big each
        // character is (m_GlyphSize); the atlas texture is whatever that requires.
        //
        // setScale(), NOT setMinimumScale(): pack() only ever treats setMinimumScale()'s value as a
        // STARTING POINT for picking dimensions - once tryPack() succeeds at that scale, pack() falls
        // through to packAndScale() unconditionally whenever this->scale is still unset (<=0), which
        // does its own binary search for the LARGEST scale that still fits the just-chosen
        // dimensions and silently overrides whatever GlyphSize the user actually asked for. Confirmed
        // live: baked glyphs were visibly much bigger than the requested pixel size. setScale() fixes
        // the scale outright, so pack() takes its scale>0 branch and never reaches packAndScale() -
        // dimensions still auto-derive around the FIXED size instead of the size drifting to fill
        // whatever dimensions got picked.
        msdf_atlas::TightAtlasPacker Packer;
        Packer.setScale(static_cast<double>(m_Descriptor.m_GlyphSize));
        Packer.setPixelRange(msdfgen::Range(static_cast<double>(m_Descriptor.m_PixelRange)));
        Packer.setMiterLimit(1.0);
        // See m_PixelPadding's own comment (xfont_rsc_descriptor.h) for why this exists at all -
        // TightAtlasPacker's own default is 0, which let block compression bleed unrelated glyphs
        // together at atlas edges.
        Packer.setSpacing(m_Descriptor.m_PixelPadding);

        // Pass 1: POWER_OF_TWO_RECTANGLE gives a fast, GUARANTEED-to-fit upper bound (its own
        // built-in size selector only ever tries square and 2:1-rectangle power-of-two dimensions -
        // see size-selectors.h - so it can land as much as ~2x oversized on either axis versus the
        // true minimum, e.g. a charset that truly only needed ~200 tall was getting rounded up to a
        // full 256).
        Packer.setDimensionsConstraint(msdf_atlas::DimensionsConstraint::POWER_OF_TWO_RECTANGLE);
        if (const int Remaining = Packer.pack(Glyphs.data(), static_cast<int>(Glyphs.size())); Remaining != 0)
        {
            msdfgen::destroyFont(pFont);
            msdfgen::deinitializeFreetype(pFreetype);
            return Remaining < 0
                ? xerr::create_f<state, "Failed to pack the glyphs into the atlas">()
                : xerr::create_f<state, "Could not fit every glyph into the atlas - reduce GlyphSize or the charset">();
        }

        int AtlasWidth = 0, AtlasHeight = 0;
        Packer.getDimensions(AtlasWidth, AtlasHeight);

        // Pass 2: tighten each axis independently down to the smallest MULTIPLE-OF-4 size (the only
        // real constraint BC7/BC4 block compression imposes - mips are off, so there's no separate
        // power-of-two requirement) that still fits, via binary search using setDimensions() to force
        // an exact size and pack() to test it. Width first (height held at its already-fitting upper
        // bound), then height (against the now-tightened width) - each axis search is monotonic
        // (more room can only help a bin-pack fit, never hurt), so a plain binary search is valid.
        {
            auto Fits = [&](int W, int H) -> bool
            {
                Packer.setDimensions(W, H);
                return Packer.pack(Glyphs.data(), static_cast<int>(Glyphs.size())) == 0;
            };
            // Searches in units of 4px; UpperBound4 (already known to fit) anchors the upper end.
            auto TightenAxis = [&](int UpperBound, auto&& FitsAt) -> int
            {
                int Lo = 1, Hi = UpperBound / 4;
                while (Lo < Hi)
                {
                    const int Mid = Lo + (Hi - Lo) / 2;
                    if (FitsAt(Mid * 4)) Hi = Mid; else Lo = Mid + 1;
                }
                return Lo * 4;
            };

            const int TightW = TightenAxis(AtlasWidth,  [&](int W) { return Fits(W, AtlasHeight); });
            const int TightH = TightenAxis(AtlasHeight, [&](int H) { return Fits(TightW, H); });

            // Lock in final glyph placement at the tightened size - each probe above overwrote it.
            Fits(TightW, TightH);
            AtlasWidth  = TightW;
            AtlasHeight = TightH;
        }

        // Not a user-facing setting - just a sanity ceiling against a pathological charset/GlyphSize
        // combination producing a texture no real GPU could bind (8192 is a safe universal 2D
        // texture-dimension limit).
        constexpr int kMaxSafeAtlasDim = 8192;
        if (AtlasWidth > kMaxSafeAtlasDim || AtlasHeight > kMaxSafeAtlasDim)
        {
            msdfgen::destroyFont(pFont);
            msdfgen::deinitializeFreetype(pFreetype);
            return xerr::create_f<state, "The auto-sized atlas would exceed the 8192x8192 safety limit - reduce GlyphSize or the charset">();
        }
        LogMessage(msg_type::INFO, std::format("Atlas dimensions: {} x {} (auto-derived at GlyphSize={})", AtlasWidth, AtlasHeight, m_Descriptor.m_GlyphSize));

        displayProgressBar("Generating distance field", 0.4f);

        //
        // Generate the distance-field bitmap. MTSDF: 4-channel (RGB=median-reconstructible MSDF,
        // A=true SDF for effects) - the whole point of keeping all 4 channels in ONE texture now is
        // that neither piece is compressed any more (see this file's own top-of-descriptor comment),
        // so there's no compression-format reason left to split them into two virtual textures the
        // way this used to work. SDF: 1-channel true distance field, safe to compress (BC4) because
        // there's no cross-channel median to break.
        //
        msdf_atlas::GeneratorAttributes GenAttribs;
        GenAttribs.config.overlapSupport = true;  // msdfgen's own fallback for self-intersecting contours without Skia
        GenAttribs.scanlinePass          = true;  // ditto - error correction runs regardless (not optional, see design notes)
        // Error correction is what actually keeps the R/G/B channels mutually consistent (no
        // conflicting per-channel distances at a given pixel) for MTSDF - the CHD/median
        // reconstruction in the runtime shader is only as good as this pass. Harmless (a no-op cost)
        // for SDF's single channel, so left on unconditionally rather than branched.
        GenAttribs.config.errorCorrection.mode              = msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY;
        GenAttribs.config.errorCorrection.distanceCheckMode = msdfgen::ErrorCorrectionConfig::ALWAYS_CHECK_DISTANCE;

        const bool bIsMtsdf = m_Descriptor.m_OutputType == xfont_rsc::output_type::MTSDF;
        const int  TextureChannels = bIsMtsdf ? 4 : 1;
        std::vector<unsigned char> TexturePixels(static_cast<std::size_t>(AtlasWidth) * AtlasHeight * TextureChannels);

        if (bIsMtsdf)
        {
            msdf_atlas::ImmediateAtlasGenerator<float, 4, msdf_atlas::mtsdfGenerator, msdf_atlas::BitmapAtlasStorage<msdfgen::byte, 4>> Generator(AtlasWidth, AtlasHeight);
            Generator.setAttributes(GenAttribs);
            Generator.setThreadCount(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
            Generator.generate(Glyphs.data(), static_cast<int>(Glyphs.size()));

            auto Bitmap = static_cast<msdfgen::BitmapConstSection<msdfgen::byte, 4>>(Generator.atlasStorage());
            Bitmap.reorient(msdfgen::Y_DOWNWARD); // top-left origin, matching the atlas-pixel bounds convention used below
            for (int y = 0; y < AtlasHeight; ++y)
                for (int x = 0; x < AtlasWidth; ++x)
                {
                    const msdfgen::byte* pPixel = Bitmap(x, y);
                    auto* pOut = &TexturePixels[(static_cast<std::size_t>(y) * AtlasWidth + x) * 4];
                    pOut[0] = pPixel[0]; pOut[1] = pPixel[1]; pOut[2] = pPixel[2]; pOut[3] = pPixel[3];
                }
        }
        else // SDF
        {
            msdf_atlas::ImmediateAtlasGenerator<float, 1, msdf_atlas::sdfGenerator, msdf_atlas::BitmapAtlasStorage<msdfgen::byte, 1>> Generator(AtlasWidth, AtlasHeight);
            Generator.setAttributes(GenAttribs);
            Generator.setThreadCount(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
            Generator.generate(Glyphs.data(), static_cast<int>(Glyphs.size()));

            auto Bitmap = static_cast<msdfgen::BitmapConstSection<msdfgen::byte, 1>>(Generator.atlasStorage());
            Bitmap.reorient(msdfgen::Y_DOWNWARD);
            for (int y = 0; y < AtlasHeight; ++y)
                for (int x = 0; x < AtlasWidth; ++x)
                    TexturePixels[static_cast<std::size_t>(y) * AtlasWidth + x] = Bitmap(x, y)[0];
        }

        displayProgressBar("Writing atlas assets", 0.55f);

        //
        // Emit the virtual Texture resource - Descriptor.txt/Info.txt written via xtexture_rsc's
        // own descriptor type (guaranteed byte-compatible with what xtexture_compiler reads), GUID
        // derived deterministically from this font's own resource guid so recompiling the same font
        // always reuses the same virtual texture identity instead of orphaning a new one each time.
        //
        const auto FontGuidHex = std::format("{:016X}", m_ResourceGuid.m_Value);

        xrsc::texture_ref TextureRef{};

        auto EmitVirtualTexture = [&](std::string_view RoleSalt, xtexture_rsc::usage_type UsageType, xtexture_rsc::compression_format Compression
                                     , const std::vector<unsigned char>& Pixels, int Channels) -> xresource::instance_guid
        {
            const auto InstanceGuid = xresource::instance_guid::GenerateGUIDCopy(std::format("{}_{}", FontGuidHex, RoleSalt).c_str());
            const auto GuidValue    = InstanceGuid.m_Value;
            const auto Byte0        = std::format("{:02X}", (GuidValue) & 0xFF);
            const auto Byte1        = std::format("{:02X}", (GuidValue >> 8) & 0xFF);
            const auto GuidHex      = std::format("{:016X}", GuidValue);

            // This virtual texture's own descriptor folder - same GUID-sharded scheme every resource
            // uses (Cache/Descriptors/<Type>/<byte0>/<byte1>/<guidhex>.desc/). Computed before the PNG
            // is written so the PNG can live directly inside it instead of a shared Cache/Temp/Font
            // dumping ground: nothing but this one virtual resource ever references this PNG, so it
            // belongs with the descriptor that owns it (and gets cleaned up/relocated alongside it,
            // rather than orphaned in a scratch folder shared by every font in the project).
            const auto DescDir = std::format(L"{}/Texture/{}/{}/{}.desc", m_ProjectPaths.m_CachedDescriptors, xstrtool::To(std::string_view(Byte0)), xstrtool::To(std::string_view(Byte1)), xstrtool::To(std::string_view(GuidHex)));
            CreatePath(DescDir);

            // single_input paths are resolved relative to the project root (see xtexture_compiler.cpp's
            // own LoadTexture(..., m_ProjectPaths.m_Project + "/" + FileName)), so strip that prefix
            // back off DescDir rather than re-deriving the "Cache/Descriptors/..." literal by hand.
            const auto DescDirRelToProject = DescDir.substr(m_ProjectPaths.m_Project.length() + 1);
            const auto AssetRelPath  = std::format(L"{}/{}.png", DescDirRelToProject, xstrtool::To(std::string_view(RoleSalt)));
            const auto AssetFullPath = std::format(L"{}/{}", m_ProjectPaths.m_Project, AssetRelPath);
            WritePng(AssetFullPath, AtlasWidth, AtlasHeight, Channels, Pixels);

            xtexture_rsc::descriptor TexDesc;
            TexDesc.m_UsageType                = UsageType;
            TexDesc.m_InputVariant              = xtexture_rsc::single_input{ AssetRelPath };
            TexDesc.m_Compression               = Compression;
            TexDesc.m_bSRGB                      = false; // this is math data (distance field), never gamma-encoded
            TexDesc.m_bGenerateMips             = false; // mip-filtering would corrupt the distance field
            TexDesc.m_Quality                    = 1.0f; // max BC7/BC4 encoder quality - the default (0.5) is tuned for ordinary color/normal textures; MSDF/SDF data is far more sensitive to per-block quantization error than those are, since reconstruction depends on precise per-channel comparisons
            TexDesc.m_UWrap                      = xtexture_rsc::wrap_type::CLAMP_TO_EDGE;
            TexDesc.m_VWrap                      = xtexture_rsc::wrap_type::CLAMP_TO_EDGE;

            xproperty::settings::context Context{};
            TexDesc.Serialize(false, DescDir + L"/Descriptor.txt", Context);

            xresource_pipeline::info Info{ xresource::full_guid{ InstanceGuid, xtexture_rsc::resource_type_guid_v } };
            Info.m_Name = std::format("{} ({})", xstrtool::To(std::wstring_view(m_Descriptor.m_FontFile)), RoleSalt);
            // Same mechanism a regular asset uses to record which folder it lives in (see
            // AssetMgr::NewAsset's own m_RscLinks.push_back(ParentGUID)) - the Asset Browser already
            // inverts every resource's m_RscLinks into an in-memory parent->children index at scan
            // time, so linking back to the font here is the ONLY change needed for this virtual
            // texture to show up as the font's child in the browser - no browser-side schema change.
            Info.m_RscLinks.push_back(xresource::full_guid{ m_ResourceGuid, xfont_rsc::resource_type_guid_v });
            Info.Serialize(false, DescDir + L"/Info.txt", Context);

            // The PNG itself is NOT recorded here as one of the font's own m_VirtualAssets - see the
            // BITMAP branch's own comment on this further up this file for why.
            m_Dependencies.m_VirtualResources.push_back(xresource::full_guid{ InstanceGuid, xtexture_rsc::resource_type_guid_v });

            return InstanceGuid;
        };

        // Neither mode compresses any more - MTSDF never did (multi-channel median reconstruction is
        // far too sensitive to per-block quantization error), and SDF's own BC4 option turned out to
        // have the same problem just less severely (confirmed by direct visual comparison against the
        // atlas, not merely theoretical - see this file's own history). R_UNCOMPRESSED is the genuine
        // single-channel 8bpp format most engines use for a raw SDF atlas - 4x R_BC4's memory, but
        // exact, unlike RGBA_UNCOMPRESSED (32bpp) which would waste 3 more channels on top of that for
        // data that only ever had one.
        TextureRef.m_Instance = bIsMtsdf
            ? EmitVirtualTexture("texture", xtexture_rsc::usage_type::COLOR_AND_ALPHA, xtexture_rsc::compression_format::RGBA_UNCOMPRESSED, TexturePixels, 4)
            : EmitVirtualTexture("texture", xtexture_rsc::usage_type::INTENSITY, xtexture_rsc::compression_format::R_UNCOMPRESSED, TexturePixels, 1);

        displayProgressBar("Building glyph table", 0.7f);

        //
        // Build the compact runtime glyph table + kerning table + perfect hash
        //
        std::vector<std::uint32_t> Codepoints;
        Codepoints.reserve(Glyphs.size());
        for (auto& G : Glyphs) Codepoints.push_back(G.getCodepoint());

        xfont::perfect_hash_build HashBuild;
        if (xfont::BuildPerfectHash(Codepoints, HashBuild) == false)
        {
            msdfgen::destroyFont(pFont);
            msdfgen::deinitializeFreetype(pFreetype);
            return xerr::create_f<state, "Failed to build a perfect hash for this charset (this should not happen for realistic charset sizes)">();
        }

        std::vector<xfont_rsc::glyph> RuntimeGlyphs(Glyphs.size());
        std::vector<std::uint32_t> SlotToGlyph(HashBuild.m_SlotCount, 0xFFFFFFFFu);
        for (std::size_t i = 0; i < Glyphs.size(); ++i)
        {
            auto& G = Glyphs[i];
            double L, B, R, T;
            G.getQuadAtlasBounds(L, B, R, T);
            auto& RG = RuntimeGlyphs[i];
            RG.m_Codepoint = G.getCodepoint();
            RG.m_AtlasX = static_cast<std::uint16_t>(std::lround(L));
            // getQuadAtlasBounds is Y-up (bottom-origin, meant for direct OpenGL-style UVs), but the
            // atlas bitmap above is written row-by-row (y=0 first) straight into a top-down PNG/texture -
            // so B/T need flipping into top-down pixel rows here, or every glyph samples vertically
            // mirrored/misaligned relative to its neighbors once packed into one shared atlas.
            RG.m_AtlasY = static_cast<std::uint16_t>(std::lround(AtlasHeight - T));
            RG.m_AtlasW = static_cast<std::uint16_t>(std::lround(R - L));
            RG.m_AtlasH = static_cast<std::uint16_t>(std::lround(T - B));

            G.getQuadPlaneBounds(L, B, R, T);
            RG.m_PlaneLeft   = xfont_rsc::ToFixed(L);
            RG.m_PlaneBottom = xfont_rsc::ToFixed(B);
            RG.m_PlaneRight  = xfont_rsc::ToFixed(R);
            RG.m_PlaneTop    = xfont_rsc::ToFixed(T);
            RG.m_Advance     = xfont_rsc::ToFixed(G.getAdvance());

            const auto Bucket = xfont::PerfectHashBucket(RG.m_Codepoint, HashBuild.m_BucketCount);
            const auto Slot   = xfont::PerfectHashSlot(RG.m_Codepoint, HashBuild.m_Displacement[Bucket], HashBuild.m_SlotCount);
            SlotToGlyph[Slot] = static_cast<std::uint32_t>(i);
        }

        std::map<std::uint32_t, std::uint32_t> GlyphIndexByFontIndex;
        for (std::size_t i = 0; i < Glyphs.size(); ++i)
            GlyphIndexByFontIndex[static_cast<std::uint32_t>(Glyphs[i].getIndex())] = static_cast<std::uint32_t>(i);

        std::vector<xfont_rsc::kern_pair> KernPairs;
        for (auto& [Pair, Adjust] : FontGeometry.getKerning())
        {
            auto ItL = GlyphIndexByFontIndex.find(static_cast<std::uint32_t>(Pair.first));
            auto ItR = GlyphIndexByFontIndex.find(static_cast<std::uint32_t>(Pair.second));
            if (ItL == GlyphIndexByFontIndex.end() || ItR == GlyphIndexByFontIndex.end()) continue;
            xfont_rsc::kern_pair KP;
            KP.m_Left   = Glyphs[ItL->second].getCodepoint();
            KP.m_Right  = Glyphs[ItR->second].getCodepoint();
            KP.m_Adjust = xfont_rsc::ToFixed(Adjust);
            KernPairs.push_back(KP);
        }
        std::sort(KernPairs.begin(), KernPairs.end(), [](auto& A, auto& B)
        {
            return A.m_Left != B.m_Left ? A.m_Left < B.m_Left : A.m_Right < B.m_Right;
        });

        const auto& Metrics = FontGeometry.getMetrics();

        msdfgen::destroyFont(pFont);
        msdfgen::deinitializeFreetype(pFreetype);

        //
        // Assemble the final runtime resource
        //
        xfont_rsc::font Font{};
        Font.m_OutputType           = m_Descriptor.m_OutputType;
        Font.m_Texture              = TextureRef;
        Font.m_PixelRange           = m_Descriptor.m_PixelRange;
        Font.m_LineHeight           = static_cast<float>(Metrics.lineHeight);
        Font.m_Ascender             = static_cast<float>(Metrics.ascenderY);
        Font.m_Descender            = static_cast<float>(Metrics.descenderY);
        Font.m_UnderlineY           = static_cast<float>(Metrics.underlineY);
        Font.m_UnderlineThickness   = static_cast<float>(Metrics.underlineThickness);
        Font.m_nGlyphs              = static_cast<std::uint32_t>(RuntimeGlyphs.size());
        Font.m_nKernPairs           = static_cast<std::uint32_t>(KernPairs.size());
        Font.m_HashSlotCount        = HashBuild.m_SlotCount;
        Font.m_HashBucketCount      = HashBuild.m_BucketCount;
        Font.m_nSizeGroups          = 0; // BITMAP only - unreachable here (see the earlier not-implemented-yet guard)

        const std::uint64_t DataBytes = std::uint64_t(sizeof(std::uint16_t)) * HashBuild.m_Displacement.size()
                                       + std::uint64_t(sizeof(std::uint32_t)) * SlotToGlyph.size()
                                       + std::uint64_t(sizeof(xfont_rsc::glyph)) * RuntimeGlyphs.size()
                                       + std::uint64_t(sizeof(xfont_rsc::kern_pair)) * KernPairs.size();
        Font.m_DataByteCount = DataBytes;
        std::vector<char> Blob(DataBytes);
        char* pWrite = Blob.data();
        std::memcpy(pWrite, HashBuild.m_Displacement.data(), sizeof(std::uint16_t) * HashBuild.m_Displacement.size()); pWrite += sizeof(std::uint16_t) * HashBuild.m_Displacement.size();
        std::memcpy(pWrite, SlotToGlyph.data(), sizeof(std::uint32_t) * SlotToGlyph.size());   pWrite += sizeof(std::uint32_t) * SlotToGlyph.size();
        std::memcpy(pWrite, RuntimeGlyphs.data(), sizeof(xfont_rsc::glyph) * RuntimeGlyphs.size()); pWrite += sizeof(xfont_rsc::glyph) * RuntimeGlyphs.size();
        std::memcpy(pWrite, KernPairs.data(), sizeof(xfont_rsc::kern_pair) * KernPairs.size());
        Font.m_pData = Blob.data();

        displayProgressBar("Serializing", 0.9f);

        for (auto& T : m_Target)
        {
            if (T.m_bValid == false) continue;

            xserializer::stream Serializer;
            if (auto Err = Serializer.Save(T.m_DataPath, Font,
                    m_OptimizationType == optimization_type::O0 ? xserializer::compression_level::FAST
                  : m_OptimizationType == optimization_type::O1 ? xserializer::compression_level::MEDIUM
                  : xserializer::compression_level::HIGH); Err)
                return Err;
        }

        displayProgressBar("Serializing", 1.0f);
        return {};
    }
};

//---------------------------------------------------------------------------------------------

namespace xfont_compiler
{
    std::unique_ptr<instance> instance::Create(void)
    {
        return std::make_unique<implementation>();
    }
}
