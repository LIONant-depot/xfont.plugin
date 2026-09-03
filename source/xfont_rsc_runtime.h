#ifndef XFONT_RSC_RUNTIME_H
#define XFONT_RSC_RUNTIME_H
#pragma once

// Compact runtime binary format for a compiled Font resource - same discipline as xbitmap (fixed,
// size-pinned header) and xgeom_static::geom (one contiguous heap blob, sub-spans addressed by byte
// offset rather than separate allocations). The atlas pixels themselves are NOT stored here - this
// struct only ever holds glyph metrics/kerning/hash-table data plus a reference to the virtual
// Texture resource the font compiler emitted (see xfont_rsc_descriptor.h's own top comment).
#include "xfont_perfect_hash.h"
#include "dependencies/xresource_mgr/source/xresource_mgr.h"
#include <cmath>
#include <algorithm>
#include <utility>

// Re-declared locally rather than including xtexture's own loader header - each resource plugin
// is its own self-contained DLL/exe, so a cross-plugin type-guid reference is always re-derived
// from the same deterministic hash rather than shared via an #include, exactly like every other
// resource-to-resource reference in this codebase (e.g. xgeom_static's own material_instance_ref).
// Guarded on a shared macro (see xtexture_xgpu_rsc_loader.h's matching comment) so an editor that
// includes both this header and xtexture's own loader header in one translation unit - as
// E28_FontResourcePipeline.cpp does - doesn't hit a duplicate-definition error.
#ifndef XRSC_TEXTURE_TYPE_GUID_V_DECLARED
#define XRSC_TEXTURE_TYPE_GUID_V_DECLARED
namespace xrsc
{
    inline static constexpr auto texture_type_guid_v = xresource::type_guid(xresource::guid_generator::Instance64FromString("texture"));
    using                        texture_ref         = xresource::def_guid<texture_type_guid_v>;
}
#endif

namespace xfont_rsc
{
    // Declared here (not in xfont_rsc_descriptor.h) because this header is the more fundamental of
    // the two - a shipped game loading a compiled font needs it, nothing needs the descriptor past
    // compile time - so the descriptor includes THIS header and reuses this type directly for its
    // own OutputType field, rather than each declaring an independent copy. Unlike
    // texture_type_guid_v below, this isn't a cross-plugin boundary (descriptor and runtime are the
    // same plugin), so there's no reason to duplicate it.
    enum class output_type : std::uint8_t { MTSDF, SDF, BITMAP };

    // One baked glyph - fixed size, quantized. Plane bounds/advance are in fixed-point em units
    // (Q_FIXED_SHIFT_V fractional bits) - a signed 16-bit value at a 12-bit fraction covers roughly
    // +-8 em, comfortably beyond any real font's ascender/descender/tail extents. Atlas bounds are
    // plain pixel coordinates into the atlas texture. For BITMAP mode, "em units" don't apply in the
    // usual scalable sense - plane bounds/advance are still stored the same fixed-point way, just
    // relative to that glyph's own baked pixel size (see size_group::m_PixelSize), not a shared em.
    inline constexpr int    Q_FIXED_SHIFT_V = 12;
    inline constexpr float  Q_FIXED_SCALE_V = float(1 << Q_FIXED_SHIFT_V);

    inline std::int16_t ToFixed(double V) noexcept { return static_cast<std::int16_t>(std::lround(V * Q_FIXED_SCALE_V)); }
    inline float         FromFixed(std::int16_t V) noexcept { return static_cast<float>(V) / Q_FIXED_SCALE_V; }

    struct glyph
    {
        std::uint32_t m_Codepoint{};                                        // hash-slot verification key
        std::uint16_t m_AtlasX{}, m_AtlasY{}, m_AtlasW{}, m_AtlasH{};        // pixels, into the atlas texture
        std::int16_t  m_PlaneLeft{}, m_PlaneBottom{}, m_PlaneRight{}, m_PlaneTop{}; // fixed-point em units
        std::int16_t  m_Advance{};                                          // fixed-point em units
    };
    static_assert(sizeof(glyph) == 24, "glyph must stay exactly 24 bytes - update this alongside any field change");

    // Sparse, sorted by (m_Left,m_Right) - kerning pairs are a small fraction of glyphPairs^2, so a
    // binary search here is simpler than a second perfect hash and costs nothing measurable (kerning
    // lookups happen once per adjacent glyph pair during text layout, not per frame/per pixel).
    // MTSDF/SDF only - BITMAP mode doesn't bake kerning (m_nKernPairs is always 0 there).
    struct kern_pair
    {
        std::uint32_t m_Left{}, m_Right{};  // codepoints
        std::int16_t  m_Adjust{};           // fixed-point em units, added to m_Left's advance
    };
    static_assert(sizeof(kern_pair) == 12, "kern_pair must stay exactly 12 bytes - update this alongside any field change");

