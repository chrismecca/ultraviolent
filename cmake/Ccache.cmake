option(ULTRAVIOLENT_USE_CCACHE "Use ccache as the compiler launcher when available" ON)

# A missing or broken ccache must never make the build fail (DEVELOPMENT "Ccache"): use it
# only when its cache directory exists or can be created. An explicitly configured launcher
# always wins.
if(ULTRAVIOLENT_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(ULTRAVIOLENT_CCACHE_PROGRAM ccache)
    if(ULTRAVIOLENT_CCACHE_PROGRAM)
        execute_process(
            COMMAND "${ULTRAVIOLENT_CCACHE_PROGRAM}" --get-config cache_dir
            OUTPUT_VARIABLE ultraviolent_ccache_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE ultraviolent_ccache_result
        )
        if(ultraviolent_ccache_result EQUAL 0 AND ultraviolent_ccache_dir)
            # Not file(MAKE_DIRECTORY), which is fatal on failure.
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${ultraviolent_ccache_dir}"
                RESULT_VARIABLE ultraviolent_ccache_mkdir
                ERROR_QUIET
            )
        endif()
        if(ultraviolent_ccache_result EQUAL 0 AND IS_DIRECTORY "${ultraviolent_ccache_dir}")
            set(CMAKE_CXX_COMPILER_LAUNCHER "${ULTRAVIOLENT_CCACHE_PROGRAM}")
        else()
            message(STATUS "ccache cache directory '${ultraviolent_ccache_dir}' is unusable; "
                           "building without ccache")
        endif()
    endif()
endif()
