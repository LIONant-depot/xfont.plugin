#ifndef XFONT_COMPILER_H
#define XFONT_COMPILER_H
#pragma once

// This is a headless CLI compiler (no ImGui) - skip xresource_pipeline.h's own ImGui-UI property
// stub include (XRESOURCE_PIPELINE_NO_COMPILER is set as a target-wide compile definition in
// build/dependency/CMakeLists.txt, since xresource_pipeline_v2's own .cpp needs it too). See that
// CMake file's comment for why: xtexture_compiler doesn't define this, but its own already-built
// .exe predates a later upstream xproperty change that removed that stub header entirely -
// rebuilding it fresh today would need the same define.
#include "dependencies/xresource_pipeline_v2/source/xresource_pipeline.h"

namespace xfont_compiler
{
    enum class state : std::uint32_t
    { OK
    , FAILURE
    };

    struct instance : xresource_pipeline::compiler::base
    {
        static std::unique_ptr<instance> Create(void);
    };
}

#endif
