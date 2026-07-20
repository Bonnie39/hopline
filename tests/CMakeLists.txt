find_package(Catch2 3 REQUIRED)

add_executable(hopline_tests
    test_time.cpp
    test_track.cpp
    test_sequence.cpp
    test_commands.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Track.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Sequence.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Project.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Command.cpp
    ${CMAKE_SOURCE_DIR}/src/model/Commands.cpp
)

target_include_directories(hopline_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hopline_tests PRIVATE Catch2::Catch2WithMain)

if(MSVC)
    target_compile_options(hopline_tests PRIVATE /W4 /permissive-)
else()
    target_compile_options(hopline_tests PRIVATE -Wall -Wextra)
endif()

include(Catch)
catch_discover_tests(hopline_tests)
