// 声明应用协调器及其资源所有权，连接截图、设置、系统集成和窗口消息流程。

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>

namespace open_st
{
class FrozenDesktopFrame;
class CaptureOverlaySession;
class SelectionModel;
class WindowSelectionSnapshot;
class CaptureSelectionInput;
struct SelectionSnapshot;
struct PointI;
class SettingsWindow;
class SelectionOutputRenderer;
class CaptureCompletion;
class CaptureToolbar;
class CaptureCommandGate;
class PinWindowManager;
struct PinWindowCallbacks;
enum class PinCommand : std::uint32_t;
enum class CaptureToolbarCommand : std::uint32_t;
class SingleInstance;
class StartupRegistration;
class WelcomeWindow;
class WindowRenderer;
class LogMaintenanceTask;
struct SettingsMaintenanceStatus;
class HotkeyManager;
struct SettingsWindowCallbacks;

// 进程级应用协调器：拥有隐藏消息窗口、托盘、全局热键和当前截图会话。
// 捕获与呈现的具体实现由独立模块完成，App 只负责按 Win32 消息驱动它们的生命周期。
class App final
{
  public:
    // 创建应用协调器并保存进程模块实例。
    // 入参：instance：借用的当前程序模块句柄。
    // 返回：构造函数无返回值；窗口、设置及捕获资源留待运行时初始化。
    explicit App(HINSTANCE instance) noexcept;
    // 禁止复制构造，确保应用协调器及其系统资源只由原对象管理。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    App(const App&) = delete;
    // 禁止复制赋值，避免应用协调器及其系统资源出现多个所有者。
    // 入参：未命名的同类型 const 引用：拟复制的源对象。
    // 返回：函数已删除，调用会导致编译错误，无运行时返回结果。
    App& operator=(const App&) = delete;
    // 关闭应用持有的业务窗口和进程级服务并释放系统资源。
    // 入参：无。
    // 返回：析构函数无返回值；停止实例监听，关闭设置和截图会话，移除托盘并释放图标、消息窗口、COM 和日志。
    ~App();

    // 把当前稳定截图选区复制到系统剪贴板。
    // 入参：无。
    // 返回：无返回值；经统一完成流程处理，不允许输出时忽略，失败显示本地化提示。
    void CopySelection();
    // 让用户选择文件目标并保存当前稳定截图选区。
    // 入参：无。
    // 返回：无返回值；经统一完成流程处理，取消或失败保留仍有效的截图会话。
    void SaveSelection();

    // 初始化应用服务并运行主消息循环直到退出。
    // 入参：showCommand（定义中未命名的 int）：Win32 初始显示方式，当前托盘应用不使用该值。
    // 返回：正常退出返回 WM_QUIT 的退出码；初始化或消息循环失败返回 1；成功转发第二实例请求返回 0。
    int Run(int showCommand);

