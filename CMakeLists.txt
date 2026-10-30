cmake_minimum_required(VERSION 3.20)
project(ibkr_algo_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ALGO_WITH_PYTHON "Enable Python strategies (embedded interpreter via pybind11)" ON)
option(ALGO_WITH_IBKR   "Build the Interactive Brokers adapter (needs the official TWS API C++ SDK)" OFF)
option(ALGO_BUILD_TESTS "Build the unit and integration tests" ON)
set(TWS_API_DIR "" CACHE PATH "Path to IBKR TWS API 'source/cppclient/client' directory")

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Embedded web dashboard: web/index.html is compiled into the binary so the
# app is a single self-contained executable.
# ---------------------------------------------------------------------------
set(ALGO_WEB_HTML ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
set(ALGO_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${ALGO_GENERATED_DIR}/algo/web_assets.hpp
  COMMAND ${CMAKE_COMMAND} -DINPUT=${ALGO_WEB_HTML}
          -DOUTPUT=${ALGO_GENERATED_DIR}/algo/web_assets.hpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  DEPENDS ${ALGO_WEB_HTML} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  COMMENT "Embedding web dashboard")
add_custom_target(algo_web_assets DEPENDS ${ALGO_GENERATED_DIR}/algo/web_assets.hpp)

# ---------------------------------------------------------------------------
# Core library: everything except main(). Tests link against it.
# ---------------------------------------------------------------------------
add_library(algo_core
  src/core/portfolio.cpp
  src/core/metrics.cpp
  src/core/config.cpp
  src/data/csv_feed.cpp
  src/data/bar_aggregator.cpp
  src/risk/risk_manager.cpp
  src/broker/sim_broker.cpp
  src/broker/ibkr_util.cpp
  src/engine/engine.cpp
  src/engine/backtester.cpp
  src/engine/session_manager.cpp
  src/strategy/strategy_loader.cpp
  src/strategy/cpp_plugin.cpp
  src/server/web_server.cpp
)
add_dependencies(algo_core algo_web_assets)
target_include_directories(algo_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${ALGO_GENERATED_DIR})
target_link_libraries(algo_core PUBLIC nlohmann_json::nlohmann_json httplib::httplib ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_definitions(algo_core PUBLIC
  ALGO_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/include"
  ALGO_CXX_COMPILER="${CMAKE_CXX_COMPILER}")

if(MSVC)
  target_compile_options(algo_core PRIVATE /W4)
else()
  target_compile_options(algo_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ALGO_WITH_PYTHON)
  target_sources(algo_core PRIVATE src/strategy/python_strategy.cpp)
  target_link_libraries(algo_core PUBLIC pybind11::embed)
  target_compile_definitions(algo_core PUBLIC ALGO_WITH_PYTHON=1)
endif()

if(ALGO_WITH_IBKR)
  if(NOT TWS_API_DIR)
    message(FATAL_ERROR "ALGO_WITH_IBKR=ON requires -DTWS_API_DIR=<path to TWS API source/cppclient/client>")
  endif()
  file(GLOB TWS_SOURCES ${TWS_API_DIR}/*.cpp)
  add_library(twsapi STATIC ${TWS_SOURCES})
  target_include_directories(twsapi PUBLIC ${TWS_API_DIR})
  # TWS API >= 10.x uses the Intel decimal floating point library for Decimal.
  if(TWS_DECIMAL_LIB)
    target_link_libraries(twsapi PUBLIC ${TWS_DECIMAL_LIB})
  endif()
  target_sources(algo_core PRIVATE src/broker/ibkr_broker.cpp)
  target_link_libraries(algo_core PUBLIC twsapi)
  target_compile_definitions(algo_core PUBLIC ALGO_WITH_IBKR=1)
  # The TWS API changed two callback signatures across 10.x releases. If the
  # adapter fails to compile with "does not override", flip these options.
  # Both changed in TWS API 10.33 (IBKR changelog, Dec 2024). Turn OFF for older SDKs.
  option(TWS_ERROR_WITH_TIME "TWS API >= 10.33: EWrapper::error() takes an errorTime argument" ON)
  option(TWS_CANCEL_WITH_ORDERCANCEL "TWS API >= 10.33: cancelOrder() takes an OrderCancel object" ON)
  if(TWS_ERROR_WITH_TIME)
    target_compile_definitions(algo_core PRIVATE TWS_ERROR_WITH_TIME=1)
  endif()
  if(TWS_CANCEL_WITH_ORDERCANCEL)
    target_compile_definitions(algo_core PRIVATE TWS_CANCEL_WITH_ORDERCANCEL=1)
  endif()
endif()

add_executable(algoengine src/app/main.cpp)
target_link_libraries(algoengine PRIVATE algo_core)

if(ALGO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
