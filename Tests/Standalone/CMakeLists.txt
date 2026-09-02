cmake_minimum_required(VERSION 3.20)
project(KrigingPortableGate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(kriging_portable STATIC
    ../../Source/KrigingCore/Private/Portable/KrigePortableCore.cpp
    ../../Source/KrigingCore/Private/Portable/KrigePortableAnalysis.cpp
    ../../Source/KrigingCore/Private/Portable/KrigePortableMarchingCubes.cpp)
target_include_directories(kriging_portable PUBLIC
    ../../Source/KrigingCore/Public
    ../../Source/KrigingCore/Private/Portable)
if(MSVC)
    target_compile_options(kriging_portable PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_portable PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(kriging_core_tests test_core.cpp)
target_link_libraries(kriging_core_tests PRIVATE kriging_portable)
if(MSVC)
    target_compile_options(kriging_core_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_core_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME kriging_core_properties COMMAND kriging_core_tests)

add_executable(kriging_allocation_probe allocation_probe.cpp)
target_link_libraries(kriging_allocation_probe PRIVATE kriging_portable)
if(MSVC)
    target_compile_options(kriging_allocation_probe PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_allocation_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME kriging_global_query_no_allocations COMMAND kriging_allocation_probe)

add_executable(kriging_mc_sphere_test test_marching_cubes.cpp)
target_link_libraries(kriging_mc_sphere_test PRIVATE kriging_portable)
if(MSVC)
    target_compile_options(kriging_mc_sphere_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_mc_sphere_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME mc_sphere_test COMMAND kriging_mc_sphere_test)

add_executable(kriging_mc_table_test test_mc_tables.cpp)
target_link_libraries(kriging_mc_table_test PRIVATE kriging_portable)
if(MSVC)
    target_compile_options(kriging_mc_table_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_mc_table_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME mc_table_test COMMAND kriging_mc_table_test)

add_executable(kriging_analysis_smoke_test test_analysis_smoke.cpp)
target_link_libraries(kriging_analysis_smoke_test PRIVATE kriging_portable)
if(MSVC)
    target_compile_options(kriging_analysis_smoke_test PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_analysis_smoke_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME analysis_smoke_test COMMAND kriging_analysis_smoke_test)


# Compile the actual Unreal-facing translation units against narrow syntax
# stubs. This is not an Unreal compatibility claim; it catches ordinary C++
# errors (wrong field names, arity, missing local headers) without an engine.
add_library(kriging_unreal_source_syntax OBJECT
    ../../Source/KrigingCore/Private/KrigingCoreModule.cpp
    ../../Source/KrigingCore/Private/Tests/KrigeCoreAutomationTests.cpp)
target_include_directories(kriging_unreal_source_syntax PRIVATE
    UnrealStubs
    ../../Source/KrigingCore/Public
    ../../Source/KrigingCore/Private/Portable)
target_compile_definitions(kriging_unreal_source_syntax PRIVATE
    WITH_DEV_AUTOMATION_TESTS=1)
if(MSVC)
    target_compile_options(kriging_unreal_source_syntax PRIVATE /W4 /permissive-)
else()
    target_compile_options(kriging_unreal_source_syntax PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
