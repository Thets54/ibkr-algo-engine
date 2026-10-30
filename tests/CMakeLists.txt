include(GoogleTest)

add_executable(algo_tests
  test_portfolio.cpp
  test_risk.cpp
  test_metrics.cpp
  test_data.cpp
  test_sim_broker.cpp
  test_engine.cpp
  test_strategy_loader.cpp
  test_session.cpp
  test_web_api.cpp
)
target_link_libraries(algo_tests PRIVATE algo_core GTest::gtest_main)
target_include_directories(algo_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(algo_tests PRIVATE ALGO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

# Each test runs in its own process so the embedded interpreter and
# temporary folders never leak between tests.
gtest_discover_tests(algo_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
