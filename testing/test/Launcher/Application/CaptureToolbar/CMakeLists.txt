# 工具栏测试独立链接自身模块，防止借 Application 引入隐藏产品依赖。
add_executable(open_st_capture_toolbar_tests
    source/toolbar_geometry_tests.cpp
    source/capture_toolbar_window_tests.cpp
)
target_link_libraries(open_st_capture_toolbar_tests PRIVATE
    OpenST::CaptureToolbar GTest::gtest_main user32 gdi32 comctl32)
target_include_directories(open_st_capture_toolbar_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/CaptureToolbar/private")
target_link_options(open_st_capture_toolbar_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")

include(GoogleTest)
gtest_discover_tests(open_st_capture_toolbar_tests TEST_PREFIX "capture_toolbar."
    PROPERTIES LABELS "capture_toolbar;unit")

# 显式开启的人工窗口必须等待真人关闭，不能继承普通回归的超时。
add_executable(open_st_capture_toolbar_manual_tests source/capture_toolbar_manual_tests.cpp)
target_link_libraries(open_st_capture_toolbar_manual_tests PRIVATE
    OpenST::CaptureToolbar GTest::gtest_main user32 gdi32 comctl32)
target_link_options(open_st_capture_toolbar_manual_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
gtest_discover_tests(open_st_capture_toolbar_manual_tests TEST_PREFIX "capture_toolbar."
    PROPERTIES LABELS "capture_toolbar;manual" TIMEOUT 0)
