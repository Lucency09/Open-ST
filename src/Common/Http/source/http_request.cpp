// 文件职责：纯请求参数校验，不建立连接或包含产品协议规则。
#include <http_transport.h>
#include <string_view>
#include <windows.h>
#include <winhttp.h>

namespace open_st::http
{
// 统一校验公共请求参数，不建立网络连接，不判断供应商的业务路径。
// 入参：request 为候选请求。返回：URL、代理、头和预算有效时 true。
bool IsValidRequest(const Request& request) noexcept
{
    try
    {
        if (request.url.empty() || request.url.size() > 32768 || request.idleTimeout.count() <= 0 ||
            request.connectTimeout.count() <= 0 || request.maxResponseBytes == 0 || request.maxErrorBytes == 0 ||
            request.body.size() > MAXDWORD || request.url.find_first_of(L"\r\n") != std::wstring::npos ||
            request.url.find(L'\0') != std::wstring::npos || request.url.find(L'#') != std::wstring::npos ||
            (request.method != Method::Get && request.method != Method::Post && request.method != Method::Put &&
             request.method != Method::Patch) ||
            (request.method == Method::Get && !request.body.empty()))
        {
            return false;
        }
        std::wstring headers;
        for (const auto& header : request.headers)
        {
            if (header.name.empty() ||
                header.name.find_first_not_of(
                    L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!#$%&'*+.^_`|~") !=
                    std::wstring::npos ||
                header.value.find_first_of(L"\r\n") != std::wstring::npos ||
                header.value.find(L'\0') != std::wstring::npos || _wcsicmp(header.name.c_str(), L"Host") == 0 ||
                _wcsicmp(header.name.c_str(), L"Content-Length") == 0 ||
                _wcsicmp(header.name.c_str(), L"Transfer-Encoding") == 0)
            {
                return false;
            }
            headers += header.name + L": " + header.value + L"\r\n";
            if (headers.size() > 32768)
            {
                return false;
            }
        }
        if (request.proxy.mode != ProxyMode::System && request.proxy.mode != ProxyMode::Custom)
        {
            return false;
        }
        if (request.proxy.mode == ProxyMode::Custom)
        {
            const std::wstring proxyUrl = request.proxy.address.find(L"://") == std::wstring::npos
                                              ? L"http://" + request.proxy.address
                                              : request.proxy.address;
            URL_COMPONENTS proxy{};
            proxy.dwStructSize = sizeof(proxy);
            proxy.dwHostNameLength = proxy.dwUrlPathLength = proxy.dwExtraInfoLength = proxy.dwUserNameLength =
                proxy.dwPasswordLength = static_cast<DWORD>(-1);
            if (request.proxy.address.size() > 2048 || request.proxy.address.find(L'\0') != std::wstring::npos ||
                request.proxy.address.find_first_of(L"\r\n") != std::wstring::npos ||
                !WinHttpCrackUrl(proxyUrl.c_str(), static_cast<DWORD>(proxyUrl.size()), 0, &proxy) ||
                proxy.nScheme != INTERNET_SCHEME_HTTP || proxy.dwHostNameLength == 0 || proxy.dwUserNameLength != 0 ||
                proxy.dwPasswordLength != 0 || proxy.dwExtraInfoLength != 0 ||
                (proxy.dwUrlPathLength != 0 && std::wstring_view(proxy.lpszUrlPath, proxy.dwUrlPathLength) != L"/"))
            {
                return false;
            }
        }
        URL_COMPONENTS components{};
        components.dwStructSize = sizeof(components);
        components.dwHostNameLength = static_cast<DWORD>(-1);
        components.dwUrlPathLength = static_cast<DWORD>(-1);
        components.dwExtraInfoLength = static_cast<DWORD>(-1);
        components.dwUserNameLength = static_cast<DWORD>(-1);
        components.dwPasswordLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(request.url.c_str(), static_cast<DWORD>(request.url.size()), 0, &components) ||
            (components.nScheme != INTERNET_SCHEME_HTTPS &&
             !(request.allowHttp && components.nScheme == INTERNET_SCHEME_HTTP)) ||
            components.dwHostNameLength == 0 || components.dwUserNameLength != 0 || components.dwPasswordLength != 0)
        {
            return false;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

} // namespace open_st::http
