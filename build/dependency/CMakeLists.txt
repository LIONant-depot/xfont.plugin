include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# ---------------------------------------------------------------------------
# LIONant-family dependencies (each ships its own build/dependency/CMakeLists.txt,
# which FetchAndPopulate add_subdirectory's automatically)
# ---------------------------------------------------------------------------
FetchAndPopulate("https://github.com/LIONant-depot/xserializer.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_pipeline_v2.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xresource_mgr.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xstrtool.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xmath.git" "main")

# stb (header-only, no build/dependency/CMakeLists.txt of its own - FetchAndPopulate
# just clones the raw source into dependencies/stb; we add its include path ourselves below)
FetchAndPopulate("https://github.com/nothings/stb.git" "master")

# ---------------------------------------------------------------------------
# FreeType - fetched as a plain CMake subdirectory (not vcpkg). msdfgen/msdf-atlas-gen
# both do `if(NOT TARGET Freetype::Freetype) find_package(Freetype REQUIRED) endif()` -
# pre-creating the alias here makes that check succeed without needing vcpkg or a
# system install. PNG/HarfBuzz/Brotli/Bzip2 support disabled - none of it is needed
# just to rasterize glyph outlines.
# ---------------------------------------------------------------------------
set(FT_DISABLE_ZLIB     OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
  freetype
  GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
  GIT_TAG        VER-2-13-3
  GIT_SHALLOW    TRUE
  SOURCE_DIR     "${CMAKE_SOURCE_DIR}/dependencies/freetype"
)
FetchContent_MakeAvailable(freetype)
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS freetype)
endif()

# ---------------------------------------------------------------------------
# msdf-atlas-gen (MIT) - brings its own msdfgen (MIT) submodule along via
# GIT_SUBMODULES_RECURSE. Skia/vcpkg disabled (not needed - Skia is only an
# enhanced geometry-preprocessing option, msdfgen's own preprocessing is used
# instead); standalone CLI + Artery Font export disabled (we drive the library
# API directly and write our own runtime format); msdfgen's own PNG support
# disabled (we write PNGs ourselves via stb_image_write.h, avoiding a libpng/
# zlib-discovery dependency entirely).
# ---------------------------------------------------------------------------
set(MSDF_ATLAS_DYNAMIC_RUNTIME  ON  CACHE BOOL "" FORCE) # match this project's own default /MD(d) rather than msdfgen/msdf-atlas-gen's own static-CRT default
set(MSDF_ATLAS_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_USE_SKIA         OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_NO_ARTERY_FONT   ON  CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG         ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
  msdf-atlas-gen
  GIT_REPOSITORY https://github.com/Chlumsky/msdf-atlas-gen.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES_RECURSE TRUE
  SOURCE_DIR     "${CMAKE_SOURCE_DIR}/dependencies/msdf-atlas-gen"
)
FetchContent_MakeAvailable(msdf-atlas-gen)

# ---------------------------------------------------------------------------
# xfont_compiler's own sources
# ---------------------------------------------------------------------------
DefineInterfaceComponent(xfont_compiler_ "dependencies/xcore"
  "source/xfont_rsc_descriptor.h"
  "source/xfont_perfect_hash.h"
  "source/xfont_rsc_runtime.h"
  "source/bridges/xserializer/xfont_to_xserializer.h"
  "source/Compiler/xfont_compiler.h"
  "source/Compiler/xfont_compiler.cpp"
)

target_link_libraries(${TARGET_PROJECT} PRIVATE msdf-atlas-gen::msdf-atlas-gen)
target_include_directories(${TARGET_PROJECT} PRIVATE "${CMAKE_SOURCE_DIR}/dependencies/stb")

# BITMAP output type (xfont_rsc_descriptor.h) rasterizes directly via FreeType (FT_Load_Glyph/
# FT_Render_Glyph), bypassing msdfgen's own shape/distance-field pipeline entirely - msdf-atlas-gen
# links FreeType PRIVATE-ly (its own headers don't propagate here), so both the include path and a
# direct link are needed on this target too. FreeType itself is already fetched above (msdf-atlas-
# gen's own dependency), so this doesn't add a new external dependency, just exposes it here too.
target_include_directories(${TARGET_PROJECT} PRIVATE "${CMAKE_SOURCE_DIR}/dependencies/freetype/include")
target_link_libraries(${TARGET_PROJECT} PRIVATE freetype)

# Headless CLI compiler (no ImGui) - skip xresource_pipeline.h's own ImGui-UI property stub
# include. Applied target-wide (not just our own sources) since xresource_pipeline_v2's own
# xresource_pipeline.cpp - built as part of this same target via ProcessComponents() - includes
# xresource_pipeline.h directly and needs the same define.
target_compile_definitions(${TARGET_PROJECT} PRIVATE XRESOURCE_PIPELINE_NO_COMPILER)
