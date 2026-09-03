#ifndef XFONT_TO_XSERIALIZER_H
#define XFONT_TO_XSERIALIZER_H
#pragma once
#include "../../xfont_rsc_runtime.h"
#include "dependencies/xserializer/source/xserializer.h"

// Same pattern as xbitmap's own bridge (xbitmap_to_xserializer.h): one blob for the variable-length
// section (allocated fresh on read via mem_type::m_bUnique, already-owned on write), then every
// fixed scalar/sub-struct field individually - no field-order requirement, xserializer handles that.
namespace xserializer::io_functions
{
    template<> inline
    xerr SerializeIO<xfont_rsc::font>(xserializer::stream& Stream, const xfont_rsc::font& Font) noexcept
    {
        auto& F = const_cast<xfont_rsc::font&>(Font);

        // Stream.Serialize() computes an offset of each field's address RELATIVE TO the object being
        // serialized (isLocalVariable() asserts the address falls within [&F, &F+sizeof(Font)) - not
        // a check for C++ stack-locality despite the name) - a standalone local copy (e.g. of the
        // enum, cast to a byte "for safety") fails that check since it lives outside F entirely.
        // Every field serialized here must be the real member, in place - xserializer supports
        // enums natively (is_enum_v is one of its accepted scalar types), so no cast is needed
        // anyway.
        xerr Err;
        false
        || (Err = Stream.Serialize(F.m_OutputType))
        || (Err = Stream.Serialize(F.m_Texture.m_Instance.m_Value))
        || (Err = Stream.Serialize(F.m_PixelRange))
        || (Err = Stream.Serialize(F.m_LineHeight))
        || (Err = Stream.Serialize(F.m_Ascender))
        || (Err = Stream.Serialize(F.m_Descender))
        || (Err = Stream.Serialize(F.m_UnderlineY))
        || (Err = Stream.Serialize(F.m_UnderlineThickness))
        || (Err = Stream.Serialize(F.m_nGlyphs))
        || (Err = Stream.Serialize(F.m_nKernPairs))
        || (Err = Stream.Serialize(F.m_HashSlotCount))
        || (Err = Stream.Serialize(F.m_HashBucketCount))
        || (Err = Stream.Serialize(F.m_nSizeGroups))
        || (Err = Stream.Serialize(F.m_DataByteCount))
        || (Err = Stream.Serialize(F.m_pData, F.m_DataByteCount, mem_type{ .m_bUnique = true }))
        ;

        return Err;
    }
}
#endif
