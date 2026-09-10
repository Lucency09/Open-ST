# 构建独立贴图窗口模块，声明图像、交互、窗口与 GPU 绘制实现及 SDK 依赖。

add_library(open_st_pin_window STATIC)
add_library(OpenST::PinWindow ALIAS open_st_pin_window)
target_sources(open_st_pin_window
    PRIVATE
        source/pin_image.cpp
        source/pin_interaction_model.cpp
        source/pin_window.cpp
        source/pin_window_manager.cpp
        source/pin_renderer.cpp
        private/pin_interaction_model.h
        private/pin_window.h
        private/pin_renderer.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES include/pin_image.h include/pin_window_manager.h
)
target_include_directories(open_st_pin_window PRIVATE private)
target_link_libraries(open_st_pin_window
    PUBLIC open_st_project_options
    PRIVATE OpenST::Common user32 gdi32 d3d11 dxgi d2d1 dcomp
)
