# graphics 负责从原生冻结桌面派生 BGRA8 预览，并管理截图选区与覆盖窗口呈现。
add_subdirectory(Capture)
add_subdirectory(HDR)

add_library(open_st_graphics STATIC)
add_library(OpenST::Graphics ALIAS open_st_graphics)

target_sources(open_st_graphics
    PRIVATE
        source/sdr_selection_frame.cpp
        source/selection_output_renderer.cpp
        source/desktop_preview.cpp
        source/outside_mask_layout.cpp
        source/overlay_renderer.cpp
        source/overlay_settings.cpp
        source/selection_model.cpp
        private/outside_mask_layout.h
        private/overlay_settings.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/sdr_selection_frame.h
            include/selection_output_renderer.h
            include/desktop_preview.h
            include/overlay_renderer.h
            include/selection_model.h
)

target_link_libraries(open_st_graphics
    PUBLIC
        OpenST::Capture
        OpenST::HDR
    PRIVATE
        OpenST::Common
        d2d1
        d3d11
        dxgi
)

target_include_directories(open_st_graphics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
