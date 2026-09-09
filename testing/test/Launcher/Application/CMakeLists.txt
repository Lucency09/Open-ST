# application 会话测试使用隐藏 HWND 验证逐输出登记与同步销毁边界，不显示桌面遮罩。
add_subdirectory(CaptureToolbar)
add_subdirectory(Localization)
add_subdirectory(Export)
add_subdirectory(Graphics)
add_subdirectory(Settings)
add_subdirectory(SystemIntegration)

add_executable(open_st_application_tests source/capture_overlay_session_tests.cpp source/capture_completion_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/simple_message_window_tests.cpp)
target_sources(open_st_application_tests PRIVATE source/capture_toolbar_integration_tests.cpp)
target_link_libraries(open_st_application_tests PRIVATE OpenST::Application OpenST::Graphics OpenST::WindowRenderer OpenST::CaptureToolbar GTest::gtest_main)
target_link_options(open_st_application_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
target_include_directories(open_st_application_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/Launcher/Application/private")
gtest_discover_tests(open_st_application_tests TEST_PREFIX "application." PROPERTIES LABELS "application;integration")
