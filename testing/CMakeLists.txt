# testing 是测试基础设施的唯一入口。
# Mock 必须先于测试加载，使测试目标能够链接对应的 gMock target。
add_subdirectory(mock)
add_subdirectory(test)