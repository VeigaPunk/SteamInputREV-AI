cmake_minimum_required(VERSION 3.16)
project(steaminput-harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(steaminput-harness src/steaminput_harness.cpp)

if(UNIX AND NOT APPLE)
    add_executable(ds5-input tools/ds5-input/ds5-input.c)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED opus)
    pkg_check_modules(PULSE REQUIRED libpulse libpulse-simple)
    add_executable(ds5-mic tools/ds5-mic/ds5-mic.c)
    target_include_directories(ds5-mic PRIVATE ${OPUS_INCLUDE_DIRS} ${PULSE_INCLUDE_DIRS})
    target_link_libraries(ds5-mic PRIVATE ${OPUS_LIBRARIES} ${PULSE_LIBRARIES})
    target_compile_options(ds5-mic PRIVATE -Wall -Wextra)
    target_compile_options(ds5-input PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_link_libraries(steaminput-harness PRIVATE advapi32)
elseif(UNIX)
    target_link_libraries(steaminput-harness PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    target_compile_options(steaminput-harness PRIVATE /W4)
else()
    target_compile_options(steaminput-harness PRIVATE -Wall -Wextra)
endif()

install(TARGETS steaminput-harness RUNTIME DESTINATION bin)
