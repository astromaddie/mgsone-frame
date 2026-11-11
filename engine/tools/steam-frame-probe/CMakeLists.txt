cmake_minimum_required(VERSION 3.20)
project(mgs_frame_probe LANGUAGES C CXX)

find_package(Vulkan REQUIRED)
include(../../cmake/MgsOpenXR.cmake)

add_executable(mgs_frame_probe main.cpp)
target_compile_features(mgs_frame_probe PRIVATE cxx_std_20)
target_compile_options(mgs_frame_probe PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(mgs_frame_probe PRIVATE Vulkan::Vulkan OpenXR::openxr_loader)
