cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(Sherman)

# try to use ccache if available
find_program(CCACHE_FOUND ccache)
if(CCACHE_FOUND)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache) # Less useful to do it for linking, see edit2
    message(STATUS "Found ccache")
endif()

# add_compile_options(--gcc-toolchain=${PROJECT_SOURCE_DIR}/toolchain)
# set(CMAKE_CXX_COMPILER "/usr/bin/clang++-10 --gcc-toolchain=${PROJECT_SOURCE_DIR}/toolchain")
# set(CMAKE_CXX_FLAGS "--gcc-toolchain=${PROJECT_SOURCE_DIR}/toolchain")
# set(CMAKE_CC_FLAGS "--gcc-toolchain=${PROJECT_SOURCE_DIR}/toolchain")

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(ExternalProject)

option(BUILD_TEST "Whether or not build the tests" ON)
option(BUILD_BENCH "Whether or not build the benchmark" ON)
option(ASAN "Turn on sanitization" OFF)
option(PF "Profile" OFF)
option(VALGRIND "Support debugging with valgrind" OFF)

# glog and gflags
# find_package(glog 0.0.0 REQUIRED)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Build examples" FORCE)
add_subdirectory(thirdparty/glog)
find_package(gflags REQUIRED)
# target_link_libraries(sherman PUBLIC ${GLOG_LIBRARIES} ${GFLAGS_LIBRARIES})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-g3 -Wall -Wextra -Werror -Wunused -Wunused-private-field -mno-avx -fPIC)

if (VALGRIND)
    add_compile_options(-mno-avx)
else()
    add_compile_options(-march=native)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

set(LINKS_FLAGS "-rdynamic")

if(ASAN)
    set(ASAN_OPTIONS "fast_unwind_on_fatal=1")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fsanitize-recover=address -fPIE")
    add_definitions(-DBOOST_USE_ASAN)
    if (VALGRIND)
        message(FATAL_ERROR "ASAN and VALGRIND are not compatible")
    endif()
endif()

if(PF)
    add_compile_options(-pg)
endif()


