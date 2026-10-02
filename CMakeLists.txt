cmake_minimum_required(VERSION 3.28)
project(vdrd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# P0: stdlib-only. nuRaft arrives in P3 behind this option so P0 builds offline.
option(VDR_WITH_NURAFT "Fetch and link nuRaft (P3+)" OFF)

add_executable(vdrd src/main.cpp)
target_include_directories(vdrd PRIVATE src)
target_compile_options(vdrd PRIVATE -Wall -Wextra)

add_executable(block_store_demo src/block_store_demo.cpp)
target_include_directories(block_store_demo PRIVATE src)
target_compile_options(block_store_demo PRIVATE -Wall -Wextra)

add_executable(nbd_demo src/nbd_demo.cpp)
target_include_directories(nbd_demo PRIVATE src)
target_compile_options(nbd_demo PRIVATE -Wall -Wextra)
target_link_libraries(nbd_demo PRIVATE pthread)

include(CTest)
add_test(NAME block_store_demo COMMAND block_store_demo)
add_test(NAME nbd_demo COMMAND nbd_demo)

if(VDR_WITH_NURAFT)
  find_package(OpenSSL REQUIRED)
  find_package(ZLIB REQUIRED)
  find_package(Threads REQUIRED)
  include(FetchContent)
  FetchContent_Declare(
    nuraft
    GIT_REPOSITORY https://github.com/eBay/NuRaft.git
    GIT_TAG master
    GIT_SUBMODULES asio
  )
  FetchContent_MakeAvailable(nuraft)
  target_compile_definitions(vdrd PRIVATE VDR_WITH_NURAFT)
  target_include_directories(vdrd PRIVATE ${nuraft_SOURCE_DIR}/include)
  target_link_libraries(vdrd PRIVATE nuraft OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)
endif()
