# 独立使用 Win32 工具链生成真实异架构 DLL，不依赖新工程或旧工程的运行库目标。
cmake_minimum_required(VERSION 3.28)
project(dfy_loader_x86_fixture LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_library(dfy_loader_x86 SHARED ../dependency.c)
