cmake_minimum_required(VERSION 3.16)
project(swarmbit LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Homebrew installs the CMake package as "LibtorrentRasterbar"; on Linux
# distros it is commonly "libtorrent-rasterbar". Try both and pick the
# imported target that actually exists.
find_package(LibtorrentRasterbar QUIET)
find_package(libtorrent-rasterbar QUIET)

set(TORRENT_TARGET "")
if(TARGET LibtorrentRasterbar::torrent-rasterbar)
  set(TORRENT_TARGET LibtorrentRasterbar::torrent-rasterbar)
elseif(TARGET LibtorrentRasterbar::libtorrent-rasterbar)
  set(TORRENT_TARGET LibtorrentRasterbar::libtorrent-rasterbar)
elseif(TARGET libtorrent-rasterbar::libtorrent-rasterbar)
  set(TORRENT_TARGET libtorrent-rasterbar::libtorrent-rasterbar)
else()
  message(FATAL_ERROR "Could not find a libtorrent-rasterbar imported target")
endif()

add_library(swarmbit STATIC
  src/session_manager.cpp
  src/torrent_handle.cpp
  src/alert_monitor.cpp
)
target_include_directories(swarmbit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(swarmbit PUBLIC ${TORRENT_TARGET})

add_executable(swarmbit-cli src/main.cpp)
target_link_libraries(swarmbit-cli PRIVATE swarmbit)

enable_testing()
add_executable(swarmbit-tests tests/test_torrent.cpp)
target_link_libraries(swarmbit-tests PRIVATE swarmbit)
add_test(NAME swarmbit-tests COMMAND swarmbit-tests)

# Boost is a dependency of libtorrent. Homebrew installs it at a standard
# location; add it if not already on the include path.
find_path(BOOST_INCLUDE_DIR boost/config.hpp
  PATHS /opt/homebrew/opt/boost/include /usr/local/opt/boost/include
  NO_DEFAULT_PATH)
if(BOOST_INCLUDE_DIR)
  target_include_directories(swarmbit PUBLIC ${BOOST_INCLUDE_DIR})
endif()

# libtorrent is built with SSL support; link OpenSSL.
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  target_link_libraries(swarmbit PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(swarmbit PUBLIC TORRENT_USE_SSL=1)
endif()
