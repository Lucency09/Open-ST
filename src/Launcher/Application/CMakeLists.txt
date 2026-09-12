# 构建 Application 协调库，加载直接子模块并声明应用编排源码及依赖。

# application 封装应用生命周期、托盘、快捷键和窗口消息协调逻辑。
add_subdirectory(CaptureToolbar)
add_subdirectory(Export)
add_subdirectory(Graphics)
add_subdirectory(Localization)
add_subdirectory(PinWindow)
add_subdirectory(Settings)
add_subdirectory(SystemIntegration)

add_library(open_st_application STATIC)
add_library(OpenST::Application ALIAS open_st_application)

target_sources(open_st_application
    PRIVATE
        source/app.cpp
        source/app_callbacks.cpp
        source/app_pin.cpp
        source/capture_completion.cpp
        source/capture_toolbar_monitor.cpp
        source/diagnostic_text.cpp
        private/capture_toolbar_monitor.h
        private/capture_command_gate.h
        private/diagnostic_text.h
        source/save_image_dialog.cpp
        source/save_directory.cpp
        private/save_directory.h
        source/simple_message_window.cpp
        private/simple_message_window.h
        private/capture_completion.h
        private/save_image_dialog.h
        source/capture_overlay_session.cpp
        private/capture_overlay_session.h
        private/resource.h
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/app.h
)

target_link_libraries(open_st_application
    PUBLIC
        open_st_project_options
    PRIVATE
        # 功能模块只作为 application 的实现细节，不传播给 launcher。
        OpenST::Export
        OpenST::PinWindow
        OpenST::CaptureToolbar
        OpenST::Capture
        OpenST::Graphics
        OpenST::Localization
        OpenST::Settings
        OpenST::Common
        OpenST::WindowRenderer
        OpenST::SystemIntegration

        # 以下均为 Windows SDK 系统库，不属于 vcpkg 第三方依赖。
        ole32
        uuid
        comctl32
        shell32
)

# private/ 只供本模块实现包含；生成的版本头也不对其他模块公开。
target_include_directories(open_st_application
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/private"
        "${PROJECT_BINARY_DIR}/generated"
)
