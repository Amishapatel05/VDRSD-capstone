cmake_minimum_required(VERSION 3.28)
project(vdrd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)

# P0: stdlib-only. nuRaft arrives in P3 behind this option so P0 builds offline.
option(VDR_WITH_NURAFT "Fetch and link nuRaft (P3+)" OFF)

add_executable(vdrd src/main.cpp)
target_include_directories(vdrd PRIVATE src)

add_executable(block_store_demo src/block_store_demo.cpp)
target_include_directories(block_store_demo PRIVATE src)

add_executable(nbd_demo src/nbd_demo.cpp)
target_include_directories(nbd_demo PRIVATE src)
target_link_libraries(nbd_demo PRIVATE pthread)

if(VDR_WITH_NURAFT)
  find_package(OpenSSL REQUIRED)
  find_package(ZLIB REQUIRED)
  include(FetchContent)
  FetchContent_Declare(
    nuraft
    GIT_REPOSITORY https://github.com/eBay/NuRaft.git
    GIT_TAG master
    GIT_SUBMODULES ""
  )
  FetchContent_MakeAvailable(nuraft)
  target_link_libraries(vdrd PRIVATE nuraft OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
endif()
