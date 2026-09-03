#ifndef XFONT_XGPU_XRSC_GUID_LOADER_H
#define XFONT_XGPU_XRSC_GUID_LOADER_H
#pragma once

#include "xfont_rsc_runtime.h"
#include "dependencies/xresource_mgr/source/xresource_mgr.h"

namespace xrsc
{
    inline static constexpr auto font_type_guid_v = xresource::type_guid(xresource::guid_generator::Instance64FromString("font"));
    using                        font_ref         = xresource::def_guid<font_type_guid_v>;
}

namespace xgpu { struct texture; }

namespace xfont
{
    // Runtime handle handed to game/editor code. Deliberately holds a pointer to the loaded
    // xfont_rsc::font blob (kept alive for the resource's whole lifetime) rather than copying/
    // re-deriving from it, since the blob's own m_pData is one contiguous allocation shared with
    // the header (xserializer's own "single block, single free" convention) - copying the header
    // and freeing the original would free the blob out from under the copy. The texture pointer is
    // resolved once at Load time through the normal xresource::mgr reference mechanism, so GPU
    // upload/caching is fully shared with every other texture consumer. What the texture actually
    // holds (MSDF+SDF in RGBA, a plain SDF in R, or a bitmap glyph atlas in RGBA) depends on
    // m_pFont->m_OutputType - there's only ever one texture now, for any output type.
    struct rt
    {
        xfont_rsc::font* m_pFont{ nullptr };
        xgpu::texture*   m_pTexture{ nullptr };

        // Captured at Load time, BEFORE m_pFont->m_Texture ever gets passed to Mgr.getResource() -
        // xresource::mgr::getResource(def_guid&) mutates its argument in place, overwriting
        // m_Instance with a raw pointer once resolved (a self-caching optimization, see its own
        // comment), so m_pFont->m_Texture is NOT a real GUID any more once this resource is loaded.
        // A caller that needs the compiled texture's own file path later (e.g. to check its
        // timestamp) must use this, never re-derive from the (by-then-mutated) ref field - doing so
        // once already tripped xresource_mgr.h's own isPointer()==false assert.
        std::wstring m_TextureResourcePath{};

        const xfont_rsc::glyph* FindGlyph(std::uint32_t Codepoint) const noexcept { return m_pFont->FindGlyph(Codepoint); }
        std::int16_t FindKernAdjust(std::uint32_t Left, std::uint32_t Right) const noexcept { return m_pFont->FindKernAdjust(Left, Right); }
    };
}

template<>
struct xresource::loader< xrsc::font_type_guid_v >
{
    constexpr static inline auto         type_name_v        = L"Font";
    constexpr static inline auto         use_death_march_v  = false;
    using                                data_type          = xfont::rt;
    static data_type*                    Load               ( xresource::mgr& Mgr, const full_guid& GUID );
    static void                          Destroy            ( xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID );
};

#endif
