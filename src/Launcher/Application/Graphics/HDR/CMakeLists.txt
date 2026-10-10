# 配置 Launcher/Application/Graphics/HDR 模块的源码、公开头及构建依赖。

# HDR 独立拥有颜色解码与 Windows 原生色调映射，不反向依赖 Graphics 或兄弟 Capture。
add_library(open_st_hdr STATIC)
add_library(OpenST::HDR ALIAS open_st_hdr)

# 从当前开发环境的 Windows SDK 选择 fxc，构建期生成直接嵌入的 C++ 字节码。
set(_hdr_sdk_root "$ENV{WindowsSdkDir}")
if(NOT _hdr_sdk_root)
    get_filename_component(_hdr_sdk_root
        "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE)
endif()
set(_hdr_sdk_version "${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
if(NOT _hdr_sdk_version)
    set(_hdr_sdk_version "$ENV{WindowsSDKVersion}")
    string(REGEX REPLACE "[/\\\\]+$" "" _hdr_sdk_version "${_hdr_sdk_version}")
endif()
if(NOT _hdr_sdk_version)
    file(GLOB _hdr_sdk_versions RELATIVE "${_hdr_sdk_root}/Include" "${_hdr_sdk_root}/Include/10.*")
    list(SORT _hdr_sdk_versions COMPARE NATURAL ORDER DESCENDING)
    list(GET _hdr_sdk_versions 0 _hdr_sdk_version)
endif()
find_program(_hdr_fxc NAMES fxc.exe PATHS "${_hdr_sdk_root}/bin/${_hdr_sdk_version}/x64" NO_DEFAULT_PATH NO_CACHE REQUIRED)
set(_hdr_shader_header "${CMAKE_CURRENT_BINARY_DIR}/generated/adaptive_shoulder_shader.h")
set(_hdr_shader_source "${PROJECT_SOURCE_DIR}/resources/shaders/adaptive_shoulder.hlsl")
add_custom_command(OUTPUT "${_hdr_shader_header}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${_hdr_fxc}" /nologo /T ps_4_0 /E main /O3 /Ges
        /D D2D_ENTRY=main /D D2D_FULL_SHADER
        /I "${_hdr_sdk_root}/Include/${_hdr_sdk_version}/um"
        /Fh "${_hdr_shader_header}" /Vn g_adaptiveShoulderShader
        "${_hdr_shader_source}"
    DEPENDS "${_hdr_shader_source}"
    VERBATIM
)
target_include_directories(open_st_hdr PRIVATE private "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_sources(open_st_hdr
    PRIVATE
        source/color_conversion.cpp
        source/native_tone_mapper.cpp
        source/adaptive_shoulder_effect.cpp
        private/adaptive_shoulder_effect.h
        private/native_tone_mapper_test_access.h
        "${_hdr_shader_header}"
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/color_conversion.h
            include/hdr_image_view.h
            include/native_tone_mapper.h
)

target_link_libraries(open_st_hdr
    PUBLIC
        open_st_project_options
    PRIVATE
        OpenST::Common
        d2d1
        d3d11
        dxgi
        dxguid
)