  private:
    friend struct AppToolbarTestAccess; // 测试仅替换截图状态，消息投递与分派使用真实 App。
    friend struct AppPinTestAccess;
    friend struct AppHotkeyTestAccess;
    friend struct AppSelectionTestAccess;
    struct PinOperation;
    // 集中组装贴图文本查询和异步业务命令回调。
    // 入参：无。
    // 返回：借用当前 App 的回调集合，Manager 在 App 退出前解除回调。
    PinWindowCallbacks MakePinCallbacks();
    // 将当前冻结选区准备为独立贴图，成功后结束截图并显示新图。
    // 入参：无。
    // 返回：无返回值；失败保留仍有效选区供重试。
    void PinSelection();
    // 预订贴图输出命令，投递独立请求代次到消息窗口。
    // 入参：id 为稳定贴图标识；command 为复制或保存。
    // 返回：成功排队 true；无效目标、重复请求或忙状态 false。
    bool PostPinCommand(std::uint64_t id, PinCommand command) noexcept;
    // 消费贴图请求，重新检查目标并在模态保护中执行输出。
    // 入参：command 为消息命令；request 为投递时的请求代次。
    // 返回：无返回值；旧请求丢弃，输出完成保留贴图。
    void DispatchPinCommand(PinCommand command, std::uint64_t request);
    // 建立隐藏消息窗口与 App 的关联并转发窗口消息。
    // 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
    // 返回：App 实例处理消息的结果；尚未绑定实例时返回 DefWindowProcW 的结果。
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 处理截图覆盖窗口的绘制、选区输入及会话失效消息。
    // 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
    // 返回：已处理消息的 Win32 结果；无应用关联或未处理消息交给默认窗口过程。
    static LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 处理应用消息中枢收到的启动请求、热键、托盘和退出命令。
    // 入参：window：接收消息的窗口句柄；message：Win32 消息编号；wParam、lParam：对应消息的附加数据。
    // 返回：已消费业务消息的处理结果；其他消息返回 DefWindowProcW 的结果。
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // 注册并创建接收热键、托盘和业务命令的隐藏消息窗口。
    // 入参：无。
    // 返回：消息窗口创建成功时 true；注册或创建失败时 false 并记录日志。
    bool CreateMessageWindow();
    // 为常驻应用添加系统托盘入口并加载图标。
    // 入参：无。
    // 返回：无返回值；成功后记录托盘状态，失败仅记日志，不阻止消息循环。
    void AddTrayIcon();
    // 移除应用已经添加的系统托盘图标。
    // 入参：无。
    // 返回：无返回值；清除本地添加标记，未添加时不操作。
    void RemoveTrayIcon() noexcept;
    // 在语言切换后刷新现有窗口和托盘中的本地化文本。
    // 入参：无。
    // 返回：无返回值；更新已存在的界面对象，不创建新的业务窗口。
    void RefreshLocalizedUi();
    // 合并并显示设置或文本资源读取失败的待提示告警。
    // 入参：无。
    // 返回：无返回值；截图或模态提示期间延后处理，其余时消费告警并显示一次合并提示。
    void ReportDataReadWarnings();
    // 在资源管理器打开实际日志目录，不创建目录或应用设置草稿。
    // 入参：无。
    // 返回：系统打开请求成功为 true。
    bool OpenLogDirectory() noexcept;
    // 为已确认操作启动唯一历史日志任务，后台不占据设置忙状态。
    // 入参：无。
    // 返回：启动成功为 true；退出中、已运行或创建失败为 false。
    bool StartHistoricalLogCleanup() noexcept;
    // 查询独立于设置窗口寿命的维护状态及本地化结果。
    // 入参：无。
    // 返回：忙状态和可显示文字，不消费结果。
    SettingsMaintenanceStatus QueryLogMaintenanceStatus() const;
    // 收取已完成任务并刷新打开的设置窗口，兼顾通知失败恢复。
    // 入参：无。
    // 返回：无；模态忙时延迟界面通知，不丢失结果。
    void DrainLogMaintenance() noexcept;
    // 请求取消并等待后台结束，必须先于 Logger 和通知窗口销毁。
    // 入参：无。
    // 返回：无。
    void StopLogMaintenance() noexcept;
    // 对连续参数错误去重，延迟到截图或模态结束后提示。
    // 入参：invalidFields 为格式及质量错误位。
    // 返回：无。
    void TrackStorageWarnings(std::uint32_t invalidFields) noexcept;
    // 在当前光标位置提供截图、设置、关于及退出托盘菜单。
    // 入参：无。
    // 返回：无返回值；用户选中的命令通过窗口消息交回 App 处理。
    void ShowTrayMenu();
    // 打开或激活应用的唯一非模态设置窗口。
    // 入参：无。
    // 返回：无返回值；首次创建时注入业务回调，忙状态下不重复开启。
    void ShowSettings();
    // 组装供设置和欢迎窗口调用的本地化及系统集成回调。
    // 入参：无。
    // 返回：包含文本、语言、启动项和忙状态通知的回调集合；其中捕获的 App 须存活到回调解除。
    SettingsWindowCallbacks MakeSettingsCallbacks();
    // 创建供截图工具栏同步查询本地化文本的回调。
    // 入参：无。
    // 返回：文本查询函数；在 UI 线程调用，本地化服务须在调用期间可用。
    std::function<std::wstring(std::string_view)> MakeToolbarTextResolver();
    // 创建供截图工具栏投递业务命令的回调。
    // 入参：无。
    // 返回：UI 线程命令接收函数；捕获的 App 须存活到工具栏解除回调。
    std::function<bool(CaptureToolbarCommand, std::uint64_t)> MakeToolbarCommandHandler();
    // 创建供单实例接收线程检查截图准入状态的回调。
    // 入参：无。
    // 返回：仅读取原子门禁的查询函数；App 须先停止并等待监听线程，再释放自身资源。
    std::function<std::optional<LPARAM>()> MakeCaptureGateCallback();
    // 查询当前用户启动入口并生成可显示的本地化状态。
    // 入参：无。
    // 返回：与当前启动注册状态对应的宽字符串；不代表 Windows 系统启动许可已开启。
    std::wstring StartupStatusText() const;
    // 按用户已提交的设置启用或移除当前用户启动入口。
    // 入参：enabled：true 请求登记当前程序启动路径，false 请求移除入口。
    // 返回：系统入口达到请求状态时 true；系统集成服务缺失或操作失败时 false。
    bool ApplyStartup(bool enabled);
    // 显示可重试的退出清理窗口并执行用户选择的清理步骤。
    // 入参：无。
    // 返回：无返回值；失败保留状态提示，用户完成清理或确认退出后请求结束应用。
    void ShowCleanup();
    // 选择系统模态提示的所属窗口。
    // 入参：无。
    // 返回：依次选择有效截图窗口、可见贴图、设置或隐藏消息窗口；不转移所有权。
    HWND DialogOwner() const noexcept;
    // 根据应用忙状态发布跨线程截图准入状态并更新代次。
    // 入参：无。
    // 返回：无返回值；原子更新暂停标记和代次，使较早排队的截图请求失效。
    void UpdateCaptureGate() noexcept;
    // 捕获冻结桌面并建立一次跨显示器截图选区会话。
    // 入参：无。
    // 返回：无返回值；成功显示覆盖窗口并创建工具栏，稳定选区后再显示工具栏；失败清理已创建资源并提示原因。
    void StartCapture();
    // 为已建立的截图覆盖会话创建无激活工具栏。
    // 入参：无。
    // 返回：无返回值；失败记录并提示，截图会话仍可通过键盘操作。
    void CreateCaptureToolbar() noexcept;
    // 按当前选区状态同步工具栏显示位置和可见性。
    // 入参：updateMonitor：true 重新选择目标显示器；false 保留当前目标屏幕。
    // 返回：无返回值；无稳定选区时隐藏工具栏，定位或显示失败记录诊断。
    void RefreshCaptureToolbar(bool updateMonitor = false) noexcept;
    // 撤销旧选区的排队命令并刷新工具栏。
    // 入参：updateMonitor：是否同时重新选择工具栏目标显示器，默认 false。
    // 返回：无返回值；更新命令代次及输入时间边界后同步工具栏。
    void InvalidateToolbarCommands(bool updateMonitor = false) noexcept;
    // 为截图完成命令预订处理位置并投递到 App 消息队列。
    // 入参：command：稳定的工具栏命令 ID；token：提交时的选区代次。
    // 返回：预订及消息投递成功时 true；状态无效、已有请求或投递失败时 false，并撤销失败预订。
    bool PostToolbarCommand(CaptureToolbarCommand command, std::uint64_t token) noexcept;
    // 校验并消费排队的工具栏命令，调用对应截图完成入口。
    // 入参：command：消息携带的命令 ID；token：消息携带的选区代次。
    // 返回：无返回值；过期或不匹配请求被丢弃，有效请求在按钮回调返回后执行业务。
    void DispatchToolbarCommand(CaptureToolbarCommand command, std::uint64_t token);
    // 判断当前截图会话能否接受新的完成命令。
    // 入参：无。
    // 返回：会话有效、选区稳定且不处于准备或模态忙状态时 true，否则 false。
    bool CanSubmitToolbarCommand() const noexcept;
    // 按层级处理 Esc 或右键取消操作。
    // 入参：无。
    // 返回：无返回值；优先取消拖动，再清空已有选区，最后关闭无选区的覆盖会话。
    void CancelSelectionOrClose() noexcept;
    // 在冻结帧捕获后、遮罩创建前采集窗口几何并建立首次候选。
    // 入参：无；读取当前冻结桌面和鼠标物理位置。
    // 返回：无返回值；识别失败保留手动框选，分配失败交由启动保护清理。
    void InitializeWindowSelection();
    // 生成仅供绘制的选区副本，候选不会改变正式模型或输出准入。
    // 入参：无。
    // 返回：正式选区优先，否则返回无控制点的候选绘制快照。
    SelectionSnapshot SelectionForDrawing() const noexcept;
    // 从当前会话的只读窗口几何更新悬停候选。
    // 入参：point 为虚拟桌面物理鼠标坐标。
    // 返回：绘制候选实际变化时 true；正式选区或按下锁定期间不变。
    bool UpdateWindowCandidate(PointI point) noexcept;
    // 用当前鼠标位置恢复悬停候选，读取失败则清空候选。
    // 入参：无。
    // 返回：无返回值；只更新几何，不请求输出或重抓屏幕。
    void RefreshWindowCandidate() noexcept;
    // 区分候选按下待判定与现有自由框选、移动和缩放。
    // 入参：window 为所在遮罩，用于 DPI 容差；point 为物理按下位置。
    // 返回：接受此次交互时 true，调用方随后获取鼠标捕获。
    bool BeginSelectionInput(HWND window, PointI point) noexcept;
    // 推进锁定候选的拖动判定或现有模型，未按下时更新悬停。
    // 入参：point 为最新物理鼠标坐标。
    // 返回：需要刷新绘制时 true。
    bool UpdateSelectionInput(PointI point) noexcept;
    // 结束按下交互，先提交单击或自由框选再由调用方释放鼠标捕获。
    // 入参：point 为物理抬起位置，抬起也检查拖动阈值。
    // 返回：无返回值；零面积自由框选恢复悬停状态。
    void EndSelectionInput(PointI point) noexcept;
    // 查询候选待判定或模型拖动是否仍在进行。
    // 入参：无。
    // 返回：任一交互进行中为 true。
    bool HasSelectionInteraction() const noexcept;
    // 先清除待判定状态并撤销模型交互，避免同步失捕误确认候选。
    // 入参：无。
    // 返回：处理了交互时 true；调用方负责释放捕获和刷新工具栏。
    bool CancelSelectionInput() noexcept;
    // 结束当前截图会话并释放覆盖窗口及相关资源。
    // 入参：无。
    // 返回：无返回值；完成或模态忙状态下先标记失效，待同步调用结束后清理；其余情况立即释放。
    void CloseOverlay() noexcept;
    // 显示包含当前构建版本的本地化关于窗口。
    // 入参：无。
    // 返回：无返回值；复用简单模态消息入口。
    void ShowAbout();
    // 显示截图失败的本地化原因。
    // 入参：detailKey：描述失败原因的本地化文本键，仅在本次同步提示中借用。
    // 返回：无返回值；将原因嵌入截图错误消息正文后显示。
    void ShowCaptureError(std::string_view detailKey);
    // 显示可随语言变化刷新的简单模态提示，并在 Renderer 失败时回退系统提示。
    // 入参：message、title：正文和标题查询回调；fallbackFlags：系统提示标志；owner：可选的受保护所属窗口。
    // 返回：无返回值；守卫覆盖自定义和系统提示两条路径，阻止模态期间再次进入业务。
    void ShowSimpleMessage(std::function<std::wstring()> message, std::function<std::wstring()> title,
                           UINT fallbackFlags, HWND owner = nullptr);