    // BITMAP mode only: one baked point-size's worth of glyphs gets its own perfect hash + glyph
    // table, since a (codepoint,size) pair would otherwise need widening xfont_perfect_hash.h's key
    // - reusing it unchanged, once per size, is simpler and keeps that file untouched. m_ByteOffset
    // is this group's own sub-blob's start within font::m_pData, so a lookup only ever needs one
    // header array scan (linear - there are at most a handful of sizes) plus one direct jump, not a
    // running sum of every earlier group's size.
    struct size_group_header
    {
        float         m_PixelSize{};
        std::uint32_t m_nGlyphs{};
        std::uint32_t m_HashSlotCount{};
        std::uint32_t m_HashBucketCount{};
        std::uint64_t m_ByteOffset{};        // offset from the START of the size-group region in m_pData
    };

    // Fixed header - always present regardless of glyph/kern-pair/hash-table/size-group counts,
    // which follow as one contiguous variable-length blob (see the layout comment on m_pData below).
    struct font
    {
        constexpr static std::uint16_t xserializer_version_v = 2; // v2: single m_Texture (was two), m_OutputType, optional BITMAP size groups

        output_type       m_OutputType{};
        xrsc::texture_ref m_Texture{};    // always present - the one atlas/SDF/bitmap texture, meaning depends on m_OutputType

        float m_PixelRange{};             // MTSDF/SDF only - must match what the runtime shader assumes for AA width
        float m_LineHeight{};
        float m_Ascender{};
        float m_Descender{};
        float m_UnderlineY{};
        float m_UnderlineThickness{};

        // MTSDF/SDF (m_nSizeGroups==0): exactly the glyph/kern/hash table described by these fields,
        // living directly in m_pData - unchanged shape from before. BITMAP (m_nSizeGroups>0): these
        // top-level fields are all 0; look up a size_group instead (see SizeGroups()/FindSizeGroup()).
        std::uint32_t m_nGlyphs{};
        std::uint32_t m_nKernPairs{};
        std::uint32_t m_HashSlotCount{};
        std::uint32_t m_HashBucketCount{};

        std::uint32_t m_nSizeGroups{};    // BITMAP only; 0 for MTSDF/SDF

        // Total byte size of the variable blob below, computed directly by the compiler (writer)
        // rather than re-derived here from the other count fields - BITMAP mode's total size depends
        // on EACH size_group's own nested counts, which aren't reachable from top-level scalars
        // alone the way the flat MTSDF/SDF layout's total was. See xfont_to_xserializer.h - it uses
        // this field directly instead of recomputing, for both modes uniformly.
        std::uint64_t m_DataByteCount{};

        // Variable section, one contiguous blob, addressed by byte offset from m_pData:
        //   MTSDF/SDF (m_nSizeGroups==0):
        //     std::uint16_t m_HashBucketCount entries : CHD displacement per bucket
        //     std::uint32_t m_HashSlotCount    entries : slot -> glyph index (0xFFFFFFFF = empty slot)
        //     glyph          m_nGlyphs      entries
        //     kern_pair      m_nKernPairs   entries (sorted by (m_Left,m_Right))
        //   BITMAP (m_nSizeGroups>0):
        //     size_group_header m_nSizeGroups entries, THEN for each group in order, at that
        //     group's own m_ByteOffset (relative to right after the header array):
        //       std::uint16_t Displacement[group.m_HashBucketCount]
        //       std::uint32_t SlotToGlyph[group.m_HashSlotCount]
        //       glyph         Glyphs[group.m_nGlyphs]
        //     (no kerning for BITMAP)
        char* m_pData{};

        std::uint16_t*  Displacement() const noexcept { return reinterpret_cast<std::uint16_t*>(m_pData); }
        std::uint32_t*  SlotToGlyph()  const noexcept { return reinterpret_cast<std::uint32_t*>(m_pData + sizeof(std::uint16_t) * m_HashBucketCount); }
        glyph*          Glyphs()       const noexcept { return reinterpret_cast<glyph*>(reinterpret_cast<char*>(SlotToGlyph()) + sizeof(std::uint32_t) * m_HashSlotCount); }
        kern_pair*      KernPairs()    const noexcept { return reinterpret_cast<kern_pair*>(reinterpret_cast<char*>(Glyphs()) + sizeof(glyph) * m_nGlyphs); }

        size_group_header* SizeGroups() const noexcept { return reinterpret_cast<size_group_header*>(m_pData); }
        char* SizeGroupRegionStart() const noexcept { return m_pData + sizeof(size_group_header) * m_nSizeGroups; }

