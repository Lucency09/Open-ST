// 验证日志分级、轮换、并发写入及限定范围清理的安全边界。

#include <log.h>

#include "logger.h"

#include <gtest/gtest.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
class LoggerTest : public testing::Test
{
  protected:
    // 重置日志单例并准备当前用例专属临时根目录，避免继承旧日志状态。
    // 入参：无显式入参。
    // 返回：无返回值。
    void SetUp() override
    {
        open_st::Logger::Shutdown();
        const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
        this->root_ = std::filesystem::temp_directory_path() /
                      ("open_st_logger_" + std::to_string(GetCurrentProcessId()) + "_" + testInfo->name());
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        ASSERT_FALSE(error);
    }

    // 停止日志写入并清理当前用例的临时目录。
    // 入参：无显式入参。
    // 返回：无返回值。
    void TearDown() override
    {
        open_st::Logger::Shutdown();
        std::error_code error;
        std::filesystem::remove_all(this->root_, error);
        EXPECT_FALSE(error);
    }

    // 枚举并排序测试日志目录中的普通 .log 文件；目录缺失时返回空列表。
    // 入参：无显式入参。
    // 返回：按路径排序的日志文件列表；目录不存在时返回空列表。
    std::vector<std::filesystem::path> LogFiles() const
    {
        std::vector<std::filesystem::path> files;
        const std::filesystem::path directory = this->root_ / "data" / "logs";
        if (!std::filesystem::exists(directory))
        {
            return files;
        }
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".log")
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    // 按二进制读取指定测试文件的全部字节，供内容与清理断言使用。
    // 入参：path 为测试文件路径。
    // 返回：文件的原始字节字符串，供磁盘内容断言使用。
    static std::string ReadFile(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    // 统计换行符数量，核对每条日志是否独占一个物理行。
    // 入参：text 为待统计的日志文本。
    // 返回：text 中换行符的数量。
    static std::size_t CountLines(const std::string& text)
    {
        return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
    }

    // 校验项目日志文件名的毫秒时间戳及可选重名序号格式。
    // 入参：path 为测试文件路径。
    // 返回：文件名符合日志时间戳命名格式时为 true，否则为 false。
    static bool HasTimestampedName(const std::filesystem::path& path)
    {
        const std::regex pattern(
            R"(^Open-ST-[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{3}(-[0-9]{3,})?\.log$)");
        return std::regex_match(path.filename().string(), pattern);
    }

    // 从已确认格式的日志文件名提取时间戳，比较轮换文件的创建时间。
    // 入参：path 为测试文件路径。
    // 返回：文件名中固定位置的毫秒时间戳文本。
    static std::string FilenameTimestamp(const std::filesystem::path& path)
    {
        const std::string name = path.filename().string();
        return name.substr(8, 23);
    }

    std::filesystem::path root_;
};

// 验证默认目录层级、五个级别接口、单行转义，以及 Error 的立即刷新语义。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, writes_levels_to_application_data_directory)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_DEBUG("debug");
    OPEN_ST_LOG_INFO("info with newline\ncontinued");
    OPEN_ST_LOG_WARNING("warning");
    OPEN_ST_LOG_ERROR("error");
    OPEN_ST_LOG_FATAL("fatal");

    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_TRUE(LoggerTest::HasTimestampedName(files.front()));
    const std::string contents = LoggerTest::ReadFile(files.front());
    const std::regex timestampPattern(R"((^|\n)[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} )");
    const std::size_t timestampCount = static_cast<std::size_t>(std::distance(
        std::sregex_iterator(contents.begin(), contents.end(), timestampPattern), std::sregex_iterator()));
#if defined(OPEN_ST_DEBUG_LOGS)
    EXPECT_EQ(timestampCount, 5U);
    EXPECT_NE(contents.find("[DEBUG]"), std::string::npos);
    EXPECT_NE(contents.find(" debug\n"), std::string::npos);
    EXPECT_NE(contents.find("[testing/test/logging/source/logger_tests.cpp:"), std::string::npos);
#else
    EXPECT_EQ(timestampCount, 4U);
    EXPECT_EQ(contents.find("logger_tests.cpp:"), std::string::npos);
#endif
    EXPECT_NE(contents.find("[INFO]"), std::string::npos);
    EXPECT_NE(contents.find(" info with newline\\ncontinued\n"), std::string::npos);
    EXPECT_NE(contents.find("[WARNING]"), std::string::npos);
    EXPECT_NE(contents.find(" warning\n"), std::string::npos);
    EXPECT_NE(contents.find("[ERROR]"), std::string::npos);
    EXPECT_NE(contents.find(" error\n"), std::string::npos);
    EXPECT_NE(contents.find("[FATAL]"), std::string::npos);
    EXPECT_NE(contents.find(" fatal\n"), std::string::npos);
}

