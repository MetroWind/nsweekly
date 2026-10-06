# cmake -B build . && cmake --build build -j24
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
find_package(SQLite3 3.35.0 REQUIRED)
find_package(cmark REQUIRED)

set(SOURCE_FILES
  src/app.cpp
  src/app.hpp
  src/auth.cpp
  src/auth.hpp
  src/config.cpp
  src/config.hpp
  src/auth_module.cpp
  src/auth_module.hpp
  src/session_service.cpp
  src/session_service.hpp
  src/weekly_module.cpp
  src/weekly_module.hpp
  src/weekly_data.cpp
  src/weekly_data.hpp
  src/user_data.cpp
  src/user_data.hpp
  src/user_sql.cpp
  src/user_sql.hpp
  src/route_urls.cpp
  src/route_urls.hpp
  src/http_adaptation.cpp
  src/http_adaptation.hpp
  src/http_response.hpp
  src/database.cpp
  src/database.hpp
  src/error.hpp
  src/http_client.cpp
  src/http_client.hpp
  src/url.cpp
  src/url.hpp
  src/utils.hpp
  src/weekly.cpp
  src/weekly.hpp
)
set(LIBS
  cxxopts
  httplib
  spdlog::spdlog
  ryml::ryml
  ${CURL_LIBRARIES}
  ${SQLite3_LIBRARIES}
  # This can be found in the installed
  # cmark-targets-relwithdebinfo.cmake.
  cmark::cmark
)

set(INCLUDES
  ${CURL_INCLUDE_DIR}
  ${json_SOURCE_DIR}/single_include
  ${inja_SOURCE_DIR}/single_include/inja
  ${SQLite3_INCLUDE_DIRS}
  ${cmark_INCLUDE_DIRS}
)

add_executable(nsweekly ${SOURCE_FILES} src/main.cpp)
set_property(TARGET nsweekly PROPERTY CXX_STANDARD 23)

set_property(TARGET nsweekly PROPERTY COMPILE_WARNING_AS_ERROR TRUE)
target_compile_options(nsweekly PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(nsweekly PRIVATE ${INCLUDES})
target_link_libraries(nsweekly PRIVATE ${LIBS})

set(TEST_FILES
  src/test_utils.hpp
  src/http_client_test.cpp
  src/http_client_mock.hpp
  src/auth_test.cpp
  src/auth_mock.hpp
  src/url_test.cpp
  src/session_service_test.cpp
  src/routes_test.cpp
  src/route_urls_test.cpp
  src/app_test.cpp
  src/auth_module_test.cpp
  src/weekly_module_test.cpp
  src/http_adaptation_test.cpp
  src/user_data_test.cpp
  src/weekly_data_test.cpp
  src/storage_mock.hpp
  src/storage_test_support.hpp
  src/database_test.cpp
  src/utils_test.cpp
)

# ctest --test-dir build
add_executable(nsweekly_test ${SOURCE_FILES} ${TEST_FILES})
set_property(TARGET nsweekly_test PROPERTY CXX_STANDARD 23)

set_property(TARGET nsweekly_test PROPERTY COMPILE_WARNING_AS_ERROR TRUE)
target_compile_options(nsweekly_test PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(nsweekly_test PRIVATE
  ${INCLUDES}
  ${googletest_SOURCE_DIR}/googletest/include
  ${googletest_SOURCE_DIR}/googlemock/include
)

target_link_libraries(nsweekly_test PRIVATE
  ${LIBS}
  GTest::gtest_main
  GTest::gmock_main
)

enable_testing()
include(GoogleTest)
gtest_discover_tests(nsweekly_test)

target_compile_definitions(nsweekly_test PRIVATE
  NSWEEKLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
