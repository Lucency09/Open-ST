# HDR 独立拥有颜色解码与 Windows 原生色调映射，不反向依赖 Graphics 或兄弟 Capture。
add_library(open_st_hdr STATIC)
add_library(OpenST::HDR ALIAS open_st_hdr)

target_sources(open_st_hdr
    PRIVATE
        source/color_conversion.cpp
        source/native_tone_mapper.cpp
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