        // BITMAP only - picks the baked size closest to Requested (fixed-size raster fonts can't
        // scale, so this is "best available" rather than exact unless the caller happens to match).
        const size_group_header* FindClosestSizeGroup(float Requested) const noexcept
        {
            if (m_nSizeGroups == 0) return nullptr;
            const size_group_header* pBest = &SizeGroups()[0];
            float BestDelta = std::abs(pBest->m_PixelSize - Requested);
            for (std::uint32_t i = 1; i < m_nSizeGroups; ++i)
            {
                const float Delta = std::abs(SizeGroups()[i].m_PixelSize - Requested);
                if (Delta < BestDelta) { BestDelta = Delta; pBest = &SizeGroups()[i]; }
            }
            return pBest;
        }

        // O(1) codepoint -> glyph lookup via the CHD perfect hash; codepoints outside the baked set
        // are guaranteed to either land on a genuinely empty slot or a slot whose stored glyph's own
        // m_Codepoint disagrees (the hash function only promises uniqueness WITHIN the baked set) -
        // both cases are treated as "not found" here. MTSDF/SDF only - use FindGlyphInSizeGroup for
        // BITMAP.
        const glyph* FindGlyph(std::uint32_t Codepoint) const noexcept
        {
            const auto Bucket      = xfont::PerfectHashBucket(Codepoint, m_HashBucketCount);
            const auto Displace    = Displacement()[Bucket];
            const auto Slot        = xfont::PerfectHashSlot(Codepoint, Displace, m_HashSlotCount);
            const auto GlyphIndex  = SlotToGlyph()[Slot];
            if (GlyphIndex == 0xFFFFFFFFu) return nullptr;
            const glyph* pGlyph = &Glyphs()[GlyphIndex];
            return pGlyph->m_Codepoint == Codepoint ? pGlyph : nullptr;
        }

        // BITMAP only - same CHD lookup as FindGlyph, scoped to one size_group's own sub-blob.
        static const glyph* FindGlyphInSizeGroup(const char* pGroupRegionStart, const size_group_header& Group, std::uint32_t Codepoint) noexcept
        {
            const auto* pDisplacement = reinterpret_cast<const std::uint16_t*>(pGroupRegionStart + Group.m_ByteOffset);
            const auto* pSlotToGlyph  = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(pDisplacement) + sizeof(std::uint16_t) * Group.m_HashBucketCount);
            const auto* pGlyphs       = reinterpret_cast<const glyph*>(reinterpret_cast<const char*>(pSlotToGlyph) + sizeof(std::uint32_t) * Group.m_HashSlotCount);

            const auto Bucket     = xfont::PerfectHashBucket(Codepoint, Group.m_HashBucketCount);
            const auto Displace   = pDisplacement[Bucket];
            const auto Slot       = xfont::PerfectHashSlot(Codepoint, Displace, Group.m_HashSlotCount);
            const auto GlyphIndex = pSlotToGlyph[Slot];
            if (GlyphIndex == 0xFFFFFFFFu) return nullptr;
            const glyph* pGlyph = &pGlyphs[GlyphIndex];
            return pGlyph->m_Codepoint == Codepoint ? pGlyph : nullptr;
        }

        // BITMAP only - the raw Glyphs[] array for one size_group (Group.m_nGlyphs entries), for
        // callers that want to enumerate every glyph rather than look one up by codepoint (e.g. a
        // debug overlay drawing every packed atlas rect). Same pointer arithmetic as
        // FindGlyphInSizeGroup, just skipping the hash lookup.
        static const glyph* GlyphsInSizeGroup(const char* pGroupRegionStart, const size_group_header& Group) noexcept
        {
            const auto* pDisplacement = reinterpret_cast<const std::uint16_t*>(pGroupRegionStart + Group.m_ByteOffset);
            const auto* pSlotToGlyph  = reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(pDisplacement) + sizeof(std::uint16_t) * Group.m_HashBucketCount);
            return reinterpret_cast<const glyph*>(reinterpret_cast<const char*>(pSlotToGlyph) + sizeof(std::uint32_t) * Group.m_HashSlotCount);
        }

        // Binary search - see kern_pair's own comment on why this is sorted-search rather than hashed.
        std::int16_t FindKernAdjust(std::uint32_t Left, std::uint32_t Right) const noexcept
        {
            if (m_nSizeGroups != 0) return 0; // BITMAP - no kerning baked
            auto* pBegin = KernPairs();
            auto* pEnd   = pBegin + m_nKernPairs;
            auto  It     = std::lower_bound(pBegin, pEnd, std::pair{ Left, Right }, [](const kern_pair& K, const std::pair<std::uint32_t,std::uint32_t>& V)
            {
                return K.m_Left != V.first ? K.m_Left < V.first : K.m_Right < V.second;
            });
            return (It != pEnd && It->m_Left == Left && It->m_Right == Right) ? It->m_Adjust : std::int16_t{0};
        }
    };
}
#endif
