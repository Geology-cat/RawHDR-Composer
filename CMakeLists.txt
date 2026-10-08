cmake_minimum_required(VERSION 3.20)

# ---- 対応環境 ----------------------------------------------------------
# macOS 10.12.6 (Sierra) 以降（Intel）。Apple シリコンはもともと macOS 11 以降。
# Xcode 16 が公式に対応する最低は 10.13 だが、10.12 向けにもビルドでき、使う API はすべて 10.12 にある
# （下の -Werror=unguarded-availability で機械的に確かめる）。
# project() より前に設定しないと、SDKの既定値（＝ホストのmacOSの版）が使われてしまう。
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "最低対応macOSバージョン")

# Universal 2 は -DRBH_UNIVERSAL=ON で有効にする。
option(RBH_UNIVERSAL "x86_64 + arm64 の Universal 2 としてビルドする" OFF)
if(RBH_UNIVERSAL)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "" FORCE)
endif()

project(RawHDRComposer VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- 共通コンパイルフラグ ----------------------------------------------
add_library(rbh_flags INTERFACE)
target_compile_options(rbh_flags INTERFACE
    -Wall
    -Wextra
    # 配置ターゲット（10.12）より新しいAPIを使ったらコンパイルエラーにする。
    # libc++ の std::filesystem などにも availability 注釈が付いているので、機械的に弾ける。
    -Werror=unguarded-availability
    -Werror=unguarded-availability-new
)

add_subdirectory(third_party/LibRaw)
add_subdirectory(hdrcore)
add_subdirectory(hdrcli)
add_subdirectory(app)

enable_testing()
add_subdirectory(tests)
