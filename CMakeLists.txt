# cmake -B build . && cmake --build build -j
cmake_minimum_required(VERSION 3.24)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(Weekly)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
)
FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG v3.1.1
)
FetchContent_Declare(
  httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.14.3
)
FetchContent_Declare(
  ryml
  GIT_REPOSITORY https://github.com/biojppm/rapidyaml.git
  GIT_TAG
  GIT_SHALLOW FALSE  # ensure submodules are checked out
)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_Declare(
  inja
  GIT_REPOSITORY https://github.com/pantor/inja.git
  GIT_TAG main
)
include_directories(${json_SOURCE_DIR}/single_include)
set(INJA_USE_EMBEDDED_JSON FALSE)
set(INJA_BUILD_TESTS FALSE)
set(BUILD_BENCHMARK FALSE)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.12.0
)
set(SPDLOG_USE_STD_FORMAT ON)
FetchContent_MakeAvailable(json inja httplib cxxopts googletest ryml spdlog)
unset(BUILD_BENCHMARK)

find_package(CURL REQUIRED)

set(SOURCE_FILES
  src/error.hpp
  src/utils.hpp
  src/auth.hpp
  src/auth.cpp
  src/http_client.hpp
  src/http_client.cpp
)
set(LIBS
  cxxopts
  httplib
  spdlog::spdlog
  ryml::ryml
  ${CURL_LIBRARIES}
)
set(INCLUDES
  ${CURL_INCLUDE_DIR}
)

add_executable(weekly ${SOURCE_FILES} src/main.cpp)
set_property(TARGET weekly PROPERTY CXX_EXTENSIONS FALSE)
set_property(TARGET weekly PROPERTY CXX_STANDARD 23)

set_property(TARGET weekly PROPERTY COMPILE_WARNING_AS_ERROR TRUE)
target_compile_options(weekly PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(weekly PRIVATE ${INCLUDES})
target_link_libraries(weekly PRIVATE ${LIBS})

set(TEST_FILES
  src/http_client_test.cpp
)

# ctest --test-dir build
add_executable(weekly_test ${SOURCE_FILES} ${TEST_FILES})
set_property(TARGET weekly_test PROPERTY CXX_EXTENSIONS FALSE)
set_property(TARGET weekly_test PROPERTY CXX_STANDARD 23)

set_property(TARGET weekly_test PROPERTY COMPILE_WARNING_AS_ERROR TRUE)
target_compile_options(weekly_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(weekly_test PRIVATE
  ${INCLUDES}
  ${googletest_SOURCE_DIR}/googletest/include
)

target_link_libraries(weekly_test PRIVATE ${LIBS} GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(weekly_test)
