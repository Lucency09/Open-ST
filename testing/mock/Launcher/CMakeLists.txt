# launcher 只保留程序入口，通常不需要 mock；若职责扩大再在这里添加。
# 真实 mock 到来时采用 include 与 source 目录，
# 并在本文件创建 OpenST::<Module>Mocks target，私有链接 GTest::gmock。
add_subdirectory(Application)
