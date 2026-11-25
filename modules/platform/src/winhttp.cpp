#include <dafeiyu/http.hpp>
#include <windows.h>
#include <winhttp.h>
#include <memory>
#include <stdexcept>

namespace dfy::http {
namespace {
using Handle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;

// 将 WinHTTP 失败转换为不含请求或凭据的网络错误。
void require(bool success) {
    if (!success) {
        const auto code = GetLastError();
        throw std::runtime_error(
            code == ERROR_WINHTTP_TIMEOUT ? "network: timeout" : "network: WinHTTP error " + std::to_string(code));
    }
}

// 接管新建的 WinHTTP 句柄，使正常返回和异常退出都自动释放资源。
Handle own(HINTERNET handle) {
    require(handle != nullptr);
    return Handle(handle, WinHttpCloseHandle);
}
}

// 通过 Windows 系统网络栈执行一次 HTTPS JSON 请求，保留系统证书校验。
// 步骤：解析地址 → 创建连接并设置超时 → 发送认证请求 → 读取状态与有界正文。
Response post_json(const std::string& url, const std::string& api_key, const std::string& body, int timeout_ms) {
    const std::wstring address(url.begin(), url.end());
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    require(WinHttpCrackUrl(address.c_str(), 0, ICU_REJECT_USERPWD, &parts));
    if (parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwExtraInfoLength != 0 || body.size() > 8 * 1024 * 1024) {
        throw std::runtime_error("network: expected HTTPS URL and request within 8 MiB");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);

    auto session = own(WinHttpOpen(
        L"DaFeiYu/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    require(WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms));
    auto connection = own(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    auto request = own(WinHttpOpenRequest(connection.get(), L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));

    // 不跟随重定向，避免将认证信息传递到另一个地址。
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    require(WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)));
    const std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\nAuthorization: Bearer " +
                                 std::wstring(api_key.begin(), api_key.end()) + L"\r\n";
    require(WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
        const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0));
    require(WinHttpReceiveResponse(request.get(), nullptr));

    DWORD status = 0, size = sizeof(status);
    require(WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));
    Response response{status, {}};
    if (status != 200) {
        return response;
    }

    char buffer[8192];
    DWORD count = 0;
    do {
        require(WinHttpReadData(request.get(), buffer, sizeof(buffer), &count));
        if (response.body.size() + count > 8 * 1024 * 1024) {
            throw std::runtime_error("network: response exceeds 8 MiB");
        }
        response.body.append(buffer, count);
    } while (count != 0);
    return response;
}
}