// 验证 Debug 参数在 Release 中不求值；Debug 构建则应正常记录并求值一次。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, debug_macro_obeys_build_configuration)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    int evaluationCount = 0;
    OPEN_ST_LOG_DEBUG("evaluation=", ++evaluationCount);
#if defined(OPEN_ST_DEBUG_LOGS)
    EXPECT_EQ(evaluationCount, 1);
#else
    EXPECT_EQ(evaluationCount, 0);
#endif
}

// 验证用较小阈值验证按行轮换；每次轮换重新取毫秒时间戳，并只保留最近三个文件。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, rotates_by_line_count_and_retains_three_files)
{
    open_st::LogOptions options;
    options.maxLines = 2;
    options.maxFileBytes = 4096;
    options.retainedFiles = 3;
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    for (int index = 0; index < 7; ++index)
    {
        if (index > 0 && index % 2 == 0)
        {
            Sleep(5);
        }
        OPEN_ST_LOG_INFO("line ", index);
    }
    open_st::Logger::Shutdown();

    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 3U);
    std::set<std::string> timestamps;
    for (const std::filesystem::path& file : files)
    {
        EXPECT_TRUE(LoggerTest::HasTimestampedName(file));
        timestamps.insert(LoggerTest::FilenameTimestamp(file));
    }
    EXPECT_EQ(timestamps.size(), 3U);
}

// 验证重启后继续追加当天未满文件；已满后按创建下一份文件的当刻生成新时间戳。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, resumes_last_file_after_restart)
{
    open_st::LogOptions options;
    options.maxLines = 3;
    options.maxFileBytes = 4096;
    options.retainedFiles = 3;
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    OPEN_ST_LOG_INFO("first");
    OPEN_ST_LOG_INFO("second");
    open_st::Logger::Shutdown();

    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    OPEN_ST_LOG_INFO("third");
    open_st::Logger::Shutdown();
    std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(LoggerTest::CountLines(LoggerTest::ReadFile(files.front())), 3U);
    const std::filesystem::path firstFile = files.front();

    Sleep(5);
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    OPEN_ST_LOG_INFO("fourth");
    open_st::Logger::Shutdown();
    files = this->LogFiles();
    ASSERT_EQ(files.size(), 2U);
    EXPECT_TRUE(LoggerTest::HasTimestampedName(files[0]));
    EXPECT_TRUE(LoggerTest::HasTimestampedName(files[1]));
    const std::filesystem::path nextFile = files[0] == firstFile ? files[1] : files[0];
    EXPECT_NE(LoggerTest::FilenameTimestamp(firstFile), LoggerTest::FilenameTimestamp(nextFile));
}

// 验证上次进程留下未换行的尾部记录时，重启追加会先补分隔符，不把两条记录粘连。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, separates_an_incomplete_tail_after_restart)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    open_st::Logger::Shutdown();
    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    {
        std::ofstream output(files.front(), std::ios::binary | std::ios::trunc);
        output << "partial";
    }

    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_INFO("complete");
    open_st::Logger::Shutdown();
    const std::string contents = LoggerTest::ReadFile(files.front());
    EXPECT_EQ(contents.find("partial\n"), 0U);
    EXPECT_NE(contents.find("[INFO]"), std::string::npos);
    EXPECT_NE(contents.find(" complete\n"), std::string::npos);
    EXPECT_EQ(LoggerTest::CountLines(contents), 2U);
}

