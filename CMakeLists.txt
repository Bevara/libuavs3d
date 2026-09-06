cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBUAVS3D_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_avs3.c
        ${CMAKE_CURRENT_SOURCE_DIR}/avs3_stubs.c
)

SET(LIBUAVS3D_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libuavs3d.a
)

SET(LIBUAVS3D_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libuavs3d
        "${LIBUAVS3D_SRC}"
        "${LIBUAVS3D_LIBS}"
        []
        ""
        "${LIBUAVS3D_INC}"
        ""
        "1")
