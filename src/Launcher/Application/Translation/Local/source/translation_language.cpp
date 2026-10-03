// 使用 Windows 离线 ELS 在模型读取前识别源语言，识别只处理独立 NFC 副本。
#include <translation_language.h>
#include <translation_lines.h>
#include <windows.h>
#include <elscore.h>
#include <elssrvc.h>
#include <memory>

namespace open_st::translation_local::detail
{
namespace
{
// 同步调用固定离线服务并复制有界结果，释放顺序保证输入活到属性袋释放之后。
// 入参：text 为 NFC UTF-16 全文。返回：按相关性排列的原始多字符串，失败为空。
std::wstring DetectWithWindows(std::wstring_view text)
{
    GUID serviceId = ELS_GUID_LANGUAGE_DETECTION;
    MAPPING_ENUM_OPTIONS options{};
    options.Size = sizeof(options);
    options.pGuid = &serviceId;
    options.OnlineService = OFFLINE_SERVICES;
    MAPPING_SERVICE_INFO* services{};
    DWORD count{};
    const HRESULT listed = MappingGetServices(&options, &services, &count);
    // 释放系统枚举表；属性袋比枚举表先释放。
    // 入参：value 为服务表。返回：无。
    const auto releaseServices = [](MAPPING_SERVICE_INFO* value) { (void)MappingFreeServices(value); };
    const std::unique_ptr<MAPPING_SERVICE_INFO, decltype(releaseServices)> ownedServices(services, releaseServices);
    if (FAILED(listed) || services == nullptr || count == 0)
        return {};
    MAPPING_PROPERTY_BAG bag{};
    bag.Size = sizeof(bag);
    const HRESULT recognized =
        MappingRecognizeText(services, text.data(), static_cast<DWORD>(text.size()), 0, nullptr, &bag);
    // 释放系统属性袋，包括失败时已分配的上下文。
    // 入参：value 为属性袋。返回：无。
    const auto releaseBag = [](MAPPING_PROPERTY_BAG* value) { (void)MappingFreePropertyBag(value); };
    const std::unique_ptr<MAPPING_PROPERTY_BAG, decltype(releaseBag)> ownedBag(&bag, releaseBag);
    if (FAILED(recognized) || bag.dwRangesCount == 0 || bag.prgResultRanges == nullptr)
        return {};
    const MAPPING_DATA_RANGE& range = bag.prgResultRanges[0];
    if (range.pData == nullptr || range.dwDataSize < sizeof(wchar_t) * 2 || range.dwDataSize % sizeof(wchar_t) != 0 ||
        range.dwDataSize > 65536)
        return {};
    return std::wstring(static_cast<const wchar_t*>(range.pData), range.dwDataSize / sizeof(wchar_t));
}
} // namespace
// 识别始终与翻译共用取消和截止时间；系统不可中断调用返回后立即重新检查。
// 入参：UTF-8 全文、整轮预算和可选测试检测器。返回：只映射首选明确支持的语言。
Result DetectSourceLanguage(std::string_view text, std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                            const LanguageDetector& detector)
{
    if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
        return {stopped};
    if (text.empty() || text.size() > 40000 || text.find('\0') != std::string_view::npos)
        return {Error::InvalidInput};
    const int size =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return {Error::InvalidInput};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(),
                            size) != size)
        return {Error::InvalidInput};
    const int capacity = NormalizeString(NormalizationC, wide.data(), size, nullptr, 0);
    if (capacity <= 0)
        return {Error::NotApplicable};
    std::wstring normalized(static_cast<std::size_t>(capacity), L'\0');
    const int written = NormalizeString(NormalizationC, wide.data(), size, normalized.data(), capacity);
    if (written <= 0)
        return {Error::NotApplicable};
    normalized.resize(static_cast<std::size_t>(written));
    if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
        return {stopped};
    const std::wstring languages = detector ? detector(normalized) : DetectWithWindows(normalized);
    if (const Error stopped = StopReason(deadline, stop); stopped != Error::None)
        return {stopped};
    const std::size_t end = languages.find(L'\0');
    if (end == std::wstring::npos || end == 0)
        return {Error::NotApplicable};
    const std::wstring_view preferred(languages.data(), end);
    Result result;
    if (preferred == L"en" || preferred.starts_with(L"en-"))
        result.detectedLanguage = "en";
    // 中性 zh 已明确中文，可归一化到现有中文源模型；明确繁体 zh-Hant 不改写。
    else if (preferred == L"zh" || preferred == L"zh-Hans" || preferred.starts_with(L"zh-Hans-") ||
             preferred == L"zh-CN" || preferred == L"zh-SG")
        result.detectedLanguage = "zh-CN";
    else
        result.error = Error::NotApplicable;
    return result;
}
} // namespace open_st::translation_local::detail
