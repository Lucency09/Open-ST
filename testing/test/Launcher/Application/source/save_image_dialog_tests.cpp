// 验证保存文件名规范和原窗口二次确认规则，不打开系统对话框或覆盖用户文件。

#include "save_image_dialog_events.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace open_st
{
namespace
{
// 验证所选类型主导后缀，大小写有效后缀保留，多点主体和目录不丢失。
// 入参：无；使用独立路径样例。
// 返回：无；通过断言报告纯路径规范化结果。
TEST(SaveImageDialogTest, normalizes_only_conflicting_final_extension)
{
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/图片.png", ImageFileFormat::Jpeg),
              std::filesystem::path(L"C:/shots/图片.jpg"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/图片.jpg", ImageFileFormat::Png),
              std::filesystem::path(L"C:/shots/图片.png"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/name.JpEg", ImageFileFormat::Jpeg),
              std::filesystem::path(L"C:/shots/name.JpEg"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/name.JPG", ImageFileFormat::Jpeg),
              std::filesystem::path(L"C:/shots/name.JPG"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/name.PNG", ImageFileFormat::Png),
              std::filesystem::path(L"C:/shots/name.PNG"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/name", ImageFileFormat::Jpeg),
              std::filesystem::path(L"C:/shots/name.jpg"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/name", ImageFileFormat::Png),
              std::filesystem::path(L"C:/shots/name.png"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/a.b.123.unknown", ImageFileFormat::Jpeg),
              std::filesystem::path(L"C:/shots/a.b.123.jpg"));
    EXPECT_EQ(NormalizeSaveImagePath(L"C:/shots/a.b.", ImageFileFormat::Png),
              std::filesystem::path(L"C:/shots/a.b.png"));
    EXPECT_THROW((void)NormalizeSaveImagePath(L"file.png", static_cast<ImageFileFormat>(99)), std::invalid_argument);
    EXPECT_STREQ(SaveImageDefaultExtension(ImageFileFormat::Jpeg), L"jpg");
    EXPECT_STREQ(SaveImageDefaultExtension(ImageFileFormat::Png), L"png");
    EXPECT_STREQ(SaveImageDefaultExtension(static_cast<ImageFileFormat>(99)), L"");
}

// 验证后缀修正只能更新原窗口并返回 S_FALSE，再次确认正确目标才能 S_OK。
// 入参：无；替身记录文件名更新和原因提示，不向真实系统发起保存。
// 返回：无；通过调用顺序和 HRESULT 验证不会复用旧路径覆盖许可。
TEST(SaveImageDialogTest, correction_requires_another_confirmation_of_final_path)
{
    SaveImageTarget target{L"C:/shots/a.b.png", ImageFileFormat::Jpeg};
    std::vector<std::wstring> calls;
    // 模拟原窗口文件名更新，保留下一轮确认应读取的最终候选。
    // 入参：filename：完整修正路径。
    // 返回：S_OK 表示系统接受新的编辑内容，不代表已确认保存。
    const std::function<HRESULT(const std::wstring&)> setFilename = [&target, &calls](const std::wstring& filename)
    {
        calls.push_back(filename);
        target.path = filename;
        return S_OK;
    };
    // 记录修正提示发生于文件名更新之后。
    // 入参：extension：带点的标准后缀。
    // 返回：无。
    const std::function<void(const std::wstring&)> notify = [&calls](const std::wstring& extension)
    { calls.push_back(extension); };
    EXPECT_EQ(CheckSaveImageFileOk(target, setFilename, notify), S_FALSE);
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0], L"C:/shots/a.b.jpg");
    EXPECT_EQ(calls[1], L".jpg");
    EXPECT_EQ(CheckSaveImageFileOk(target, setFilename, notify), S_OK);
    EXPECT_EQ(calls.size(), 2U);
}

// 验证系统拒绝文件名更新或输入格式错误时不能显示修正成功，也不能接受旧目标。
// 入参：无；模拟设置文件名失败以及异常回调。
// 返回：无；失败 HRESULT 保持拒绝确认语义且不通知成功。
TEST(SaveImageDialogTest, correction_failure_does_not_accept_or_report_success)
{
    const SaveImageTarget target{L"C:/shots/existing.png", ImageFileFormat::Jpeg};
    bool notified{};
    // 模拟系统文件名更新失败。
    // 入参：filename：待更新路径。
    // 返回：E_ACCESSDENIED。
    const std::function<HRESULT(const std::wstring&)> reject = [](const std::wstring&) { return E_ACCESSDENIED; };
    // 检测不应发生的修正成功提示。
    // 入参：extension：拟提示的后缀。
    // 返回：无。
    const std::function<void(const std::wstring&)> notify = [&notified](const std::wstring&) { notified = true; };
    EXPECT_EQ(CheckSaveImageFileOk(target, reject, notify), E_ACCESSDENIED);
    EXPECT_FALSE(notified);
    EXPECT_TRUE(FAILED(CheckSaveImageFileOk({target.path, static_cast<ImageFileFormat>(99)}, reject, notify)));
    EXPECT_FALSE(notified);
    // 模拟窗口边界回调异常，禁止异常穿过 COM ABI。
    // 入参：filename：候选修正路径。
    // 返回：总是抛出异常，由被测确认逻辑转换成 E_FAIL。
    const std::function<HRESULT(const std::wstring&)> fail = [](const std::wstring&) -> HRESULT
    { throw std::runtime_error("injected"); };
    EXPECT_EQ(CheckSaveImageFileOk(target, fail, notify), E_FAIL);
    EXPECT_FALSE(notified);
}
// 验证关闭系统窗口失败后仍拒绝确认，后续成功校验不清除首次事件错误。
// 入参：无；关闭替身始终失败，记录所有关闭时传入的错误码。
// 返回：无；与真实事件同用状态边界，确认 S_FALSE 与 Show 结果优先级。
TEST(SaveImageDialogTest, event_failure_rejects_even_when_close_fails_and_show_succeeds)
{
    SaveImageDialogEventState state;
    std::vector<HRESULT> closeErrors;
    // 模拟 Close 自身返回失败，不能使事件错误变成用户确认。
    // 入参：error：事件状态传入的首个错误。
    // 返回：E_UNEXPECTED，表示窗口未能关闭。
    const auto close = [&closeErrors](HRESULT error)
    {
        closeErrors.push_back(error);
        return E_UNEXPECTED;
    };
    EXPECT_EQ(state.CheckFileOk(E_ACCESSDENIED, close), S_FALSE);
    EXPECT_EQ(state.CheckFileOk(S_OK, close), S_FALSE);
    EXPECT_EQ(state.CheckFileOk(E_OUTOFMEMORY, close), S_FALSE);
    EXPECT_EQ(closeErrors, (std::vector<HRESULT>{E_ACCESSDENIED, E_ACCESSDENIED, E_ACCESSDENIED}));
    EXPECT_EQ(state.ShowResult(S_OK), E_ACCESSDENIED);
    EXPECT_EQ(state.CompleteShow(S_OK), SaveChoice::Failed);
    EXPECT_EQ(state.CompleteShow(HRESULT_FROM_WIN32(ERROR_CANCELLED)), SaveChoice::Failed);
    EXPECT_EQ(state.CompleteShow(E_UNEXPECTED), SaveChoice::Failed);
}

// 验证正常修正与取消保持原语义，错误码恰为取消或关闭异常也仍属于事件失败。
// 入参：无；成功关闭和抛异常的替身覆盖 COM 边界故障。
// 返回：无；使用产品最终 CompleteShow 边界验证不会把事件错误误报为取消。
TEST(SaveImageDialogTest, final_choice_distinguishes_cancel_from_event_error)
{
    SaveImageDialogEventState state;
    int closeCount{};
    // 记录正常修正不应触发关闭，失败时模拟系统接受关闭请求。
    // 入参：error：拟用于关闭的事件错误。
    // 返回：S_OK。
    const auto close = [&closeCount](HRESULT)
    {
        ++closeCount;
        return S_OK;
    };
    EXPECT_EQ(state.CheckFileOk(S_FALSE, close), S_FALSE);
    EXPECT_EQ(state.CheckFileOk(S_OK, close), S_OK);
    EXPECT_EQ(closeCount, 0);
    EXPECT_EQ(state.CompleteShow(S_OK), SaveChoice::Accepted);
    EXPECT_EQ(state.CompleteShow(HRESULT_FROM_WIN32(ERROR_CANCELLED)), SaveChoice::Cancelled);
    EXPECT_EQ(state.CompleteShow(E_FAIL), SaveChoice::Failed);
    EXPECT_EQ(state.CheckFileOk(HRESULT_FROM_WIN32(ERROR_CANCELLED), close), S_FALSE);
    EXPECT_EQ(closeCount, 1);
    EXPECT_EQ(state.CompleteShow(HRESULT_FROM_WIN32(ERROR_CANCELLED)), SaveChoice::Failed);
    // 模拟异常关闭边界，不能抹除已保存的首个失败。
    // 入参：error：先前保存的事件错误。
    // 返回：总是抛异常，由状态守卫截留。
    const auto throwingClose = [](HRESULT) -> HRESULT { throw std::runtime_error("close failed"); };
    EXPECT_EQ(state.CheckFileOk(S_OK, throwingClose), S_FALSE);
    EXPECT_EQ(state.CompleteShow(S_OK), SaveChoice::Failed);
}
} // namespace
} // namespace open_st
