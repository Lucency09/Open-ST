# mock 与 src/test 使用相同的目录依赖树，并且必须在 test 之前加载。
# 这样每个测试模块可以只链接自己需要的 gMock target。
add_subdirectory(Common)
add_subdirectory(Launcher)
