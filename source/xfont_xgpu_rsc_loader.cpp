#include "xfont_xgpu_rsc_loader.h"
#include "bridges/xserializer/xfont_to_xserializer.h"
#include "dependencies/xresource_guid/source/bridges/xresource_xproperty_bridge.h"
#include <filesystem>
#include <cstdio>

//
// We will register the loader, the properties,
//
inline static auto s_FontRegistrations = xresource::common_registrations<xrsc::font_type_guid_v>{};

//------------------------------------------------------------------

xresource::loader< xrsc::font_type_guid_v >::data_type* xresource::loader< xrsc::font_type_guid_v >::Load( xresource::mgr& Mgr, const full_guid& GUID )
{
    std::wstring Path = Mgr.getResourcePath(GUID, type_name_v);

    xfont_rsc::font* pFont = nullptr;
    xserializer::stream Stream;
    if (auto Err = Stream.Load(Path, pFont); Err)
    {
        assert(false);
    }

    auto* pRt = new xfont::rt{};
    pRt->m_pFont = pFont;

    // The virtual texture this font's own compile emitted may not have been compiled yet - the
    // compile-queue cascades it automatically, but a resource compiled before that cascade existed
    // (or whose compile is still pending) genuinely has no binary on disk yet. Check existence
    // rather than asserting: the caller sees a null texture and can show "not compiled yet", same
    // as the editor already does for the font itself.
    if (pFont->m_Texture.empty() == false)
    {
        const xresource::full_guid TextureGuid{ pFont->m_Texture.m_Instance, pFont->m_Texture.m_Type };
        pRt->m_TextureResourcePath = Mgr.getResourcePath(TextureGuid, L"Texture");
        if (std::filesystem::exists(pRt->m_TextureResourcePath))
            pRt->m_pTexture = Mgr.getResource(pFont->m_Texture);
    }

    return pRt;
}

//------------------------------------------------------------------

void xresource::loader< xrsc::font_type_guid_v >::Destroy(xresource::mgr& Mgr, data_type&& Data, const full_guid& GUID)
{
    if (Data.m_pFont->m_Texture.empty() == false) Mgr.ReleaseRef(Data.m_pFont->m_Texture);

    xserializer::default_memory_handler_v.Free( xserializer::mem_type{ .m_bUnique = true }, Data.m_pFont );

    delete &Data;
}
