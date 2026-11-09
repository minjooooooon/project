cmake_minimum_required(VERSION 3.24)
project(simple_orm LANGUAGES CXX)

add_library(simple_orm INTERFACE)
target_include_directories(simple_orm INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(simple_orm INTERFACE cxx_std_20)

# Our own tests and benchmark are only built when this project is the top-level
# project (never when it is pulled in through FetchContent).
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    foreach(name test_orm_example test_orm_edge)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp)
            add_executable(${name} tests/${name}.cpp)
            target_link_libraries(${name} PRIVATE simple_orm)
            file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run_${name})
            add_test(NAME ${name} COMMAND ${name}
                     WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run_${name})
        endif()
    endforeach()

    # Benchmark, not a test: run `bench_orm all` in an empty directory.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_orm.cpp)
        add_executable(bench_orm tests/bench_orm.cpp)
        target_link_libraries(bench_orm PRIVATE simple_orm)
    endif()
endif()
