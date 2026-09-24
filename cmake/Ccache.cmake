option(ULTRAVIOLENT_USE_CCACHE "Use ccache as the compiler launcher when available" ON)

# A missing ccache must never make the build fail; fall back to the plain compiler.
# An explicitly configured launcher always wins.
if(ULTRAVIOLENT_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(ULTRAVIOLENT_CCACHE_PROGRAM ccache)
    if(ULTRAVIOLENT_CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${ULTRAVIOLENT_CCACHE_PROGRAM}")
    endif()
endif()
