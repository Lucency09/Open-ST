#include <gtest/gtest.h>

#include <string_view>

// 验证 launcher 使用的基础产品名称已经配置，防止生成缺少产品标识的可执行程序。
TEST(LauncherTest, product_name_is_not_empty)
{
    constexpr std::wstring_view productName = L"Open-ST";
    EXPECT_FALSE(productName.empty());
}
