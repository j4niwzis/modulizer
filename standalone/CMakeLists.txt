cmake_minimum_required(VERSION 4.1.1 FATAL_ERROR)

include(${CMAKE_CURRENT_LIST_DIR}/../cmake/import-std.cmake)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

project(modulizer_client LANGUAGES CXX)

# ---- Options ----

option(ENABLE_TEST_COVERAGE "Enable test coverage" OFF)
option(TEST_INSTALLED_VERSION "Test the version found by find_package" OFF)

# --- Import tools ----

include(../cmake/tools.cmake)

# ---- Dependencies ----

include(../cmake/CPM.cmake)

find_package(PkgConfig REQUIRED)

if(TEST_INSTALLED_VERSION)
  find_package(modulizer REQUIRED)
else()
  CPMAddPackage(NAME modulizer SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
endif()

# ---- Create binary ----

file(GLOB_RECURSE module_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

list(REMOVE_ITEM module_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc")

add_executable(${PROJECT_NAME} ${module_sources} ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cc)

target_sources(${PROJECT_NAME} PUBLIC FILE_SET CXX_MODULES FILES ${module_sources})

target_include_directories(${PROJECT_NAME} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 23)

# ---- Link ----

target_link_libraries(${PROJECT_NAME}
  modulizer::modulizer
)

# ---- Install rules ----

include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---- code coverage ----

if(ENABLE_TEST_COVERAGE)
  target_compile_options(${PROJECT_NAME} INTERFACE -O0 -g -fprofile-arcs -ftest-coverage)
  target_link_options(${PROJECT_NAME} INTERFACE -fprofile-arcs -ftest-coverage)
endif()
