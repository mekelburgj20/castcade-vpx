# Standalone tests of CastcadeWire and of the watcher's timeline, CastcadeTimeline.h (not part of the VPX build).
cmake_minimum_required(VERSION 3.20)
project(castcade_wire_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINIZ ${CMAKE_CURRENT_SOURCE_DIR}/../../../third-party/include/miniz/miniz.c)
set_source_files_properties(${MINIZ} PROPERTIES LANGUAGE CXX)

add_executable(wire_test
   ../CastcadeUnwind.cpp
   ../CastcadeWire.cpp
   wire_test.cpp
   ${MINIZ})

target_include_directories(wire_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../third-party/include)
target_compile_definitions(wire_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(wire_test PRIVATE /GR- /EHsc /utf-8 /W3)
target_link_libraries(wire_test PRIVATE ws2_32 oleaut32)

# The timeline alone on a simulated clock: no VPX, no socket, no file (I11.4.17)
add_executable(timeline_test timeline_test.cpp ../CastcadeTimeline.h)
target_compile_definitions(timeline_test PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(timeline_test PRIVATE /GR- /EHsc /utf-8 /W3)
