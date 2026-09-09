# 配置 Common/WindowRenderer 的镜像测试目标、链接依赖及用例发现。

add_executable(open_st_window_renderer_tests)

# 控件几何和交互在产品使用的 v6 激活上下文下验证。
target_link_options(open_st_window_renderer_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
target_sources(open_st_window_renderer_tests PRIVATE
    source/renderer_model_tests.cpp
    source/window_renderer_tests.cpp
    source/window_renderer_modal_tests.cpp
)
target_link_libraries(open_st_window_renderer_tests PRIVATE
    OpenST::WindowRenderer GTest::gtest_main
)
target_include_directories(open_st_window_renderer_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/Common/WindowRenderer/private"
)
include(GoogleTest)
gtest_discover_tests(open_st_window_renderer_tests TEST_PREFIX "window_renderer." PROPERTIES LABELS "window_renderer;renderer" TIMEOUT 20)

# 人工验收独立可执行文件；默认跳过，显式启用后无限等待用户关闭。
add_executable(open_st_window_renderer_manual_tests source/window_renderer_manual_tests.cpp)
target_link_libraries(open_st_window_renderer_manual_tests PRIVATE OpenST::WindowRenderer GTest::gtest_main)
target_link_options(open_st_window_renderer_manual_tests PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'"
)
gtest_discover_tests(open_st_window_renderer_manual_tests TEST_PREFIX "window_renderer."
    PROPERTIES LABELS "window_renderer;manual" TIMEOUT 0)
