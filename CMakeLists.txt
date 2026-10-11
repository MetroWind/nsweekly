# cmake -B build . && cmake --build build -j24
#
# Use -DMACRODOWN_BOOTSTRAP_EMSDK=ON to bootstrap Emscripten.
cmake_minimum_required(VERSION 3.24)

set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

project(Weekly)

# Set our default before dependencies can declare BUILD_TESTING as ON.
option(BUILD_TESTING "Build nsweekly tests" OFF)
include(FetchContent)
FetchContent_Declare(
  cxxopts
  GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
  GIT_TAG v3.1.1
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
FetchContent_MakeAvailable(json inja cxxopts ryml spdlog)
unset(BUILD_BENCHMARK)

FetchContent_Declare(
  libmw
  GIT_REPOSITORY https://github.com/MetroWind/libmw.git
  GIT_TAG 2d9bcbd
)
set(LIBMW_BUILD_HTTP_SERVER ON CACHE BOOL "Build libmw HTTP server")
set(LIBMW_BUILD_CRYPTO ON CACHE BOOL "Build libmw crypto" FORCE)
FetchContent_MakeAvailable(libmw)

FetchContent_Declare(
  macrodown
  GIT_REPOSITORY https://git.xeno.darksair.org/macrodown.git
)
FetchContent_MakeAvailable(macrodown)

# Build browser assets from the same MacroDown checkout as the native library.
find_program(EMCMAKE_EXECUTABLE emcmake REQUIRED)
include(ExternalProject)
ExternalProject_Add(macrodown_browser
  SOURCE_DIR "${macrodown_SOURCE_DIR}"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/macrodown_wasm"
  # makepkg's native compiler flags do not apply to the Wasm target.
  CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
    --unset=CFLAGS --unset=CXXFLAGS --unset=CPPFLAGS --unset=LDFLAGS
    "${EMCMAKE_EXECUTABLE}" "${CMAKE_COMMAND}"
    -S <SOURCE_DIR> -B <BINARY_DIR>
    -DCMAKE_BUILD_TYPE=MinSizeRel
    # Also clear native flags cached by an earlier configure.
    -DCMAKE_C_FLAGS:STRING=
    -DCMAKE_CXX_FLAGS:STRING=
    -DCMAKE_EXE_LINKER_FLAGS:STRING=
    -DCMAKE_SHARED_LINKER_FLAGS:STRING=
    -DCMAKE_MODULE_LINKER_FLAGS:STRING=
    -DMACRODOWN_BUILD_WASM=ON
    -DMACRODOWN_BUILD_CLI=OFF
    -DMACRODOWN_BUILD_TESTS=OFF
    "-DFETCHCONTENT_SOURCE_DIR_UNI-ALGO=${uni-algo_SOURCE_DIR}"
  BUILD_COMMAND "${CMAKE_COMMAND}" -E env
    --unset=CFLAGS --unset=CXXFLAGS --unset=CPPFLAGS --unset=LDFLAGS
    "${CMAKE_COMMAND}" --build <BINARY_DIR>
    --target macrodown_wasm -j24
  INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/statics/macrodown"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    <BINARY_DIR>/web/macrodown.mjs <BINARY_DIR>/web/macrodown.wasm
    "${CMAKE_CURRENT_SOURCE_DIR}/statics/macrodown"
  BUILD_ALWAYS TRUE
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(MAGIC REQUIRED IMPORTED_TARGET libmagic)
pkg_check_modules(MAGICK REQUIRED IMPORTED_TARGET Magick++)
find_package(CURL REQUIRED)
find_package(SQLite3 3.35.0 REQUIRED)

set(SOURCE_FILES
  src/attachment/attachment.cpp
  src/attachment/attachment_data.cpp
  src/attachment/attachment_files.cpp
  src/attachment/attachment_service.cpp
  src/attachment/image_variant.cpp
  src/attachment/attachment_module.cpp
  src/open_graph.cpp
  src/games/game_journal.cpp
  src/games/game_journal_data.cpp
  src/games/game_review.cpp
  src/games/game_review_data.cpp
  src/games/game.cpp
  src/games/game_data.cpp
  src/markdown.cpp
  src/games/games_module.cpp
  src/games/csv_reader.cpp
  src/games/game_import.cpp
  src/games/review_import.cpp
  src/app.cpp
  src/app.hpp
  src/auth/auth.cpp
  src/auth/auth.hpp
  src/config.cpp
  src/config.hpp
  src/auth/auth_module.cpp
  src/auth/auth_module.hpp
  src/session_service.cpp
  src/session_service.hpp
  src/weekly/weekly_module.cpp
  src/weekly/weekly_module.hpp
  src/weekly/weekly_data.cpp
  src/weekly/weekly_data.hpp
  src/user_data.cpp
  src/user_data.hpp
  src/user_sql.cpp
  src/user_sql.hpp
  src/route_urls.cpp
  src/route_urls.hpp
  src/http_adaptation.cpp
  src/http_adaptation.hpp
  src/database.cpp
  src/database.hpp
  src/error.hpp
  src/http_client.cpp
  src/http_client.hpp
  src/url.cpp
  src/url.hpp
  src/utils.hpp
  src/weekly/weekly.cpp
  src/weekly/weekly.hpp
)
set(LIBS
  MacroDown::MacroDown
  cxxopts
  mw::crypto
  PkgConfig::MAGIC
  PkgConfig::MAGICK
  mw::http-server
  spdlog::spdlog
  ryml::ryml
  ${CURL_LIBRARIES}
  ${SQLite3_LIBRARIES}
)

set(INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CURL_INCLUDE_DIR}
  ${json_SOURCE_DIR}/single_include
  ${inja_SOURCE_DIR}/single_include/inja
  ${SQLite3_INCLUDE_DIRS}
)

add_executable(nsweekly ${SOURCE_FILES} src/main.cpp)
add_dependencies(nsweekly macrodown_browser)
set_property(TARGET nsweekly PROPERTY CXX_STANDARD 23)

set_property(TARGET nsweekly PROPERTY COMPILE_WARNING_AS_ERROR TRUE)
target_compile_options(nsweekly PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(nsweekly PRIVATE ${INCLUDES})
target_link_libraries(nsweekly PRIVATE ${LIBS})

if(BUILD_TESTING)
  include(CTest)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  )
  FetchContent_MakeAvailable(googletest)

  set(TEST_FILES
    src/config_test.cpp
    src/attachment/attachment_test.cpp
    src/open_graph_test.cpp
    src/games/game_journal_test.cpp
    src/games/game_review_test.cpp
    src/games/game_test.cpp
    src/games/games_module_test.cpp
    src/test_utils.hpp
    src/http_client_test.cpp
    src/http_client_mock.hpp
    src/auth/auth_test.cpp
    src/auth/auth_mock.hpp
    src/url_test.cpp
    src/session_service_test.cpp
    src/routes_test.cpp
    src/route_urls_test.cpp
    src/app_test.cpp
    src/auth/auth_module_test.cpp
    src/weekly/weekly_module_test.cpp
    src/http_adaptation_test.cpp
    src/user_data_test.cpp
    src/weekly/weekly_test.cpp
    src/weekly/weekly_data_test.cpp
    src/storage_mock.hpp
    src/storage_test_support.hpp
    src/database_test.cpp
    src/utils_test.cpp
  )

  # ctest --test-dir build
  add_executable(nsweekly_test ${SOURCE_FILES} ${TEST_FILES})
  add_dependencies(nsweekly_test macrodown_browser)
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

  find_program(NODE_EXECUTABLE node)
  if(NODE_EXECUTABLE)
    add_test(NAME WeeklyBrowser.Wasm
      COMMAND ${NODE_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/weekly_wasm_test.mjs)
    add_test(NAME WeeklyBrowser.Preview
      COMMAND ${NODE_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/weekly_preview_test.js)
    set_tests_properties(WeeklyBrowser.Preview PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME JournalBrowser.AttachmentMarkdown
      COMMAND ${NODE_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/journal_markdown_test.js)
    add_test(NAME ReviewBrowser.ScoreFormula
      COMMAND ${NODE_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/reviews_score_test.js)
  endif()

  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(APACHE_EXECUTABLE NAMES httpd apache2)
  if(Python3_Interpreter_FOUND AND APACHE_EXECUTABLE)
    add_test(NAME AttachmentApache.DirectServing
      COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/apache_attachments_test.py
        ${APACHE_EXECUTABLE})
    set_tests_properties(AttachmentApache.DirectServing PROPERTIES
      SKIP_RETURN_CODE 77)
  endif()

  include(GoogleTest)
  gtest_discover_tests(nsweekly_test)

  target_compile_definitions(nsweekly_test PRIVATE
    NSWEEKLY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
