option(ULTRAVIOLENT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ULTRAVIOLENT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

function(ultraviolent_enable_sanitizers target)
    set(sanitizers "")

    if(ULTRAVIOLENT_ENABLE_ASAN)
        list(APPEND sanitizers "address")
    endif()

    if(ULTRAVIOLENT_ENABLE_UBSAN)
        list(APPEND sanitizers "undefined")
    endif()

    if(sanitizers)
        list(JOIN sanitizers "," sanitizer_list)
        target_compile_options(${target} PRIVATE
            -fsanitize=${sanitizer_list}
            -fno-omit-frame-pointer
        )
        target_link_options(${target} PRIVATE
            -fsanitize=${sanitizer_list}
            -fno-omit-frame-pointer
        )
    endif()
endfunction()