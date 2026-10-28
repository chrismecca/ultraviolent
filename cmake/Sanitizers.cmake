option(ULTRAVIOLENT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ULTRAVIOLENT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ULTRAVIOLENT_ENABLE_TSAN "Enable ThreadSanitizer (host-parallel CPUs)" OFF)

function(ultraviolent_enable_sanitizers target)
    set(sanitizers "")

    if(ULTRAVIOLENT_ENABLE_ASAN)
        list(APPEND sanitizers "address")
    endif()

    if(ULTRAVIOLENT_ENABLE_TSAN)
        list(APPEND sanitizers "thread")
    endif()

    set(no_recover "")
    if(ULTRAVIOLENT_ENABLE_UBSAN)
        list(APPEND sanitizers "undefined")
        # A report aborts the process, so undefined behavior fails the test that reached it
        # instead of printing a line nobody reads.
        set(no_recover -fno-sanitize-recover=undefined)
    endif()

    if(sanitizers)
        list(JOIN sanitizers "," sanitizer_list)
        target_compile_options(${target} PRIVATE
            -fsanitize=${sanitizer_list}
            ${no_recover}
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=${sanitizer_list}
            ${no_recover}
            -fno-omit-frame-pointer
        )
    endif()
endfunction()