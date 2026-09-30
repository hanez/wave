# Keep the pinned upstream checkout intact. Only Wave's build copy is adapted.
# The MC68000 registers, cycle counters and longjmp targets must belong to the
# host rendering thread; sharing them can jump into another AU instance's stack.
function(wave_prepare_musashi source destination)
    file(COPY "${source}/" DESTINATION "${destination}"
         PATTERN ".git" EXCLUDE
         PATTERN "m68kcpu.c" EXCLUDE
         PATTERN "m68kcpu.h" EXCLUDE)
    set(execution_state
        m68ki_cpu m68ki_initial_cycles m68ki_remaining_cycles m68ki_tracing
        m68ki_address_space m68ki_aerr_trap m68ki_aerr_address
        m68ki_aerr_write_mode m68ki_aerr_fc m68ki_bus_error_jmp_buf
        default_int_ack_callback_data default_bkpt_ack_callback_data
        default_pc_changed_callback_data default_set_fc_callback_data)
    foreach(filename m68kcpu.c m68kcpu.h)
        file(READ "${source}/${filename}" contents)
        foreach(symbol IN LISTS execution_state)
            # Capture the whole prefix, which always participates in the match.
            # CMake < 4.1 rejects a backreference to an unmatched optional group.
            set(pattern "(^|\n)([ \t]*(extern |static )?)((unsigned int|int|sint|uint|m68ki_cpu_core|sigjmp_buf|jmp_buf)[ \t]+${symbol}[ \t]*[;=])")
            if(filename STREQUAL "m68kcpu.c" AND NOT contents MATCHES "${pattern}")
                message(FATAL_ERROR "Musashi execution-state declaration changed: ${symbol}")
            endif()
            # Some symbols occur only in the implementation, not the header.
            if(contents MATCHES "${pattern}")
                string(REGEX REPLACE "${pattern}" "\\1\\2WAVE_M68K_THREAD_LOCAL \\4"
                       contents "${contents}")
            endif()
        endforeach()
        set(existing "")
        if(EXISTS "${destination}/${filename}")
            file(READ "${destination}/${filename}" existing)
        endif()
        if(NOT existing STREQUAL contents)
            file(WRITE "${destination}/${filename}" "${contents}")
        endif()
    endforeach()
endfunction()