// 验证单条超长消息会受限，且文件达到字节安全上限后下一条记录进入新文件。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, enforces_file_size_safety_limit)
{
    open_st::LogOptions options;
    options.maxLines = 100;
    options.maxFileBytes = 256;
    options.retainedFiles = 3;
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    const std::string largeMessage(600, 'x');
    OPEN_ST_LOG_INFO(largeMessage);
    OPEN_ST_LOG_INFO(largeMessage);
    open_st::Logger::Shutdown();

    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 2U);
    for (const std::filesystem::path& file : files)
    {
        EXPECT_LE(std::filesystem::file_size(file), options.maxFileBytes);
    }
}

// 验证最小文件预算下超长源码路径保留元数据且不突破字节上限。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, bounds_long_source_locations_at_minimum_file_size)
{
    open_st::LogOptions options;
    options.maxFileBytes = 128;
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_, options));
    const std::string source = std::string(512, 'a') + "/file.cpp";
    open_st::Logger::WriteText(open_st::log_detail::LogLevel::Error, std::string(512, 'x'), source, 12345);
    open_st::Logger::Shutdown();
    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    const std::string record = ReadFile(files.front());
    EXPECT_LE(record.size(), options.maxFileBytes);
    EXPECT_EQ(CountLines(record), 1U);
    EXPECT_NE(record.find("[ERROR] [..."), std::string::npos);
    EXPECT_NE(record.find("/file.cpp:12345] "), std::string::npos);
    EXPECT_TRUE(std::regex_search(record, std::regex(R"(^[0-9]{4}-[0-9]{2}-[0-9]{2} )")));
}

// 验证启动时清理旧会话遗留的日志并保留规定数量。
// 入参：无运行入参；测试宏参数用于用例注册。
// 返回：通过断言报告结果。
TEST_F(LoggerTest, cleans_files_left_by_previous_processes)
{
    const std::filesystem::path directory = this->root_ / "data" / "logs";
    std::filesystem::create_directories(directory);
    const std::vector<std::string> oldNames{"Open-ST-2020-01-01.log", "Open-ST-2020-01-02.log",
                                            "Open-ST-2020-01-03.log", "Open-ST-2020-01-04.log"};
    for (const std::string& name : oldNames)
    {
        std::ofstream output(directory / name, std::ios::binary);
        output << "old\n";
    }
    {
        std::ofstream unrelated(directory / "notes.log", std::ios::binary);
        unrelated << "keep\n";
    }

    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    open_st::Logger::Shutdown();
    EXPECT_EQ(this->LogFiles().size(), 4U); // 三个项目日志加一个无关 .log。
    EXPECT_TRUE(std::filesystem::exists(directory / "notes.log"));
    EXPECT_FALSE(std::filesystem::exists(directory / oldNames[0]));
    EXPECT_FALSE(std::filesystem::exists(directory / oldNames[1]));
}

// 验证多线程同步写入不会丢失或拼接记录，每次调用仍对应一条物理日志行。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, serializes_concurrent_writers)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker)
    {
        workers.emplace_back(
            // 各工作线程连续写入五十条带线程编号的记录，验证并发序列化。
            // 入参：无显式入参。
            // 返回：无返回值。
            [worker]()
            {
                for (int line = 0; line < 50; ++line)
                {
                    OPEN_ST_LOG_INFO("worker=", worker, " line=", line);
                }
            });
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }
    open_st::Logger::Shutdown();

    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    EXPECT_EQ(LoggerTest::CountLines(LoggerTest::ReadFile(files.front())), 200U);
}

