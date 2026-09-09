# 配置 Launcher/Application/Graphics/Capture 的测试替身构建与逐级目录入口。

# capture 后续在抽象显示输出和 D3D 设备边界后，在这里提供捕获相关 gMock。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
