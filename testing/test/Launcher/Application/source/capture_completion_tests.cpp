// 验证截图完成编排的转换输出顺序、忙状态门禁与失败恢复。

#include "capture_completion.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace open_st
{
namespace
{
// 记录业务顺序并按测试输入模拟边界，不创建窗口、剪贴板或真实文件。
struct CompletionProbe final
{
    std::vector<std::string> calls;
    bool generateResult{true};
    bool copyResult{true};
    bool saveResult{true};
    bool rememberResult{true};
    SaveChoice choice{SaveChoice::Accepted};

    // 构造用于验证截图完成调用顺序的探针回调集合。
    // 入参：无显式入参。
    // 返回：绑定当前探针的完成动作集合；回调借用探针，不能比探针存活更久。
    CompletionActions Actions()
    {
        CompletionActions actions;
        // 记录转换步骤及其结果。
        // 入参：无显式入参。
        // 返回：generateResult，表示模拟图像转换是否成功。
        actions.generate = [this]()
        {
            this->calls.emplace_back("generate");
            return this->generateResult;
        };
        // 记录剪贴板步骤及其结果。
        // 入参：无显式入参。
        // 返回：copyResult，表示模拟剪贴板写入是否成功。
        actions.copy = [this]()
        {
            this->calls.emplace_back("copy");
            return this->copyResult;
        };
        // 记录保存路径选择，不生成像素。
        // 入参：无显式入参。
        // 返回：choice，表示模拟保存路径选择的接受、取消或失败结果。
        actions.chooseSave = [this]()
        {
            this->calls.emplace_back("choose");
            return this->choice;
        };
        // 记录编码文件保存步骤。
        // 入参：无显式入参。
        // 返回：saveResult，表示模拟编码文件保存是否成功。
        actions.save = [this]()
        {
            this->calls.emplace_back("save");
            return this->saveResult;
        };
        // 记录保存成功后的目录持久化。
        // 入参：无显式入参。
        // 返回：rememberResult，表示模拟保存目录持久化是否成功。
        actions.rememberDirectory = [this]()
        {
            this->calls.emplace_back("remember");
            return this->rememberResult;
        };
        return actions;
    }
};

// 验证未稳定的选区不执行任何回调，忙状态保持可用。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, not_ready_ignores_all_actions)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    EXPECT_EQ(completion.CopySelection(false, probe.Actions()), CompletionResult::Ignored);
    EXPECT_EQ(completion.SaveSelection(false, probe.Actions()), CompletionResult::Ignored);
    EXPECT_TRUE(probe.calls.empty());
    EXPECT_FALSE(completion.IsBusy());
}

// 验证模态保存回调中复制及保存重入均被拒绝，不重复执行转换或系统操作。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, busy_blocks_copy_and_save_reentry)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    CompletionActions actions = probe.Actions();
    // 在模态路径选择期间模拟消息重入。
    // 入参：无显式入参。
    // 返回：SaveChoice::Accepted，允许外层保存继续。
    actions.chooseSave = [&completion, &probe]()
    {
        EXPECT_TRUE(completion.IsBusy());
        EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::Ignored);
        EXPECT_EQ(completion.SaveSelection(true, probe.Actions()), CompletionResult::Ignored);
        return SaveChoice::Accepted;
    };
    EXPECT_EQ(completion.SaveSelection(true, actions), CompletionResult::Saved);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"generate", "save", "remember"}));
    EXPECT_FALSE(completion.IsBusy());
}

// 验证取消或路径对话框失败只运行选择步骤，不生成图像或改动目标。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, cancelled_or_failed_dialog_does_not_generate)
{
    for (const SaveChoice choice : {SaveChoice::Cancelled, SaveChoice::Failed})
    {
        CaptureCompletion completion;
        CompletionProbe probe;
        probe.choice = choice;
        EXPECT_EQ(completion.SaveSelection(true, probe.Actions()),
                  choice == SaveChoice::Cancelled ? CompletionResult::Cancelled : CompletionResult::SaveFailed);
        EXPECT_EQ(probe.calls, (std::vector<std::string>{"choose"}));
        EXPECT_FALSE(completion.IsBusy());
    }
}

// 验证转换失败阻止复制与保存，后续重试仍可完成复制。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, conversion_failure_stops_side_effects_and_allows_retry)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    probe.generateResult = false;
    EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::ConversionFailed);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"generate"}));
    probe.calls.clear();
    EXPECT_EQ(completion.SaveSelection(true, probe.Actions()), CompletionResult::ConversionFailed);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"choose", "generate"}));
    probe.generateResult = true;
    EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::Copied);
    EXPECT_FALSE(completion.IsBusy());
}

// 验证保存失败不记录目录，而目录失败只产生已保存的告警结果。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, saved_file_survives_directory_failure)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    probe.saveResult = false;
    EXPECT_EQ(completion.SaveSelection(true, probe.Actions()), CompletionResult::SaveFailed);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"choose", "generate", "save"}));
    probe.calls.clear();
    probe.saveResult = true;
    probe.rememberResult = false;
    EXPECT_EQ(completion.SaveSelection(true, probe.Actions()), CompletionResult::SavedDirectoryWarning);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"choose", "generate", "save", "remember"}));
    EXPECT_FALSE(completion.IsBusy());
}

// 验证剪贴板失败是可重试结果，不被误报为已复制。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, copy_failure_allows_retry)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    probe.copyResult = false;
    EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::CopyFailed);
    EXPECT_FALSE(completion.IsBusy());
    probe.copyResult = true;
    EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::Copied);
}

// 验证文件已保存后目录回调抛异常仍返回保存成功附带告警，不传播为保存失败。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, directory_exception_preserves_saved_result)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    CompletionActions actions = probe.Actions();
    // 模拟目录持久化的异常边界。
    // 入参：无显式入参。
    // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
    actions.rememberDirectory = []() -> bool { throw std::runtime_error("directory"); };
    EXPECT_EQ(completion.SaveSelection(true, actions), CompletionResult::SavedDirectoryWarning);
    EXPECT_EQ(probe.calls, (std::vector<std::string>{"choose", "generate", "save"}));
    EXPECT_FALSE(completion.IsBusy());
}

// 验证选择对话框和转换抛异常时 RAII 均恢复忙状态，异常保持可被上层捕获。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST(CaptureCompletionTest, exceptions_restore_busy_state)
{
    CaptureCompletion completion;
    CompletionProbe probe;
    CompletionActions actions = probe.Actions();
    // 模拟对话框边界抛出异常。
    // 入参：无显式入参。
    // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
    actions.chooseSave = []() -> SaveChoice { throw std::runtime_error("dialog"); };
    EXPECT_THROW((void)completion.SaveSelection(true, actions), std::runtime_error);
    EXPECT_FALSE(completion.IsBusy());
    // 模拟图像转换抛出异常。
    // 入参：无显式入参。
    // 返回：不正常返回；主动抛出测试异常，交由被测边界处理。
    actions.generate = []() -> bool { throw std::runtime_error("conversion"); };
    EXPECT_THROW((void)completion.CopySelection(true, actions), std::runtime_error);
    EXPECT_FALSE(completion.IsBusy());
    EXPECT_EQ(completion.CopySelection(true, probe.Actions()), CompletionResult::Copied);
}
} // namespace
} // namespace open_st
