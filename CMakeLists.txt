cmake_minimum_required(VERSION 3.25)

project(
    event_market_execution_engine
    VERSION 0.2.0
    DESCRIPTION "Correct, replayable event-market execution infrastructure"
    LANGUAGES CXX
)

option(EME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(EME_BUILD_KALSHI_GATEWAY "Build the Kalshi JSON boundary" ON)
option(EME_BUILD_LIVE_CAPTURE "Build optional authenticated read-only WS collector" OFF)
option(EME_BUILD_CLI "Build the operator command-line interface" ON)
option(EME_BUILD_BENCHMARKS "Build deterministic offline performance workloads" OFF)

file(MAKE_DIRECTORY ${PROJECT_BINARY_DIR}/generated/eme)
configure_file(
    cmake/version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/eme/version.hpp
    @ONLY
)

add_library(eme_core
    src/core/execution_cost.cpp
    src/core/net_sizing.cpp
    src/book/order_book.cpp
    src/constraint/payoff.cpp
    src/journal/journal_codec.cpp
    src/journal/raw_journal.cpp
    src/market/market_state.cpp
    src/opportunity/candidate_tracker.cpp
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

if(EME_BUILD_KALSHI_GATEWAY)
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)

    add_library(eme_kalshi_gateway
        src/gateway/kalshi/market_registry.cpp
        src/gateway/kalshi/metadata_snapshot.cpp
        src/gateway/kalshi/orderbook_decoder.cpp
        src/gateway/kalshi/orderbook_normalizer.cpp
        src/gateway/kalshi/orderbook_processor.cpp
    )
    target_include_directories(eme_kalshi_gateway PUBLIC ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(eme_kalshi_gateway PUBLIC cxx_std_20)
    target_link_libraries(eme_kalshi_gateway
        PUBLIC eme_core
        PRIVATE nlohmann_json::nlohmann_json
    )
    eme_enable_warnings(eme_kalshi_gateway)

    add_library(eme_session
        src/session/async_capture.cpp
        src/session/readonly_feed.cpp
        src/session/capture_session.cpp
        src/session/session_files.cpp
        src/session/replay.cpp
        src/session/execution_study.cpp
        src/session/study_policy.cpp
        src/session/import_capture.cpp
    )
    target_include_directories(eme_session PUBLIC ${PROJECT_SOURCE_DIR}/include PRIVATE ${PROJECT_SOURCE_DIR}/src)
    find_package(Threads REQUIRED)
    target_include_directories(eme_session SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/picosha2)
    target_link_libraries(eme_session PUBLIC eme_kalshi_gateway PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
    eme_enable_warnings(eme_session)
endif()

if(EME_BUILD_CLI)
    add_executable(event-engine src/main.cpp src/cli/commands.cpp)
    target_include_directories(event-engine PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_BINARY_DIR}/generated
    )
    if(EME_BUILD_KALSHI_GATEWAY)
        target_sources(event-engine PRIVATE src/cli/session_commands.cpp)
        target_link_libraries(event-engine PRIVATE eme_session)
        target_compile_definitions(event-engine PRIVATE EME_CLI_KALSHI=1)
    else()
        target_link_libraries(event-engine PRIVATE eme_core)
    endif()
    eme_enable_warnings(event-engine)
endif()

include(CTest)

if(EME_BUILD_BENCHMARKS)
    add_executable(eme_sizing_benchmarks benchmarks/sizing_benchmarks.cpp)
    target_link_libraries(eme_sizing_benchmarks PRIVATE eme_core)
    eme_enable_warnings(eme_sizing_benchmarks)
    if(BUILD_TESTING)
        add_test(NAME eme_sizing_benchmark_smoke COMMAND eme_sizing_benchmarks --smoke)
    endif()
    add_executable(eme_candidate_benchmarks benchmarks/candidate_benchmarks.cpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/benchmarks/candidate_benchmarks.cpp)
    file(SHA256 ${PROJECT_SOURCE_DIR}/benchmarks/candidate_benchmarks.cpp EME_CANDIDATE_BENCH_SOURCE_SHA256)
    target_compile_definitions(eme_candidate_benchmarks PRIVATE
        EME_CANDIDATE_BENCH_SOURCE_SHA256="${EME_CANDIDATE_BENCH_SOURCE_SHA256}")
    target_link_libraries(eme_candidate_benchmarks PRIVATE eme_core)
    eme_enable_warnings(eme_candidate_benchmarks)
    if(BUILD_TESTING)
        add_test(NAME eme_candidate_benchmark_smoke COMMAND eme_candidate_benchmarks --smoke)
    endif()
    add_executable(eme_benchmarks benchmarks/engine_benchmarks.cpp)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/benchmarks/engine_benchmarks.cpp)
    file(SHA256 ${PROJECT_SOURCE_DIR}/benchmarks/engine_benchmarks.cpp EME_BENCH_SOURCE_SHA256)
    target_compile_definitions(eme_benchmarks PRIVATE
        EME_BENCH_SOURCE_SHA256="${EME_BENCH_SOURCE_SHA256}")
    target_include_directories(eme_benchmarks PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(EME_BUILD_KALSHI_GATEWAY)
        target_link_libraries(eme_benchmarks PRIVATE eme_kalshi_gateway)
        target_compile_definitions(eme_benchmarks PRIVATE EME_BENCH_KALSHI=1)
    else()
        target_link_libraries(eme_benchmarks PRIVATE eme_core)
    endif()
    eme_enable_warnings(eme_benchmarks)
    if(BUILD_TESTING)
        add_test(NAME eme_benchmark_smoke COMMAND eme_benchmarks --smoke)
    endif()
    if(EME_BUILD_KALSHI_GATEWAY)
        add_executable(eme_feed_benchmarks benchmarks/feed_benchmarks.cpp)
        target_link_libraries(eme_feed_benchmarks PRIVATE eme_session)
        eme_enable_warnings(eme_feed_benchmarks)
        if(BUILD_TESTING)
            add_test(NAME eme_feed_benchmark_smoke COMMAND eme_feed_benchmarks --smoke)
        endif()
        add_executable(eme_capture_benchmarks benchmarks/capture_benchmarks.cpp)
        target_link_libraries(eme_capture_benchmarks PRIVATE eme_session)
        eme_enable_warnings(eme_capture_benchmarks)
        if(BUILD_TESTING)
            add_test(NAME eme_capture_benchmark_smoke COMMAND eme_capture_benchmarks --smoke)
        endif()
        add_executable(eme_study_benchmarks benchmarks/study_benchmarks.cpp)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/benchmarks/study_benchmarks.cpp)
        file(SHA256 ${PROJECT_SOURCE_DIR}/benchmarks/study_benchmarks.cpp EME_STUDY_BENCH_SOURCE_SHA256)
        target_compile_definitions(eme_study_benchmarks PRIVATE
            EME_STUDY_BENCH_SOURCE_SHA256="${EME_STUDY_BENCH_SOURCE_SHA256}")
        target_link_libraries(eme_study_benchmarks PRIVATE eme_session)
        eme_enable_warnings(eme_study_benchmarks)
        add_executable(eme_metadata_benchmarks benchmarks/metadata_benchmarks.cpp)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/benchmarks/metadata_benchmarks.cpp)
        file(SHA256 ${PROJECT_SOURCE_DIR}/benchmarks/metadata_benchmarks.cpp EME_METADATA_BENCH_SOURCE_SHA256)
        target_compile_definitions(eme_metadata_benchmarks PRIVATE
            EME_METADATA_BENCH_SOURCE_SHA256="${EME_METADATA_BENCH_SOURCE_SHA256}")
        target_link_libraries(eme_metadata_benchmarks PRIVATE eme_kalshi_gateway)
        eme_enable_warnings(eme_metadata_benchmarks)
        if(BUILD_TESTING)
            add_test(NAME eme_metadata_benchmark_smoke COMMAND eme_metadata_benchmarks --smoke)
        endif()
        add_executable(eme_session_benchmarks benchmarks/session_benchmarks.cpp)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/benchmarks/session_benchmarks.cpp)
        file(SHA256 ${PROJECT_SOURCE_DIR}/benchmarks/session_benchmarks.cpp EME_SESSION_BENCH_SOURCE_SHA256)
        target_compile_definitions(eme_session_benchmarks PRIVATE
            EME_SESSION_BENCH_SOURCE_SHA256="${EME_SESSION_BENCH_SOURCE_SHA256}")
        target_link_libraries(eme_session_benchmarks PRIVATE eme_session)
        eme_enable_warnings(eme_session_benchmarks)
        if(BUILD_TESTING)
            add_test(NAME eme_session_benchmark_smoke COMMAND eme_session_benchmarks --smoke)
        endif()
    endif()
