// 验证通用 SHA-256 与遗留 MD5 标准向量、分块边界及原始字节语义；不读写业务文件。
#include <gtest/gtest.h>
#include <sha256.h>
#include <string>
#include <string_view>
#include <vector>

namespace
{
// 仅用于断言标准向量，把原始摘要按字节展开为小写十六进制。
// 入参：digest 为借用摘要字节。
// 返回：每字节两个字符的小写字符串。
std::string Hex(std::span<const std::byte> digest)
{
    constexpr std::string_view HEX = "0123456789abcdef";
    std::string result;
    for (std::byte value : digest)
    {
        const unsigned int number = std::to_integer<unsigned int>(value);
        result.push_back(HEX[number >> 4]);
        result.push_back(HEX[number & 15]);
    }
    return result;
}
// 验证空数据的标准摘要，空片段不改变状态也不提前结束。
// 入参：无。
// 返回：无。
TEST(Sha256Test, empty_standard_vector)
{
    open_st::Sha256 hash;
    ASSERT_TRUE(hash.IsValid());
    ASSERT_TRUE(hash.Append({}));
    std::array<std::byte, 32> digest{};
    ASSERT_TRUE(hash.Finish(digest));
    EXPECT_EQ(Hex(digest), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}
// 验证 abc 单块和逐字节分块结果一致，覆盖增量调用之间的状态保留。
// 入参：无。
// 返回：无。
TEST(Sha256Test, split_abc_matches_standard_vector)
{
    constexpr std::string_view TEXT = "abc";
    for (std::size_t block = 1; block <= 3; ++block)
    {
        open_st::Sha256 hash;
        ASSERT_TRUE(hash.IsValid());
        for (std::size_t index = 0; index < TEXT.size(); index += block)
        {
            const std::string_view chunk = TEXT.substr(index, block);
            ASSERT_TRUE(hash.Append(std::as_bytes(std::span(chunk.data(), chunk.size()))));
        }
        std::array<std::byte, 32> digest{};
        ASSERT_TRUE(hash.Finish(digest));
        EXPECT_EQ(Hex(digest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}
// 验证大量增量数据的标准向量，不把摘要实现限于短字符串。
// 入参：无。
// 返回：无。
TEST(Sha256Test, million_a_standard_vector)
{
    open_st::Sha256 hash;
    const std::vector<std::byte> chunk(1000, std::byte{'a'});
    for (int index = 0; index < 1000; ++index)
        ASSERT_TRUE(hash.Append(chunk));
    std::array<std::byte, 32> digest{};
    ASSERT_TRUE(hash.Finish(digest));
    EXPECT_EQ(Hex(digest), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
// 验证完成封闭状态，重复完成和追加均被拒绝且不覆盖调用方结果。
// 入参：无。
// 返回：无。
TEST(Sha256Test, completion_is_single_use_and_failure_preserves_output)
{
    open_st::Sha256 hash;
    std::array<std::byte, 32> digest{};
    ASSERT_TRUE(hash.Finish(digest));
    EXPECT_FALSE(hash.IsValid());
    EXPECT_FALSE(hash.Append({}));
    digest.fill(std::byte{0xA5});
    const std::array<std::byte, 32> original = digest;
    EXPECT_FALSE(hash.Finish(digest));
    EXPECT_EQ(digest, original);
}
// 验证 RFC 1321 的空输入和 abc 标准向量，覆盖空 span 及单次散列长度。
// 入参：无。
// 返回：无。
TEST(Md5Test, empty_and_abc_standard_vectors)
{
    std::array<std::byte, 16> digest{};
    ASSERT_TRUE(open_st::ComputeMd5({}, digest));
    EXPECT_EQ(Hex(digest), "d41d8cd98f00b204e9800998ecf8427e");
    constexpr std::string_view TEXT = "abc";
    ASSERT_TRUE(open_st::ComputeMd5(std::as_bytes(std::span(TEXT.data(), TEXT.size())), digest));
    EXPECT_EQ(Hex(digest), "900150983cd24fb0d6963f7d28e17f72");
}
// 验证 UTF-8 原始字节包含中文、表单特殊字符和换行，不进行编码或标准化。
// 入参：无。
// 返回：无，固定摘要由独立 hashlib 实现交叉核对。
TEST(Md5Test, unicode_raw_utf8_bytes)
{
    constexpr std::string_view TEXT = "你好&+%\n";
    std::array<std::byte, 16> digest{};
    ASSERT_TRUE(open_st::ComputeMd5(std::as_bytes(std::span(TEXT.data(), TEXT.size())), digest));
    EXPECT_EQ(Hex(digest), "d98ba23d9414f63d0279f5c4a5b09715");
}
} // namespace
