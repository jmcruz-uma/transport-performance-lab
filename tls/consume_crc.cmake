# "*_crc" variants of the benchmark clients (see consume_crc.hpp).
#
# consume_crc_variants(<client>...) gives every listed client that exists in this
# directory the include path of consume_crc.hpp and adds <client>_crc: the same
# sources, link libraries, include directories, compile definitions, options and
# features, plus CONSUME_CRC. Nothing else differs between a client and its variant.

function(consume_crc_variants)
    foreach(client IN LISTS ARGN)
        if(NOT TARGET ${client})
            continue()
        endif()
        target_include_directories(${client} PRIVATE ${TLS_COMMON_DIR})

        get_target_property(sources ${client} SOURCES)
        add_executable(${client}_crc ${sources})
        foreach(property IN ITEMS LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS
                                  COMPILE_OPTIONS COMPILE_FEATURES)
            get_target_property(value ${client} ${property})
            if(value)
                set_property(TARGET ${client}_crc PROPERTY ${property} ${value})
            endif()
        endforeach()
        target_compile_definitions(${client}_crc PRIVATE CONSUME_CRC)
    endforeach()
endfunction()
