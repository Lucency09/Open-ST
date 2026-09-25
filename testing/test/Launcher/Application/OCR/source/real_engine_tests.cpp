// 使用自有 GDI 文字图验证实际 Tesseract、模型读取与预处理，识别内容不接触用户屏幕。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <gtest/gtest.h>
#include <numeric>
#include <ocr_client.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <windows.h>

namespace
{
using namespace open_st;
using namespace std::chrono_literals;
struct Canvas
{
    HDC dc{};
    HBITMAP bitmap{};
    HFONT font{};
    HGDIOBJ oldBitmap{}, oldFont{};
    // 按 GDI 选择关系先恢复旧对象再释放画布，所有异常路径都安全清理。
    // 入参：无。
    // 返回：无。
    ~Canvas()
    {
        if (this->oldFont)
            SelectObject(this->dc, this->oldFont);
        if (this->oldBitmap)
            SelectObject(this->dc, this->oldBitmap);
        if (this->font)
            DeleteObject(this->font);
        if (this->bitmap)
            DeleteObject(this->bitmap);
        if (this->dc)
            DeleteDC(this->dc);
    }
};
// 渲染已知真值，不用桌面截图或外部授权不明图片作为自动化样例。
// 入参：text 为项目自有文字；fontPixels 为物理像素字高；dark 为深色背景。
// 返回：1200×800 BGRX；资源创建失败明确抛异常，不以空白图蒙混通过。
std::vector<std::byte> Render(std::wstring_view text, int fontPixels, bool dark)
{
    constexpr int WIDTH = 1200;
    constexpr int HEIGHT = 800;
    Canvas canvas;
    canvas.dc = CreateCompatibleDC(nullptr);
    if (!canvas.dc)
        throw std::runtime_error("GDI canvas unavailable");
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = WIDTH;
    info.bmiHeader.biHeight = -HEIGHT;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    canvas.bitmap = CreateDIBSection(canvas.dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    canvas.font =
        CreateFontW(-fontPixels, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    if (!canvas.bitmap || !canvas.font || !pixels)
        throw std::runtime_error("GDI bitmap or font unavailable");
    canvas.oldBitmap = SelectObject(canvas.dc, canvas.bitmap);
    canvas.oldFont = SelectObject(canvas.dc, canvas.font);
    const RECT rectangle{0, 0, WIDTH, HEIGHT};
    if (!FillRect(canvas.dc, &rectangle, static_cast<HBRUSH>(GetStockObject(dark ? BLACK_BRUSH : WHITE_BRUSH))))
        throw std::runtime_error("GDI background failed");
    SetBkMode(canvas.dc, TRANSPARENT);
    SetTextColor(canvas.dc, dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
    RECT textRectangle{35, 35, WIDTH - 35, HEIGHT - 35};
    if (!text.empty() && DrawTextW(canvas.dc, text.data(), static_cast<int>(text.size()), &textRectangle,
                                   DT_LEFT | DT_TOP | DT_NOPREFIX) == 0)
        throw std::runtime_error("GDI text failed");
    GdiFlush();
    std::vector<std::byte> result(static_cast<std::size_t>(WIDTH) * HEIGHT * 4);
    std::memcpy(result.data(), pixels, result.size());
    return result;
}
// 字符错误率仅去除空白，保留标点与大小写，不把近似正文强行算作准确。
// 入参：text 为样例或识别文字。
// 返回：规范化正文。
std::wstring Normalize(std::wstring text)
{
    // 忽略结果的行末排版空白，不忽略识别出的字符。
    // 入参：value 为 UTF-16 单元。
    // 返回：空白时 true。
    text.erase(std::remove_if(text.begin(), text.end(), [](wchar_t value) { return std::iswspace(value) != 0; }),
               text.end());
    return text;
}
// 计算 BMP 样例的标准编辑距离比例，允许平台字体栅格化造成少量差异。
// 入参：truth 为已知真值；actual 为识别结果。
// 返回：字符错误率，空真值另由空白用例直接断言。
double ErrorRate(std::wstring truth, std::wstring actual)
{
    truth = Normalize(std::move(truth));
    actual = Normalize(std::move(actual));
    std::vector<std::size_t> previous(actual.size() + 1), current(actual.size() + 1);
    std::iota(previous.begin(), previous.end(), 0U);
    for (std::size_t row = 1; row <= truth.size(); ++row)
    {
        current[0] = row;
        for (std::size_t column = 1; column <= actual.size(); ++column)
            current[column] = std::min({current[column - 1] + 1, previous[column] + 1,
                                        previous[column - 1] + (truth[row - 1] == actual[column - 1] ? 0 : 1)});
        current.swap(previous);
    }
    return static_cast<double>(previous.back()) / static_cast<double>(truth.size());
}

constexpr std::wstring_view MIXED_TEXT =
    L"屏幕截图文字识别测试\nOpen ST screenshot recognition\n画面の文字を読み取ります";
class OcrRealEngineTest : public testing::TestWithParam<const char*>
{
  protected:
    std::filesystem::path root;
    std::unique_ptr<OcrClient> client;
    // 复制当前档位模型至独占中文空格路径，生产缓存与安装资源保持只读。
    // 入参：无。
    // 返回：无。
    void SetUp() override
    {
        static std::atomic_uint64_t sequence{};
        this->root = std::filesystem::path(OPEN_ST_OCR_TEST_OUTPUT) /
                     (L"真实 引擎 " + std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        ASSERT_FALSE(std::filesystem::exists(this->root));
        const std::filesystem::path target = this->root / L"resources/ocr" / GetParam();
        ASSERT_TRUE(std::filesystem::create_directories(target));
        for (std::string_view language : {"chi_sim", "eng", "jpn"})
        {
            const std::string file = std::string(language) + ".traineddata";
            ASSERT_TRUE(std::filesystem::copy_file(std::filesystem::path(OPEN_ST_OCR_MODEL_CACHE) / GetParam() / file,
                                                   target / file));
        }
        this->client = std::make_unique<OcrClient>(this->root, std::function<void(std::uint64_t)>{});
    }
    // 先结束客户端及其模型线程，再清理测试唯一创建的目录。
    // 入参：无。
    // 返回：无。
    void TearDown() override
    {
        this->client.reset();
        std::error_code error;
        std::filesystem::remove_all(this->root, error);
        EXPECT_FALSE(error);
    }
    // 用实际客户端执行一次识别；只有状态完成才能读取不可变正文。
    // 入参：text 为自有图真值；font 为物理像素；dark 为背景。
    // 返回：完成快照；超过安全测试期限先请求取消再报失败。
    OcrSnapshot Recognize(std::wstring_view text, int font, bool dark = false)
    {
        const std::vector<std::byte> pixels = Render(text, font, dark);
        std::uint64_t id{};
        OcrError error{};
        if (!this->client->Submit({1200, 800, 4800, pixels}, {GetParam(), "chi_sim+eng+jpn"}, id, error))
        {
            ADD_FAILURE() << "Real OCR submission rejected: " << static_cast<int>(error);
            return {};
        }
        const std::chrono::steady_clock::time_point until = std::chrono::steady_clock::now() + 15s;
        while (this->client->Snapshot().busy && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(2ms);
        if (this->client->Snapshot().busy)
        {
            this->client->Cancel();
            ADD_FAILURE() << "Real OCR exceeded test deadline";
        }
        return this->client->Snapshot();
    }
};

// 验证 fast/best 实际混合模型同时读取三种语言，热缓存不改变正文准确性。
// 入参：无。
// 返回：无；清晰样例要求 CER 不超过 5%，不是对任意截图的准确率承诺。
TEST_P(OcrRealEngineTest, clear_mixed_text_and_warm_cache)
{
    for (int pass = 0; pass < 2; ++pass)
    {
        const OcrSnapshot result = this->Recognize(MIXED_TEXT, 38);
        ASSERT_EQ(result.error, OcrError::None);
        ASSERT_EQ(result.phase, OcrPhase::Succeeded);
        ASSERT_TRUE(result.text);
        EXPECT_LE(ErrorRate(std::wstring(MIXED_TEXT), *result.text), 0.05);
    }
}
// 验证两倍预处理防止 18 像素小字退化为空白，允许字体栅格化导致少量误识别。
// 入参：无。
// 返回：无；小字样例 CER 不超过 10%，不要求所有复杂图完全准确。
TEST_P(OcrRealEngineTest, small_text_survives_preprocessing)
{
    const OcrSnapshot result = this->Recognize(MIXED_TEXT, 18);
    ASSERT_EQ(result.error, OcrError::None);
    ASSERT_EQ(result.phase, OcrPhase::Succeeded);
    ASSERT_TRUE(result.text);
    EXPECT_LE(ErrorRate(std::wstring(MIXED_TEXT), *result.text), 0.10);
}
// 验证插值没有把深色背景文字变为空白或引擎错误。
// 入参：无。
// 返回：无；样例 CER 不超过 10%。
TEST_P(OcrRealEngineTest, dark_background_remains_readable)
{
    const OcrSnapshot result = this->Recognize(MIXED_TEXT, 38, true);
    ASSERT_EQ(result.error, OcrError::None);
    ASSERT_EQ(result.phase, OcrPhase::Succeeded);
    ASSERT_TRUE(result.text);
    EXPECT_LE(ErrorRate(std::wstring(MIXED_TEXT), *result.text), 0.10);
}
// 验证实际空白图返回 Empty，不误报失败或发布换行占位正文。
// 入参：无。
// 返回：无。
TEST_P(OcrRealEngineTest, blank_is_empty)
{
    const OcrSnapshot result = this->Recognize(L"", 38);
    EXPECT_EQ(result.error, OcrError::None);
    EXPECT_EQ(result.phase, OcrPhase::Empty);
    EXPECT_FALSE(result.text);
}
INSTANTIATE_TEST_SUITE_P(Models, OcrRealEngineTest, testing::Values("fast", "best"));
} // namespace
