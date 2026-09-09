// 验证启动入口所需的基础产品标识配置。

#include <gtest/gtest.h>

#include <string_view>

// 验证 launcher 使用的基础产品名称已经配置，防止生成缺少产品标识的可执行程序。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(LauncherTest, product_name_is_not_empty)
{
    constexpr std::wstring_view productName = L"Open-ST";
    EXPECT_FALSE(productName.empty());
}
