# 组织测试与 mock 构建入口，产品构建关闭测试时不加载此目录。

# testing 是测试基础设施的唯一入口。
# Mock 必须先于测试加载，使测试目标能够链接对应的 gMock target。
add_subdirectory(mock)
add_subdirectory(test)
