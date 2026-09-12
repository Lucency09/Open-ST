# 配置 Launcher/Application 的测试替身构建与逐级目录入口。

# application 后续在抽象托盘、快捷键和捕获会话边界后，在这里提供应用协调 gMock。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
add_subdirectory(Export)
