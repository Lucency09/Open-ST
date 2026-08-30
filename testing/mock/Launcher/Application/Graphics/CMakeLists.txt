# graphics 后续在抽象交换链和设备创建边界后，在这里提供图形相关 gMock。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
add_subdirectory(Capture)
