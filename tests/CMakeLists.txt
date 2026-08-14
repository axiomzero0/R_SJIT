cmake_minimum_required(VERSION 3.18)

# Unit test harness uses a tiny custom framework to avoid external deps.
# Each test is a self-contained C++ file with `int main()`.

file(GLOB RJIT_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(test_source ${RJIT_TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE rjit)
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/tests/include
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