endif()

if(BUILD_TESTING)
    function(eme_add_test target source library)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE ${library})
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
        eme_enable_warnings(${target})
        add_test(NAME ${target} COMMAND ${target})
    endfunction()

    eme_add_test(eme_fixed_point_tests tests/fixed_point_tests.cpp eme_core)
    eme_add_test(eme_execution_cost_tests tests/execution_cost_tests.cpp eme_core)
    eme_add_test(eme_net_sizing_tests tests/net_sizing_tests.cpp eme_core)
    eme_add_test(eme_constraint_tests tests/constraint_tests.cpp eme_core)
    eme_add_test(eme_candidate_tests tests/candidate_tests.cpp eme_core)
    eme_add_test(eme_order_book_tests tests/order_book_tests.cpp eme_core)
    eme_add_test(eme_market_state_tests tests/market_state_tests.cpp eme_core)
    eme_add_test(eme_raw_journal_tests tests/raw_journal_tests.cpp eme_core)
    eme_add_test(eme_journal_codec_tests tests/journal_codec_tests.cpp eme_core)
    target_include_directories(eme_journal_codec_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    if(EME_BUILD_KALSHI_GATEWAY)
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
        eme_add_test(
            eme_kalshi_replay_tests
            tests/kalshi_replay_tests.cpp
            eme_kalshi_gateway
        )
        eme_add_test(eme_metadata_snapshot_tests tests/metadata_snapshot_tests.cpp eme_kalshi_gateway)
        target_link_libraries(eme_metadata_snapshot_tests PRIVATE nlohmann_json::nlohmann_json)
        eme_add_test(eme_session_tests tests/session_tests.cpp eme_session)
        eme_add_test(eme_readonly_feed_tests tests/readonly_feed_tests.cpp eme_session)
        eme_add_test(eme_async_capture_tests tests/async_capture_tests.cpp eme_session)
        set_tests_properties(eme_async_capture_tests PROPERTIES TIMEOUT 30)
        eme_add_test(eme_candidate_replay_tests tests/candidate_replay_tests.cpp eme_session)
        eme_add_test(eme_study_tests tests/study_tests.cpp eme_session)
        target_include_directories(eme_study_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(eme_study_tests PRIVATE nlohmann_json::nlohmann_json)
        eme_add_test(eme_residual_study_tests tests/residual_study_tests.cpp eme_session)
        target_include_directories(eme_residual_study_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(eme_residual_study_tests PRIVATE nlohmann_json::nlohmann_json)
        target_include_directories(eme_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(eme_session_tests PRIVATE nlohmann_json::nlohmann_json)
    endif()
    if(EME_BUILD_CLI)
        if(EME_BUILD_KALSHI_GATEWAY)
            add_test(NAME eme_cli_metadata_verify COMMAND event-engine metadata verify
                ${PROJECT_SOURCE_DIR}/examples/metadata.snapshot.json)
            set_tests_properties(eme_cli_metadata_verify PROPERTIES
                PASS_REGULAR_EXPRESSION "METADATA        VALID")
            add_test(NAME eme_cli_metadata_roundtrip COMMAND ${CMAKE_COMMAND}
                -DENGINE=$<TARGET_FILE:event-engine>
                -DFIXTURE=${PROJECT_SOURCE_DIR}/examples/metadata.snapshot.json
                -DTEST_DIR=${PROJECT_BINARY_DIR}/metadata-cli-test
                -P ${PROJECT_SOURCE_DIR}/tests/metadata_cli_tests.cmake)
            add_test(NAME eme_cli_sessions COMMAND ${CMAKE_COMMAND}
                -DENGINE=$<TARGET_FILE:event-engine>
                -DFIXTURE_WRITER=$<TARGET_FILE:eme_session_tests>
                -DTEST_DIR=${PROJECT_BINARY_DIR}/session-cli-test
                -P ${PROJECT_SOURCE_DIR}/tests/session_cli_tests.cmake)
            add_test(NAME eme_cli_study COMMAND ${CMAKE_COMMAND}
                -DENGINE=$<TARGET_FILE:event-engine>
                -DFIXTURE_WRITER=$<TARGET_FILE:eme_study_tests>
                -DTEST_DIR=${PROJECT_BINARY_DIR}/study-cli-test
                -P ${PROJECT_SOURCE_DIR}/tests/study_cli_tests.cmake)
        endif()
        add_test(NAME eme_cli_status COMMAND event-engine status)
        add_test(NAME eme_cli_version COMMAND event-engine --version)
        set_tests_properties(eme_cli_version PROPERTIES
            PASS_REGULAR_EXPRESSION "event-engine ${PROJECT_VERSION}"
        )
    endif()
endif()

if(EME_BUILD_LIVE_CAPTURE)
    if(NOT EME_BUILD_KALSHI_GATEWAY)
        message(FATAL_ERROR "Live capture requires the Kalshi gateway")
    endif()
    find_path(EME_BOOST_INCLUDE_DIR boost/beast.hpp REQUIRED)
    file(STRINGS "${EME_BOOST_INCLUDE_DIR}/boost/version.hpp" EME_BOOST_VERSION_LINE REGEX "^#define BOOST_VERSION [0-9]+$")
    string(REGEX REPLACE ".*BOOST_VERSION ([0-9]+).*" "\\1" EME_BOOST_VERSION "${EME_BOOST_VERSION_LINE}")
    if(EME_BOOST_VERSION LESS 108300)
        message(FATAL_ERROR "Read-only capture requires Boost 1.83 or later")
    endif()
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
    add_library(eme_readonly_transport src/transport/readonly_capture.cpp)
    target_include_directories(eme_readonly_transport SYSTEM PRIVATE ${EME_BOOST_INCLUDE_DIR})
    target_include_directories(eme_readonly_transport PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(eme_readonly_transport PUBLIC eme_session PRIVATE OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json)
    eme_enable_warnings(eme_readonly_transport)
    if(MSVC)
        target_compile_options(eme_readonly_transport PRIVATE /bigobj)
    endif()
    add_executable(eme-capture src/transport/capture_main.cpp)
    target_include_directories(eme-capture PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(eme-capture PRIVATE eme_readonly_transport nlohmann_json::nlohmann_json)
    eme_enable_warnings(eme-capture)
    if(BUILD_TESTING AND EME_BUILD_CLI)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        find_program(EME_OPENSSL_EXECUTABLE openssl REQUIRED)
        if(NOT EXISTS "${EME_OPENSSL_EXECUTABLE}")
            message(FATAL_ERROR "Transport tests require an existing OpenSSL CLI executable")
        endif()
        add_executable(eme_capture_fixture_client tests/capture_fixture_client.cpp)
        target_include_directories(eme_capture_fixture_client PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(eme_capture_fixture_client PRIVATE eme_readonly_transport nlohmann_json::nlohmann_json)
        eme_enable_warnings(eme_capture_fixture_client)
        add_test(NAME eme_capture_transport_tests COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/capture_transport_tests.py
            --client $<TARGET_FILE:eme_capture_fixture_client>
            --engine $<TARGET_FILE:event-engine>
            --openssl ${EME_OPENSSL_EXECUTABLE}
            --metadata ${PROJECT_SOURCE_DIR}/examples/metadata.snapshot.json)
        set_tests_properties(eme_capture_transport_tests PROPERTIES TIMEOUT 90)
    endif()
endif()