    // 协调选区图像转换、复制或保存及成功后的会话收尾。
    // 入参：save：true 选择路径并保存文件，false 写入剪贴板。
    // 返回：无返回值；成功关闭会话，取消或失败恢复仍有效的选区，布局失效则安全释放。
    void CompleteSelection(bool save);

    // 读取初始组合并尝试注册，失败保持托盘可用。
    // 入参：无。
    // 返回：无；错误通过本地化提示报告。
    void InitializeHotkeys();
    // 取得当前活动组合或未注册状态的显示文字。
    // 入参：无。
    // 返回：当前语言的状态。
    std::wstring HotkeyStatusText() const;
    // 格式化带活动组合占位符的本地化文本。
    // 入参：key 为文本资源键。
    // 返回：已替换组合的显示文字。
    std::wstring HotkeyText(std::string_view key) const;
    // 处理已注册组合，校验身份、暂停和消息时间。
    // 入参：id、data 为 WM_HOTKEY 参数；time 为消息生成时间。
    // 返回：无。
    void DispatchHotkey(WPARAM id, LPARAM data, DWORD time);
    // 准备设置候选，失败不改变当前注册。
    // 入参：value 为目标组合字符串。
    // 返回：候选已就绪为 true。
    bool PrepareHotkey(std::string_view value) noexcept;
    // 无异常激活或回收候选，文字刷新由调用方在事务后执行。
    // 入参：commit 为设置是否已保存。
    // 返回：资源清理成功为 true。
    bool FinishHotkey(bool commit) noexcept;

