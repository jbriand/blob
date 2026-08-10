include_guard(GLOBAL)

# Internal: collect the transitive link closure of <target> into <out_var>.
function(_blob_collect_links target out_var)
    set(_seen "")
    set(_queue "${target}")
    while(_queue)
        list(POP_FRONT _queue _cur)
        if(_cur IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_cur}")
        if(NOT TARGET ${_cur})
            continue()
        endif()
        # Follow ALIAS targets to their real name, otherwise a forbidden
        # dependency hides behind its alias (blob::enet -> enet).
        get_target_property(_aliased ${_cur} ALIASED_TARGET)
        if(_aliased)
            list(APPEND _queue "${_aliased}")
        endif()

        get_target_property(_type ${_cur} TYPE)
        set(_deps "")
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_l ${_cur} LINK_LIBRARIES)
            if(_l)
                list(APPEND _deps ${_l})
            endif()
        endif()
        get_target_property(_i ${_cur} INTERFACE_LINK_LIBRARIES)
        if(_i)
            list(APPEND _deps ${_i})
        endif()
        foreach(_d IN LISTS _deps)
            # Strip generator expressions; we only want plain names here.
            string(REGEX REPLACE "\\$<[^>]*>" "" _d "${_d}")
            string(STRIP "${_d}" _d)
            if(_d)
                list(APPEND _queue "${_d}")
            endif()
        endforeach()
    endwhile()
    set(${out_var} "${_seen}" PARENT_SCOPE)
endfunction()

# blob_assert_no_transitive_deps(<target> FORBIDDEN <name>...)
#
# Enforces the project's hard rule that `core` stays free of rendering and I/O
# dependencies. Fails at configure time, before anything is compiled.
function(blob_assert_no_transitive_deps target)
    cmake_parse_arguments(ARG "" "" "FORBIDDEN" ${ARGN})
    if(NOT TARGET ${target})
        message(FATAL_ERROR "blob_assert_no_transitive_deps: no such target '${target}'")
    endif()

    _blob_collect_links(${target} _closure)

    set(_violations "")
    foreach(_bad IN LISTS ARG_FORBIDDEN)
        if(_bad IN_LIST _closure)
            list(APPEND _violations "${_bad}")
        endif()
    endforeach()

    if(_violations)
        message(FATAL_ERROR
            "Target '${target}' must stay free of rendering/I-O dependencies, "
            "but its link closure contains: ${_violations}\n"
            "Move whatever needs it into server/ or client/.")
    endif()
endfunction()
