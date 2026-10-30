include(FetchContent)
set(FETCHCONTENT_QUIET ON)

find_package(Threads REQUIRED)

# JSON (config files, web API payloads)
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
endif()

# HTTP server for the dashboard (header only, no TLS needed on localhost)
find_package(httplib 0.18 QUIET)
if(NOT httplib_FOUND)
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.1
    GIT_SHALLOW TRUE)
  set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE INTERNAL "")
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE INTERNAL "")
  set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE INTERNAL "")
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(httplib)
endif()

if(ALGO_WITH_PYTHON)
  find_package(Python3 COMPONENTS Interpreter Development.Embed REQUIRED)
  # Prefer a pip-installed pybind11 (python -m pybind11 --cmakedir), else fetch.
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
    OUTPUT_VARIABLE _pybind11_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_pybind11_dir)
    list(APPEND CMAKE_PREFIX_PATH ${_pybind11_dir})
  endif()
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 2.11 CONFIG QUIET)
  if(NOT pybind11_FOUND)
    FetchContent_Declare(pybind11
      GIT_REPOSITORY https://github.com/pybind/pybind11.git
      GIT_TAG v2.13.6
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(pybind11)
  endif()
endif()

if(ALGO_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
