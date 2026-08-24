cmake_minimum_required(VERSION 3.25)

project(
    event_market_execution_engine
    VERSION 0.1.0
    DESCRIPTION "Correct, replayable event-market execution infrastructure"
    LANGUAGES CXX
)

option(EME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_library(eme_core
    src/book/order_book.cpp
)

add_library(eme_kalshi_gateway
    src/gateway/kalshi/market_registry.cpp
    src/gateway/kalshi/orderbook_decoder.cpp
    src/gateway/kalshi/orderbook_normalizer.cpp
)

target_include_directories(eme_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(eme_core PUBLIC cxx_std_20)

target_include_directories(eme_kalshi_gateway
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(eme_kalshi_gateway PUBLIC cxx_std_20)
target_link_libraries(eme_kalshi_gateway
    PUBLIC eme_core
    PRIVATE nlohmann_json::nlohmann_json
)

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
eme_enable_warnings(eme_kalshi_gateway)

add_executable(event-engine src/main.cpp)
target_link_libraries(event-engine PRIVATE eme_core)
eme_enable_warnings(event-engine)

include(CTest)

if(BUILD_TESTING)
    function(eme_add_test target source library)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${library})
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
        eme_enable_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    eme_add_test(eme_fixed_point_tests tests/fixed_point_tests.cpp eme_core)
    eme_add_test(eme_order_book_tests tests/order_book_tests.cpp eme_core)
    eme_add_test(
        eme_kalshi_normalizer_tests
        tests/kalshi_normalizer_tests.cpp
        eme_kalshi_gateway
    )
    eme_add_test(
        eme_kalshi_decoder_tests
        tests/kalshi_decoder_tests.cpp
        eme_kalshi_gateway
    )
endif()