# This library is used by all executables
set(COMMON_SRC ${PROJECT_SOURCE_DIR}/src)
file(GLOB_RECURSE COMMON_FILE ${COMMON_SRC}/*.cpp)
add_library(sherman STATIC ${COMMON_FILE})
set(INCLUDE_BASE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(sherman PUBLIC ${INCLUDE_BASE})

set(CPP_COMPILE_WARNING_LESS_FLAGS -Wall -Wextra -Wctor-dtor-privacy -Wdisabled-optimization 
-Wformat=2 -Winit-self -Wmissing-declarations -Wmissing-include-dirs -Woverloaded-virtual 
-Wredundant-decls -Wsign-promo -Wstrict-overflow=5 -Wswitch-default -Werror -Wunused
-Wno-format-nonliteral -Weffc++ -Wuninitialized -Wno-sign-compare)
set(CPP_COMPILE_WARNING_MORE_FLAGS ${CPP_COMPILE_WARNING_LESS_FLAGS} -Wshadow)
target_compile_options(sherman PRIVATE ${CPP_COMPILE_WARNING_MORE_FLAGS})

# standards
target_link_libraries(sherman PUBLIC mlx5 numa pthread ibverbs memcached stdc++fs atomic stdc++)
target_link_libraries(sherman PRIVATE unwind)

# glog
target_link_libraries(sherman PUBLIC glog::glog ${GFLAGS_LIBRARIES})
target_include_directories(sherman PUBLIC ${GFLAGS_INCLUDE_DIRS})

# CityHash
# find_library(CITYHASH_LIB NAMES libcityhash.a REQUIRED)
# target_link_libraries(sherman PUBLIC ${CITYHASH_LIB})


# boost
# disable boost warning
add_definitions(-DBOOST_COROUTINES_NO_DEPRECATION_WARNING)
set(Boost_USE_STATIC_LIBS ON)
find_package(
    Boost
    1.59
    REQUIRED
    coroutine
    system
    thread
)
target_link_libraries(sherman PUBLIC ${Boost_LIBRARIES})
target_include_directories(sherman PUBLIC ${Boost_INCLUDE_DIRS})

# for fmt
find_package(fmt REQUIRED)
target_link_libraries(sherman PUBLIC fmt::fmt)
target_compile_definitions(sherman PUBLIC USE_FMT_LIB)
# link_libraries(fmt::fmt-header-only)

# for Magick++
# add_definitions(-DMAGICKCORE_QUANTUM_DEPTH=16)
# add_definitions(-DMAGICKCORE_HDRI_ENABLE=0)
# find_package(ImageMagick COMPONENTS Magick++)
# # use SYSTEM GLAD to disable warning from libraries
# include_directories(SYSTEM GLAD ${ImageMagick_INCLUDE_DIRS})
# target_link_libraries(sherman PUBLIC ${ImageMagick_LIBRARIES})


# for cityhash
set(cityhash_INSTALL_DIR ${CMAKE_SOURCE_DIR}/thirdparty/cityhash)
ExternalProject_Add(cityhash
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/cityhash
    CONFIGURE_COMMAND pwd && ./configure --enable-static
    BUILD_COMMAND make all CXXFLAGS="-g -O3" -j
    BUILD_IN_SOURCE 1
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${CMAKE_SOURCE_DIR}/thirdparty/cityhash/src/.libs/libcityhash.a)

add_dependencies(sherman cityhash)

ExternalProject_Get_property(cityhash SOURCE_DIR)
target_include_directories(sherman PUBLIC ${SOURCE_DIR}/src)
target_link_libraries(sherman PUBLIC ${SOURCE_DIR}/src/.libs/libcityhash.a)

# for fmt
# NOTE: can not use fmt as an external project:
# when our project includes fmt's headers, throw compiling error
# of unsued variable _; 
# However, if we build and install fmt into the system, it works.
# set(fmt_INSTALL_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fmt)
# ExternalProject_Add(fmt
#     SOURCE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fmt
#     CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
#     BINARY_DIR ${CMAKE_SOURCE_DIR}/thirdparty/fmt/build
#     INSTALL_COMMAND ""
#     BUILD_BYPRODUCTS ${CMAKE_SOURCE_DIR}/thirdparty/fmt/build/libfmt.a
# )
# add_dependencies(sherman fmt)

# ExternalProject_Get_property(fmt SOURCE_DIR)
# target_include_directories(sherman PUBLIC ${SOURCE_DIR}/include)
# target_link_libraries(sherman PUBLIC ${SOURCE_DIR}/build/libfmt.a)

# for jemalloc
set(jemalloc_INSTALL_DIR ${CMAKE_SOURCE_DIR}/thirdparty/jemalloc)
ExternalProject_Add(jemalloc
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/jemalloc
    CONFIGURE_COMMAND autoconf && ./configure --with-jemalloc-prefix=je --prefix=${jemalloc_INSTALL_DIR}
    BUILD_COMMAND make -j
    BUILD_IN_SOURCE 1
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${CMAKE_SOURCE_DIR}/thirdparty/jemalloc/lib/libjemalloc.so)
add_dependencies(sherman jemalloc)

ExternalProject_Get_property(jemalloc SOURCE_DIR)
target_include_directories(sherman PUBLIC ${SOURCE_DIR}/include)
target_link_libraries(sherman PUBLIC ${SOURCE_DIR}/lib/libjemalloc.so)

# for dataframe
ExternalProject_Add(dataframe
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/thirdparty/DataFrame
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
    BINARY_DIR ${CMAKE_SOURCE_DIR}/thirdparty/DataFrame/build
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${CMAKE_SOURCE_DIR}/thirdparty/DataFrame/build/libDataFrame.a
    )
add_dependencies(sherman dataframe)

# for range-v3
# which is header-only
target_include_directories(sherman PUBLIC ${CMAKE_SOURCE_DIR}/thirdparty/range-v3/include)


ExternalProject_Get_property(dataframe SOURCE_DIR)
target_include_directories(sherman PUBLIC ${SOURCE_DIR}/include)
target_link_libraries(sherman PUBLIC ${SOURCE_DIR}/build/libDataFrame.a)


# bench
if(${BUILD_BENCH})
    file(GLOB BENCHMARK_SRC ${PROJECT_SOURCE_DIR}/bench/*.cpp)

    foreach(BENCHMARK ${BENCHMARK_SRC})
        get_filename_component(BENCHMARK_NAME ${BENCHMARK} NAME_WE)
        add_executable(${BENCHMARK_NAME} ${BENCHMARK})
        target_link_libraries(${BENCHMARK_NAME} PRIVATE ${LINKS_FLAGS} sherman)
        target_compile_options(${BENCHMARK_NAME} PRIVATE ${CPP_COMPILE_WARNING_LESS_FLAGS})
    endforeach()
endif()

# test
if(${BUILD_TEST})
    file(GLOB TEST_SRC ${PROJECT_SOURCE_DIR}/test/*.cpp)

    foreach(TEST ${TEST_SRC})
        get_filename_component(TEST_NAME ${TEST} NAME_WE)
        add_executable(${TEST_NAME} ${TEST})
        target_link_libraries(${TEST_NAME} PRIVATE ${LINKS_FLAGS} sherman)
        target_compile_options(${TEST_NAME} PRIVATE ${CPP_COMPILE_WARNING_LESS_FLAGS})
    endforeach()
endif()
