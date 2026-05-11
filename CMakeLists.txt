cmake_minimum_required(VERSION 3.10)
project(DatabaseCore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VTUNE_DIR "C:/Program Files (x86)/Intel/oneAPI/vtune/latest")
if(EXISTS "${VTUNE_DIR}/sdk/include")
    message(STATUS "Found VTune ITT API at ${VTUNE_DIR}")
    include_directories("${VTUNE_DIR}/sdk/include")
    set(VTUNE_ITT_SRC "${VTUNE_DIR}/sdk/lib64/libittnotify.lib")      
    add_compile_definitions(USE_VTUNE_ITT)
else()
    message(WARNING "VTune ITT SDK not found")
    set(VTUNE_ITT_SRC "")
endif()


# ????????(??? CTest)
enable_testing()

# ???????????????????????????????#include "Database/...h" ???#include "BufferPoolManager.h"
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------
# 1. ?????????????? (Core Library)
# ???: ?????? main ?????????????????
# ---------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

# ??? Server/Main ???????????????????????main ???
list(FILTER CORE_SOURCES EXCLUDE REGEX "server_main.cpp\$")
list(FILTER CORE_SOURCES EXCLUDE REGEX "test_[A-Za-z0-9_]+\\.cpp\$")

# ????????
add_library(database_core STATIC ${CORE_SOURCES})

# ---------------------------------------------------------
# JIT / LLVM Configuration
# ---------------------------------------------------------
option(ENABLE_JIT "Enable LLVM JIT Compilation" ON)
if (ENABLE_JIT)
    # ????????LLVM ????????? REQUIRED ????????????????????????
    find_package(LLVM QUIET CONFIG)
    if(LLVM_FOUND)
        message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
        include_directories(${LLVM_INCLUDE_DIRS})
        add_definitions(${LLVM_DEFINITIONS})
        target_compile_definitions(database_core PUBLIC ENABLE_JIT=1)
        
        # ???????LLVM ???
        llvm_map_components_to_libnames(llvm_libs core executionengine mcjit native orcjit)
        target_link_libraries(database_core PUBLIC ${llvm_libs})
    else()
        message(WARNING "LLVM not found in environment. JIT Engine will be disabled (fallback to normal execution).")
        remove_definitions(-DENABLE_JIT)
    endif()
endif()

# ---------------------------------------------------------
# 2. ??????????????
# ---------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/server_main.cpp")
    add_executable(database_server server_main.cpp)
    target_link_libraries(database_server PRIVATE database_core)
    if(WIN32)
        target_link_libraries(database_server PRIVATE ws2_32 mswsock)
    endif()
endif()

# ---------------------------------------------------------
# 3. ?????????????(?????????)
# ---------------------------------------------------------
file(GLOB_RECURSE TEST_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*test_[A-Za-z0-9_]*.cpp"
)

foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE database_core ${VTUNE_ITT_SRC})
    if(WIN32)
        target_link_libraries(${test_name} PRIVATE ws2_32 mswsock)
    endif()
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
set(CMAKE_CXX_FLAGS "-g -O3 -fno-omit-frame-pointer -mavx2 -march=native")
