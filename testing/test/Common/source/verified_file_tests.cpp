// 验证公共固定文件读取的原子交付、取消和同一身份边界，不访问产品资源或网络。
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <verified_file.h>
#include <windows.h>

namespace
{
using namespace open_st;
constexpr std::string_view ABC_SHA = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
class VerifiedFileTest : public testing::Test
{
  protected:
    std::filesystem::path path;
    // 写入当前测试专属已知文件，文件名避免与其他进程冲突。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        this->path = std::filesystem::path(OPEN_ST_VERIFIED_TEST_OUTPUT) /
                     (L"验证 字节 " + std::to_wstring(GetCurrentProcessId()) + L".bin");
        std::filesystem::create_directories(this->path.parent_path());
        std::ofstream stream(this->path, std::ios::binary);
        stream << "abc";
        ASSERT_TRUE(stream);
    }
    // 只清除本用例独占的文件。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        std::filesystem::remove(this->path);
    }
};
// 验证中文路径和固定字节读取成功，随后替换磁盘文件不会改变已交付内存。
// 入参：无。
// 返回：无。
TEST_F(VerifiedFileTest, verified_bytes_survive_path_replacement)
{
    std::string bytes;
    ASSERT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, {}, bytes), VerifiedFileError::None);
    ASSERT_EQ(bytes, "abc");
    std::filesystem::remove(this->path);
    std::ofstream(this->path, std::ios::binary) << "bad";
    EXPECT_EQ(bytes, "abc");
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, {}, bytes), VerifiedFileError::Integrity);
    EXPECT_TRUE(bytes.empty());
}
// 验证长度、摘要、缺失错误不交付旧输出或部分内容。
// 入参：无。
// 返回：无。
TEST_F(VerifiedFileTest, mismatch_and_missing_clear_output)
{
    std::string bytes = "stale";
    EXPECT_EQ(ReadVerifiedFile(this->path, 2, ABC_SHA, {}, bytes), VerifiedFileError::Integrity);
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, std::string(64, '0'), {}, bytes), VerifiedFileError::Integrity);
    std::filesystem::remove(this->path);
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, {}, bytes), VerifiedFileError::Missing);
}
// 验证取消在打开之前生效，且哈希完成后的取消也不能泄漏结果。
// 入参：无。
// 返回：无。
TEST_F(VerifiedFileTest, cancellation_prevents_publication)
{
    std::string bytes = "stale";
    // 模拟已取消状态。
    // 入参：无。
    // 返回：总是停止。
    const auto stopped = [] { return true; };
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, stopped, bytes), VerifiedFileError::Cancelled);
    EXPECT_TRUE(bytes.empty());
    unsigned int calls = 0;
    // 在读取与散列之后才取消，验证最终发布屏障。
    // 入参：无。
    // 返回：第三次查询时停止。
    const auto lateStop = [&calls] { return ++calls >= 3; };
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, lateStop, bytes), VerifiedFileError::Cancelled);
    EXPECT_TRUE(bytes.empty());
}
// 验证验证期间持有拒写句柄，不能在读取后散列前换掉同名内容。
// 入参：无。
// 返回：无。
TEST_F(VerifiedFileTest, held_handle_denies_writes_until_completion)
{
    std::string bytes;
    unsigned int calls = 0;
    // 文件打开后尝试写入，立即关闭任何意外成功句柄以便报告真实失败。
    // 入参：无。
    // 返回：不取消。
    const auto check = [this, &calls]
    {
        if (++calls == 2)
        {
            const HANDLE writer =
                CreateFileW(this->path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
            EXPECT_EQ(writer, INVALID_HANDLE_VALUE);
            if (writer != INVALID_HANDLE_VALUE)
                CloseHandle(writer);
        }
        return false;
    };
    EXPECT_EQ(ReadVerifiedFile(this->path, 3, ABC_SHA, check, bytes), VerifiedFileError::None);
    EXPECT_EQ(bytes, "abc");
}
} // namespace
