
resolve_env(TORCH_BLADE_USE_ROCM OFF)
if (TORCH_BLADE_USE_ROCM)
    list(APPEND CMAKE_PREFIX_PATH /opt/rocm/hip /opt/rocm)
    find_package(rocblas)
    find_package(rocfft)
    find_package(hipsparse)
    find_package(hiprand)
    find_package(Threads)
    target_include_directories(torch_blade PUBLIC /opt/)
    target_compile_definitions(torch_blade PUBLIC -DTORCH_BLADE_USE_ROCM)
    set(TORCH_ROCM ON)
endif (TORCH_BLADE_USE_ROCM)
