// 文件职责：验证应用层诊断文本转换的常规输入、空输入和非法 UTF-16 边界。

#include <diagnostic_text.h>

#include <gtest/gtest.h>

// 验证有效 UTF-16 诊断文本转换为等价 UTF-8 文本。
// 入参：无运行时形参；宏参数 DiagnosticTextTest 为测试套件，converts_valid_text 为用例名。
// 返回：gTest 断言结果。
TEST(DiagnosticTextTest, converts_valid_text)
{
    EXPECT_EQ(open_st::WideToUtf8(L"capture failed"), "capture failed");
}

// 验证空诊断文本返回空结果供调用点兜底。
// 入参：无运行时形参；宏参数 DiagnosticTextTest 为测试套件，returns_empty_for_empty_text 为用例名。
// 返回：gTest 断言结果。
TEST(DiagnosticTextTest, returns_empty_for_empty_text)
{
    EXPECT_TRUE(open_st::WideToUtf8(L"").empty());
}

// 验证孤立 UTF-16 代理项被拒绝而不会向 noexcept 调用方传播异常。
// 入参：无运行时形参；宏参数 DiagnosticTextTest 为测试套件，rejects_invalid_utf16 为用例名。
// 返回：gTest 断言结果。
TEST(DiagnosticTextTest, rejects_invalid_utf16)
{
    const std::wstring invalid(1, static_cast<wchar_t>(0xD800));
    EXPECT_TRUE(open_st::WideToUtf8(invalid).empty());
}
