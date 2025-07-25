cmake_minimum_required(VERSION 3.10)

file(GLOB_RECURSE CMP_SRCS
    # Lossy Compression
    "apc/*.h"
    "apc/*.cpp"
    "atc/*.h"
    "atc/*.cpp"
    "ati/*.h"
    "ati/*.cpp"
    "ati/*.c"
    "basis/*.h"
    "basis/*.cpp"
    "bc6h/*.h"
    "bc6h/*.cpp"
    "bc7/*.h"
    "bc7/*.cpp"
    "block/*.h"
    "block/*.cpp"
    "buffer/*.h"
    "buffer/*.cpp"
    "dxt/*.h"
    "dxt/*.cpp"
    "dxtc/*.h"
    "dxtc/*.cpp"
    "dxtc/*.c"
    "etc/*.h"
    "etc/*.cpp"
    "etc/etcpack/*.h"
    "etc/etcpack/*.cpp"
    "etc/etcpack/*.cxx"
    "gt/*.h"
    "gt/*.cpp"

    # Common 
    "common/*.h"
    "common/*.cpp"
    ${PROJECT_SOURCE_DIR}/cmp_framework/common/*.h
    ${PROJECT_SOURCE_DIR}/cmp_framework/common/*.cpp
    ${PROJECT_SOURCE_DIR}/cmp_framework/common/half/*.h
    ${PROJECT_SOURCE_DIR}/cmp_framework/common/half/*.cpp
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/atiformats.cpp
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/atiformats.h
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/format_conversion.h
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/format_conversion.cpp
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/codec_common.h
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/codec_common.cpp
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/texture_utils.h
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common/texture_utils.cpp
)

if (OPTION_BUILD_ASTC)
    file(GLOB_RECURSE CMP_ASTC_SRCS
        "astc/*.h"
        "astc/*.cpp"
        "astc/arm/*.h"
        "astc/arm/*.cpp"
        )
    list(APPEND CMP_SRCS ${CMP_ASTC_SRCS})
endif()

if (OPTION_BUILD_BROTLIG)
    file(GLOB_RECURSE CMP_BRLG_SRCS
        "brotlig/*.h"
        "brotlig/*.cpp"
    )
    list(APPEND CMP_SRCS ${CMP_BRLG_SRCS})
endif()

add_library(CMP_Compressonator 
    SHARED
    version.h
    common.h
    compress.cpp
    compressonator.h
    compressonator.cpp
    ${CMP_SRCS}
)

target_include_directories(CMP_Compressonator
    PUBLIC
    ./
    # Lossy Compression
    apc
    atc
    ati
    basis
    bc6h
    bc7
    block
    buffer
    dxt
    dxtc
    etc
    etc/etcpack/
    gt

    # Common
    common
    ${PROJECT_SOURCE_DIR}/cmp_framework/common
    ${PROJECT_SOURCE_DIR}/cmp_framework/common/half
    ${PROJECT_SOURCE_DIR}/applications/_plugins/common
    ${PROJECT_SOURCE_DIR}/applications/_libs/cmp_math/
)

set_target_properties(CMP_Compressonator PROPERTIES FOLDER ${PROJECT_FOLDER_SDK_LIBS})

if (OPTION_BUILD_ASTC)
    target_include_directories(CMP_Compressonator
        PUBLIC
        astc
        astc/arm/
    )
endif()

if (OPTION_BUILD_BROTLIG)
    target_include_directories(CMP_Compressonator PUBLIC ${PROJECT_SOURCE_DIR}/cmp_compressonatorlib/brotlig)

    # should there be an error if OPTION_CMP_DIRECTX isn't ON?
    target_link_libraries(CMP_Compressonator PUBLIC
        ExtBrotlig
        dxcompiler.lib
        dxgi.lib
        d3d12.lib
        dxguid.lib
    )
endif()

# TODO: More libraries should be linked like this when building so that we don't have to constantly repeat source file includes
# Not just for CMP_Compressonator, do the same for the Framework and all other libraries
target_link_libraries(CMP_Compressonator PUBLIC CMP_Core CMP_Framework)

# Set RPATH so the library can find its dependencies at runtime
# // https://duerrenberger.dev/blog/2021/08/04/understanding-rpath-with-cmake/
if(UNIX)
    set_target_properties(CMP_Compressonator PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()
