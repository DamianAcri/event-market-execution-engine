cmake_minimum_required(VERSION 3.25)

project(
    event_market_execution_engine
    VERSION 0.1.0
    DESCRIPTION "Correct, replayable event-market execution infrastructure"
    LANGUAGES CXX
)

option(EME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

add_library(eme_core
    src/book/order_book.cpp
    src/gateway/kalshi/orderbook_normalizer.cpp
)

target_include_directories(eme_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(eme_core PUBLIC cxx_std_20)

function(eme_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(EME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
        if(EME_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

eme_enable_warnings(eme_core)

add_executable(event-engine src/main.cpp)
target_link_libraries(event-engine PRIVATE eme_core)
eme_enable_warnings(event-engine)

include(CTest)

if(BUILD_TESTING)
    add_executable(eme_tests tests/core_tests.cpp)
    target_link_libraries(eme_tests PRIVATE eme_core)
    eme_enable_warnings(eme_tests)

    add_test(NAME eme_core_tests COMMAND eme_tests)
endif()
