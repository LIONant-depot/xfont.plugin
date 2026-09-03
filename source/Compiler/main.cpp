#include "xfont_compiler.h"

//---------------------------------------------------------------------------------

int main( int argc, const char* argv[] )
{
    //
    // This is just for debugging
    //
    if constexpr (false)
    {
        static const char* pDebugArgs[] =
        { "FontCompiler"
        , "-PROJECT"
        , "D:\\LIONant\\xGPU\\example.lionprj"
        , "-DEBUG"
        , "D1"
        , "-DESCRIPTOR"
        , "Descriptors\\Font\\00\\00\\0000000000000000.desc"
        , "-OUTPUT"
        , "D:\\LIONant\\xGPU\\example.lionprj\\Cache\\Resources\\Platforms"
        };

        argv = pDebugArgs;
        argc = static_cast<int>(sizeof(pDebugArgs) / sizeof(pDebugArgs[0]));
    }

    //
    // Create the compiler instance
    //
    auto FontCompilerPipeline = xfont_compiler::instance::Create();

    //
    // Parse parameters
    //
    if( auto Err = FontCompilerPipeline->Parse( argc, argv ); Err )
    {
        Err.ForEachInChain( [&](xerr Error)
        {
            auto Hint   = Err.getHint();
            auto String = std::format("Error: {}\n", Err.getMessage());
            printf("%s", String.c_str());
            if (Hint.empty() == false )
                printf("Hint: %s\n", Hint.data() );
        });
        return 1;
    }

    //
    // Start compilation
    //
    if( auto Err = FontCompilerPipeline->Compile(); Err )
    {
        Err.ForEachInChain([&](xerr Error)
        {
            auto Hint   = Err.getHint();
            auto String = std::format("Error: {}\n", Err.getMessage());
            printf("%s", String.c_str());
            if (Hint.empty() == false )
                printf("Hint: %s\n", Hint.data() );
        });
        return 1;
    }

    return 0;
}
