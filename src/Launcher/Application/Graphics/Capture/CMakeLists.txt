# capture 负责在任何遮罩窗口出现前按显示输出保留不可变 SDR/HDR 原生 plane。
add_library(open_st_capture STATIC)
add_library(OpenST::Capture ALIAS open_st_capture)

target_sources(open_st_capture
    PRIVATE
        source/bgra_frame.cpp
        source/captured_plane_writer.cpp
        source/desktop_capturer.cpp
        source/display_color_info.cpp
        source/frozen_desktop_frame.cpp
        source/geometry.cpp
        source/output_capture.cpp
        source/output_capture_policy.cpp
        private/captured_plane_writer.h
        private/display_color_info.h
        private/output_capture.h
        private/output_capture_policy.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/bgra_frame.h
            include/desktop_capturer.h
            include/display_color_state.h
            include/frozen_desktop_frame.h
            include/geometry.h
)

target_include_directories(open_st_capture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")

target_link_libraries(open_st_capture
    PUBLIC
        open_st_project_options
    PRIVATE
        OpenST::Common

        d3d11
        dxgi
        user32
        
)