// 验证目录不可创建时初始化只返回失败，后续各级日志调用仍不会抛异常或阻断调用方。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, directory_failure_is_non_fatal)
{
    const std::filesystem::path blockingPath = this->root_ / "blocking-file";
    std::filesystem::create_directories(this->root_);
    {
        std::ofstream output(blockingPath, std::ios::binary);
        output << "block";
    }
    EXPECT_FALSE(open_st::Logger::Initialize(blockingPath));
    EXPECT_NO_THROW(OPEN_ST_LOG_ERROR("discarded"));
}
// 验证清理只删除命名规则认可的普通日志，保留设置、无关文件和子目录。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, shutdown_and_clear_preserves_unrelated_files)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_ERROR("before cleanup");
    const std::filesystem::path directory = this->root_ / "data" / "logs";
    const std::filesystem::path settings = this->root_ / "data" / "settings.json";
    const std::filesystem::path notes = directory / "notes.log";
    const std::filesystem::path malformed = directory / "Open-ST-not-a-date.log";
    const std::filesystem::path nested = directory / "Open-ST-2020-01-01.log";
    std::ofstream(settings) << "settings";
    std::ofstream(notes) << "notes";
    std::ofstream(malformed) << "unrelated";
    std::filesystem::create_directory(nested);
    std::ofstream(nested / "Open-ST-2020-01-02.log") << "nested";
    std::ofstream(directory / "Open-ST-2020-01-03.log") << "legacy";
    ASSERT_TRUE(open_st::ShutdownAndClearLogging());
    EXPECT_EQ(this->ReadFile(settings), "settings");
    EXPECT_EQ(this->ReadFile(notes), "notes");
    EXPECT_EQ(this->ReadFile(malformed), "unrelated");
    EXPECT_EQ(this->ReadFile(nested / "Open-ST-2020-01-02.log"), "nested");
    EXPECT_FALSE(std::filesystem::exists(directory / "Open-ST-2020-01-03.log"));
    EXPECT_EQ(this->LogFiles().size(), 2U);
    OPEN_ST_LOG_ERROR("must stay stopped");
    EXPECT_EQ(this->LogFiles().size(), 2U);
    EXPECT_TRUE(open_st::ShutdownAndClearLogging());
}
// 验证被其他进程打开且不共享删除的日志导致失败，释放后同一入口可重试。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, failed_cleanup_can_retry_without_resuming_logging)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_ERROR("keep");
    const std::vector<std::filesystem::path> files = this->LogFiles();
    ASSERT_EQ(files.size(), 1U);
    const HANDLE locked = CreateFileW(files.front().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(open_st::Logger::ShutdownAndClear());
    const std::string before = this->ReadFile(files.front());
    OPEN_ST_LOG_ERROR("must not append");
    EXPECT_EQ(this->ReadFile(files.front()), before);
    CloseHandle(locked);
    EXPECT_TRUE(open_st::Logger::ShutdownAndClear());
    EXPECT_TRUE(this->LogFiles().empty());
}
// 验证已识别名称的文件链接不能借清理删除链接或目标，移除链接后可重试。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, cleanup_refuses_symbolic_log_file)
{
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    const std::filesystem::path target = this->root_ / "keep.txt";
    std::ofstream(target) << "preserve";
    const std::filesystem::path link = this->root_ / "data" / "logs" / "Open-ST-2020-01-01.log";
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (error)
        GTEST_SKIP() << "Symbolic links unavailable: " << error.message();
    EXPECT_FALSE(open_st::Logger::ShutdownAndClear());
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    EXPECT_EQ(this->ReadFile(target), "preserve");
    std::filesystem::remove(link);
    EXPECT_TRUE(open_st::Logger::ShutdownAndClear());
}
// 验证日志目录为链接时清理拒绝进入，即使初始化阶段曾使用该路径。
// 入参：无运行入参；测试宏中的套件名和用例名用于 GoogleTest 注册。
// 返回：无返回值；通过 GoogleTest 断言记录验证结果。
TEST_F(LoggerTest, cleanup_refuses_symbolic_log_directory)
{
    const std::filesystem::path target = this->root_ / "elsewhere";
    const std::filesystem::path data = this->root_ / "data";
    std::filesystem::create_directories(target);
    std::filesystem::create_directory(data);
    std::error_code error;
    std::filesystem::create_directory_symlink(target, data / "logs", error);
    if (error)
        GTEST_SKIP() << "Directory symbolic links unavailable: " << error.message();
    ASSERT_TRUE(open_st::Logger::Initialize(this->root_));
    OPEN_ST_LOG_ERROR("preserve target");
    EXPECT_FALSE(open_st::Logger::ShutdownAndClear());
    EXPECT_FALSE(std::filesystem::is_empty(target));
    EXPECT_TRUE(std::filesystem::is_symlink(data / "logs"));
}
} // namespace
