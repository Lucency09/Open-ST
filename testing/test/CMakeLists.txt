# 加载 Common 与 Launcher 的镜像测试树，逐级注册自动测试。

# test 与 src 使用相同的目录依赖树。每个测试模块只测试对应的源码模块。
# GoogleTest 模块提供 gtest_discover_tests()，由测试程序自动枚举 TEST/TEST_F case。
include(GoogleTest)

add_subdirectory(Common)
add_subdirectory(Launcher)
