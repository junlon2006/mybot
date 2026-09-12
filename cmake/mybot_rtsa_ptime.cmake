# SPDX-License-Identifier: Apache-2.0

function(mybot_check_rtsa_ptime sdk_dir expected_ptime_ms)
    if(NOT sdk_dir OR NOT expected_ptime_ms)
        message(FATAL_ERROR
            "An Agora RTSA SDK directory and MYBOT_AUDIO_PTIME_MS are required for ptime validation")
    endif()

    set(actual_ptime_ms "")
    foreach(config_file
            "${sdk_dir}/.config"
            "${sdk_dir}/include/global_config.cmake")
        if(EXISTS "${config_file}")
            file(STRINGS "${config_file}" ptime_lines
                 REGEX "CONFIG_MINIMAL_TIMER_INTERVAL_MS")
            foreach(ptime_line IN LISTS ptime_lines)
                if(ptime_line MATCHES
                   "CONFIG_MINIMAL_TIMER_INTERVAL_MS[^0-9]*([0-9]+)")
                    set(actual_ptime_ms "${CMAKE_MATCH_1}")
                    break()
                endif()
            endforeach()
        endif()
        if(actual_ptime_ms)
            break()
        endif()
    endforeach()

    if(NOT actual_ptime_ms)
        message(FATAL_ERROR
            "Agora RTSA SDK at ${sdk_dir} does not expose "
            "CONFIG_MINIMAL_TIMER_INTERVAL_MS metadata; select a packaged SDK with "
            "matching build metadata")
    endif()
    if(NOT "${actual_ptime_ms}" STREQUAL "${expected_ptime_ms}")
        message(FATAL_ERROR
            "MYBOT_AUDIO_PTIME_MS=${expected_ptime_ms} does not match the Agora RTSA "
            "package timer cadence CONFIG_MINIMAL_TIMER_INTERVAL_MS=${actual_ptime_ms}. "
            "Select a matching RTSA SDK package.")
    endif()
endfunction()
