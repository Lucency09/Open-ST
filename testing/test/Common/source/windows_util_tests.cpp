// 验证公共 Windows 辅助保持 HRESULT 诊断格式，并按可执行文件而非工作目录定位。

#include <windows_util.h>

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace
{
// 验证共享 HRESULT 格式化保留操作名称、固定文字和无符号大写十六进制表示。
// 入参：无运行入参。
// 返回：无返回值；通过 GoogleTest 断言记录正常值及失败高位的格式结果。
TEST(WindowsUtilTest, hresult_format_preserves_existing_diagnostics)
{
    EXPECT_EQ(open_st::FormatHResult(L"创建设备", E_FAIL), L"创建设备失败，HRESULT=0x80004005");
    EXPECT_EQ(open_st::FormatHResult(L"", S_OK), L"失败，HRESULT=0x0");
    EXPECT_EQ(open_st::FormatHResult(L"测试", E_ACCESSDENIED), L"测试失败，HRESULT=0x80070005");
}

// 验证默认目录与当前进程镜像路径一致，调用不改变进程工作目录。
// 入参：无运行入参。
// 返回：无返回值；通过 GoogleTest 断言记录路径定位和无副作用结果。
TEST(WindowsUtilTest, executable_directory_matches_process_image)
{
    const std::filesystem::path workingDirectory = std::filesystem::current_path();
    const std::filesystem::path directory = open_st::GetExecutableDirectory();
    ASSERT_FALSE(directory.empty());
    EXPECT_TRUE(directory.is_absolute());
    std::array<wchar_t, 32768> imagePath{};
    DWORD length = static_cast<DWORD>(imagePath.size());
    ASSERT_NE(QueryFullProcessImageNameW(GetCurrentProcess(), 0, imagePath.data(), &length), FALSE);
    const std::filesystem::path expected(std::wstring_view(imagePath.data(), length));
    EXPECT_TRUE(std::filesystem::equivalent(directory, expected.parent_path()));
    EXPECT_EQ(std::filesystem::current_path(), workingDirectory);
}
} // namespace