    std::unique_ptr<HotkeyManager> hotkeys_;
    bool hotkeyRecording_{};
    bool hotkeyCleanupPending_{};
    DWORD hotkeyBoundary_{};
    std::uint32_t storageInvalidFields_{};
    bool storageWarningPending_{};
    bool completionBusy_{}; // 包括错误弹窗在内的忙状态。
    bool dialogActive_{};   // 简单模态弹窗及其系统兜底期间禁止重新打开业务入口。
    bool welcoming_{};
    bool shuttingDown_{};
    bool settingsBusy_{};
    bool pinModalHeld_{}; // App 自己的提示或设置模态对全部贴图持有一层暂停。
    std::unique_ptr<PinWindowManager> pinManager_;
    std::uint64_t pendingPinId_{};
    std::uint64_t pinRequestSerial_{};
    PinCommand pendingPinCommand_{};
    std::atomic<std::uint64_t> captureGate_{1}; // 低位为暂停标记，初始化期间拒绝截图。
    std::unique_ptr<SingleInstance> singleInstance_;
    std::unique_ptr<StartupRegistration> startup_;
    std::unique_ptr<LogMaintenanceTask> logMaintenance_;
    std::unique_ptr<WelcomeWindow> welcomeWindow_;
    WindowRenderer* messageRenderer_{}; // 借用当前模态提示，语言切换时同步刷新。
    std::function<void()> messageStatusRefresh_;
    bool comInitialized_{}; // 仅成功初始化时配对 CoUninitialize。
    std::unique_ptr<SelectionOutputRenderer> outputRenderer_;
    std::unique_ptr<CaptureCompletion> completion_;
    std::unique_ptr<CaptureToolbar> captureToolbar_;
    std::unique_ptr<CaptureCommandGate> toolbarGate_;
    HMONITOR toolbarMonitor_{}; // 只在选区操作结束时重新选择目标屏幕。

