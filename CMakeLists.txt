cmake_minimum_required(VERSION 3.16)
project(steaminput-harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(steaminput-harness src/steaminput_harness.cpp)

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
