// 验证模型固定哈希、中文空格路径及取消边界；测试仅在专属输出目录复制公开模型。
#include "ocr_models.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <windows.h>

namespace
{
using namespace open_st;
class OcrModelTest : public testing::Test
{
  protected:
    std::filesystem::path root;
    std::atomic_bool cancel{false};
    // 为每次测试建立独占中文空格目录，不使用产品 data 或全局临时目录。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        static std::atomic_uint64_t sequence{};
        this->root = std::filesystem::path(OPEN_ST_OCR_TEST_OUTPUT) /
                     (L"中文 模型 " + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        ASSERT_FALSE(std::filesystem::exists(this->root));
        ASSERT_TRUE(std::filesystem::create_directories(this->root));
    }
    // 仅清理当前测试创建的专属目录，保留原模型缓存。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(this->root, error);
        EXPECT_FALSE(error);
    }
    // 从构建已校验缓存复制一份模型以便受控修改，不接触正式资源。
    // 入参：family 为档位；language 为模型语言。
    // 返回：复制后的目标路径。
    std::filesystem::path Copy(std::string_view family, std::string_view language)
    {
        const std::filesystem::path relative = std::filesystem::path(family) / (std::string(language) + ".traineddata");
        const std::filesystem::path destination = this->root / L"resources/ocr" / relative;
        std::filesystem::create_directories(destination.parent_path());
        std::filesystem::copy_file(std::filesystem::path(OPEN_ST_OCR_MODEL_CACHE) / relative, destination);
        return destination;
    }
};

// 验证六份模型在中文空格路径全部通过固定哈希，混合一次加载三份且单语只加载一份。
// 入参：无。
// 返回：无。
TEST_F(OcrModelTest, six_verified_models_support_unicode_paths_and_choices)
{
    for (std::string_view family : OcrModelChoices())
    {
        for (std::string_view language : {"chi_sim", "eng", "jpn"})
            this->Copy(family, language);
        for (std::string_view language : OcrLanguageChoices())
        {
            std::vector<ocr_detail::ModelBytes> models;
            ASSERT_EQ(
                ocr_detail::LoadModels(this->root, {std::string(family), std::string(language)}, this->cancel, models),
                OcrError::None);
            EXPECT_EQ(models.size(), language == "chi_sim+eng+jpn" ? 3U : 1U);
            for (const ocr_detail::ModelBytes& model : models)
                EXPECT_FALSE(model.bytes.empty());
        }
    }
}
// 验证相同长度内容篡改不会绕过模型完整性，不把损坏模型返回为部分候选。
// 入参：无。
// 返回：无。
TEST_F(OcrModelTest, changed_bytes_fail_compiled_hash)
{
    const std::filesystem::path file = this->Copy("fast", "eng");
    {
        std::fstream stream(file, std::ios::in | std::ios::out | std::ios::binary);
        ASSERT_TRUE(stream);
        char value{};
        stream.read(&value, 1);
        value ^= 0x01;
        stream.seekp(0);
        stream.write(&value, 1);
        ASSERT_TRUE(stream);
    }
    std::vector<ocr_detail::ModelBytes> models;
    EXPECT_EQ(ocr_detail::LoadModels(this->root, {"fast", "eng"}, this->cancel, models), OcrError::ModelIntegrity);
    EXPECT_TRUE(models.empty());
}
// 验证文件大小与缺失有不同分类，不能回退环境变量目录或其他档位。
// 入参：无。
// 返回：无。
TEST_F(OcrModelTest, missing_and_truncated_files_are_reported)
{
    std::vector<ocr_detail::ModelBytes> models;
    EXPECT_EQ(ocr_detail::LoadModels(this->root, {"fast", "eng"}, this->cancel, models), OcrError::ModelMissing);
    const std::filesystem::path file = this->Copy("fast", "eng");
    std::filesystem::resize_file(file, 3);
    EXPECT_EQ(ocr_detail::LoadModels(this->root, {"fast", "eng"}, this->cancel, models), OcrError::ModelIntegrity);
    EXPECT_TRUE(models.empty());
}
// 验证取消先于读盘，模型不存在也不把已取消任务变成缺失错误。
// 入参：无。
// 返回：无。
TEST_F(OcrModelTest, cancellation_prevents_model_read)
{
    this->cancel.store(true);
    std::vector<ocr_detail::ModelBytes> models;
    EXPECT_EQ(ocr_detail::LoadModels(this->root, {}, this->cancel, models), OcrError::Cancelled);
    EXPECT_TRUE(models.empty());
}
// 验证已验证字节独立于路径后续变化，加载不能重新打开同名替代文件。
// 入参：无。
// 返回：无。
TEST_F(OcrModelTest, verified_memory_survives_file_replacement)
{
    const std::filesystem::path file = this->Copy("fast", "eng");
    std::vector<ocr_detail::ModelBytes> models;
    ASSERT_EQ(ocr_detail::LoadModels(this->root, {"fast", "eng"}, this->cancel, models), OcrError::None);
    ASSERT_EQ(models.size(), 1U);
    const std::size_t size = models.front().bytes.size();
    const char first = models.front().bytes.front();
    std::filesystem::remove(file);
    {
        std::ofstream replacement(file, std::ios::binary);
        replacement << "untrusted replacement";
    }
    EXPECT_EQ(models.front().bytes.size(), size);
    EXPECT_EQ(models.front().bytes.front(), first);
    std::vector<ocr_detail::ModelBytes> reloaded;
    EXPECT_EQ(ocr_detail::LoadModels(this->root, {"fast", "eng"}, this->cancel, reloaded), OcrError::ModelIntegrity);
}
} // namespace