    HICON largeIcon_{};         // 自有非共享图标，窗口关闭后由 App 析构释放。
    HICON smallIcon_{};         // 托盘与设置窗口借用，移除后再释放。
    HINSTANCE instance_{};      // 模块实例句柄，不拥有。
    HWND messageWindow_{};      // HWND_MESSAGE：接收热键、托盘和退出消息，不显示界面。
    bool trayAdded_{};          // 只有成功加入系统托盘后，析构时才发送 NIM_DELETE。
    bool overlayPreparing_{};   // 创建/显示 HWND 的同步消息期间禁止销毁正在使用的会话记录。
    bool overlayInvalidated_{}; // 准备期间布局或窗口失效时，在同步调用返回后统一回收。

    // frozen frame、renderer 和 selection 仅在一次截图会话中存在；renderer 不把预览帧当作输出源。
    // App 对外头文件使用前置声明，避免把 DirectX 头传播给依赖 application 的模块。
    std::unique_ptr<FrozenDesktopFrame> frozenDesktopFrame_;
    std::unique_ptr<CaptureOverlaySession> overlaySession_;
    std::unique_ptr<SelectionModel> selectionModel_;
    std::unique_ptr<WindowSelectionSnapshot> windowSelection_;
    std::unique_ptr<CaptureSelectionInput> selectionInput_;
    std::optional<RECT> windowCandidate_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
};
} // namespace open_st
