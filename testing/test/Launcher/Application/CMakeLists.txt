# 配置 Launcher/Application 的镜像测试目标、链接依赖及用例发现。

# application 会话测试使用隐藏 HWND 验证逐输出登记与同步销毁边界，不显示桌面遮罩。
add_subdirectory(CaptureToolbar)
add_subdirectory(Localization)
add_subdirectory(PinWindow)
add_subdirectory(Export)
add_subdirectory(Graphics)
add_subdirectory(Hotkeys)
add_subdirectory(Settings)
add_subdirectory(SystemIntegration)

add_executable(open_st_application_tests source/capture_overlay_session_tests.cpp source/capture_completion_tests.cpp
    source/diagnostic_text_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/simple_message_window_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/overlay_paint_batch_tests.cpp
    source/app_overlay_render_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/annotation_integration_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/overlay_input_queue_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/capture_annotation_state_tests.cpp
    source/annotation_style_dialog_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/capture_toolbar_integration_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/pin_window_integration_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/hotkey_integration_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/save_image_dialog_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/log_maintenance_task_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/capture_selection_input_tests.cpp
    source/window_selection_integration_tests.cpp)
target_link_libraries(open_st_application_tests PRIVATE OpenST::Hotkeys OpenST::Export OpenST::Common)
target_include_directories(open_st_application_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Hotkeys/private")
target_link_libraries(open_st_application_tests PRIVATE OpenST::PinWindow)
target_include_directories(open_st_application_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/PinWindow/private")
target_link_libraries(open_st_application_tests PRIVATE OpenST::Application OpenST::Graphics OpenST::WindowRenderer OpenST::CaptureToolbar GTest::gtest_main)
target_link_options(open_st_application_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
target_include_directories(open_st_application_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private")
gtest_discover_tests(open_st_application_tests TEST_PREFIX "application." PROPERTIES LABELS "application;integration")

# 窗口快照真实桌面检查独立串行，避免与其他桌面用例互相改变层级。
add_executable(open_st_window_selection_tests source/window_selection_snapshot_tests.cpp)
target_link_libraries(open_st_window_selection_tests PRIVATE OpenST::Application OpenST::Graphics
    GTest::gtest_main user32 dwmapi)
target_include_directories(open_st_window_selection_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private")
gtest_discover_tests(open_st_window_selection_tests TEST_PREFIX "application."
    PROPERTIES LABELS "application;desktop" TIMEOUT 45 RUN_SERIAL TRUE)

# 独立链接系统输出替身，验证真实 App 导出编排而不操作用户剪贴板或文件对话框。
add_executable(open_st_pin_export_integration_tests source/pin_export_integration_tests.cpp)
target_link_libraries(open_st_pin_export_integration_tests PRIVATE
    OpenST::Application OpenST::Graphics OpenST::PinWindow OpenST::Export OpenST::Capture OpenST::Settings OpenST::Localization OpenST::Common GTest::gtest_main)
target_include_directories(open_st_pin_export_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private"
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Settings/private"
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Localization/private"
    "${PROJECT_SOURCE_DIR}/src/Common/private")
gtest_discover_tests(open_st_pin_export_integration_tests TEST_PREFIX "application."
    PROPERTIES LABELS "application;integration" TIMEOUT 45)

# 独立替换样式弹窗，验证右键元素属性的真实 App 事务和模态边界。
add_executable(open_st_annotation_properties_tests source/annotation_properties_tests.cpp)
target_link_libraries(open_st_annotation_properties_tests PRIVATE
    OpenST::Application OpenST::Graphics OpenST::CaptureToolbar GTest::gtest_main)
target_include_directories(open_st_annotation_properties_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private")
gtest_discover_tests(open_st_annotation_properties_tests TEST_PREFIX "application."
    PROPERTIES LABELS "application;integration" TIMEOUT 45)

# 原位文字窗口真实集成，与属性弹窗替身隔离。
add_executable(open_st_annotation_text_integration_tests source/annotation_text_integration_tests.cpp)
target_link_libraries(open_st_annotation_text_integration_tests PRIVATE
    OpenST::Application OpenST::Graphics OpenST::WindowRenderer OpenST::Hotkeys GTest::gtest_main)
target_include_directories(open_st_annotation_text_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private"
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Hotkeys/private"
    "${PROJECT_SOURCE_DIR}/src/Common/WindowRenderer/private")
gtest_discover_tests(open_st_annotation_text_integration_tests TEST_PREFIX "application."
    PROPERTIES LABELS "application;integration" TIMEOUT 45)
