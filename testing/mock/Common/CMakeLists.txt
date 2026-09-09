# 配置 Common 的测试替身构建与逐级目录入口。

# common 当前没有需要模拟的外部边界；Logger 直接通过文件系统行为测试。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
