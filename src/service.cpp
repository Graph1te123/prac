#include <windows.h>
#include <rpc.h>
#include <strsafe.h>
#include <userenv.h>
#include <winhttp.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "constants.h"
#include "prac_service.h"

namespace {

struct ScopedHandle {
    HANDLE value = nullptr;

    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : value(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        reset();
    }

    HANDLE get() const {
        return value;
    }

    HANDLE* put() {
        reset();
        return &value;
    }

    HANDLE release() {
        HANDLE released = value;
        value = nullptr;
        return released;
    }

    void reset(HANDLE handle = nullptr) {
        if (value != nullptr && value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
        value = handle;
    }
};

struct LaunchedProcess {
    DWORD sessionId = 0;
    DWORD processId = 0;
    HANDLE process = nullptr;
};

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
};

struct ServiceConfig {
    std::wstring apiBaseUrl;
    std::wstring authMethod;
    std::wstring authEndpoint;
    std::wstring refreshMethod;
    std::wstring refreshEndpoint;
    std::wstring licenseMethod;
    std::wstring licenseEndpoint;
    std::wstring activateMethod;
    std::wstring activateEndpoint;
};

struct AuthState {
    std::wstring userName;
    std::wstring accessToken;
    std::wstring refreshToken;
    int64_t accessExpiresAt = 0;
    int64_t refreshExpiresAt = 0;

    bool IsAuthenticated() const {
        return !accessToken.empty() && !refreshToken.empty();
    }
};

struct LicenseState {
    bool hasLicense = false;
    std::wstring status = L"No active license";
    std::wstring ticket;
    int64_t expiresAt = 0;
    int64_t refreshAt = 0;
};

SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
SERVICE_STATUS g_serviceStatus{};
DWORD g_checkpoint = 1;
HANDLE g_rpcStoppedEvent = nullptr;
HANDLE g_stateChangedEvent = nullptr;
HANDLE g_refreshThread = nullptr;
std::mutex g_processesMutex;
std::vector<LaunchedProcess> g_processes;
std::mutex g_stateMutex;
AuthState g_authState;
LicenseState g_licenseState;

std::wstring GetLastErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message = size != 0 && buffer != nullptr ? buffer : L"Unknown error";
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

void WriteEventLog(WORD type, const std::wstring& message) {
    HANDLE source = RegisterEventSourceW(nullptr, kServiceName);
    if (source == nullptr) {
        return;
    }

    LPCWSTR strings[] = {message.c_str()};
    ::ReportEventW(source, type, 0, 0, nullptr, 1, 0, strings, nullptr);
    DeregisterEventSource(source);
}

int64_t CurrentUnixTime() {
    return static_cast<int64_t>(std::time(nullptr));
}

void SignalStateChanged() {
    if (g_stateChangedEvent != nullptr) {
        SetEvent(g_stateChangedEvent);
    }
}

std::wstring ReadEnvironmentString(const wchar_t* name, const wchar_t* fallback) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) {
        return fallback;
    }

    std::wstring value(length, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), length);
    if (written == 0 || written >= length) {
        return fallback;
    }

    value.resize(written);
    return value.empty() ? fallback : value;
}

ServiceConfig LoadServiceConfig() {
    ServiceConfig config{};
    config.apiBaseUrl = ReadEnvironmentString(L"PRAC_API_BASE_URL", L"https://localhost");
    config.authMethod = ReadEnvironmentString(L"PRAC_AUTH_METHOD", L"POST");
    config.authEndpoint = ReadEnvironmentString(L"PRAC_AUTH_ENDPOINT", L"/auth/login");
    config.refreshMethod = ReadEnvironmentString(L"PRAC_REFRESH_METHOD", L"POST");
    config.refreshEndpoint = ReadEnvironmentString(L"PRAC_REFRESH_ENDPOINT", L"/auth/refresh");
    config.licenseMethod = ReadEnvironmentString(L"PRAC_LICENSE_METHOD", L"GET");
    config.licenseEndpoint = ReadEnvironmentString(L"PRAC_LICENSE_ENDPOINT", L"/license");
    config.activateMethod = ReadEnvironmentString(L"PRAC_ACTIVATE_METHOD", L"POST");
    config.activateEndpoint =
        ReadEnvironmentString(L"PRAC_ACTIVATE_ENDPOINT", L"/license/activate");
    return config;
}

std::wstring BuildUrl(const std::wstring& baseUrl, const std::wstring& endpoint) {
    if (endpoint.rfind(L"https://", 0) == 0 || endpoint.rfind(L"HTTPS://", 0) == 0) {
        return endpoint;
    }

    if (baseUrl.empty()) {
        return endpoint;
    }

    const bool baseHasSlash = baseUrl.back() == L'/';
    const bool endpointHasSlash = !endpoint.empty() && endpoint.front() == L'/';
    if (baseHasSlash && endpointHasSlash) {
        return baseUrl + endpoint.substr(1);
    }
    if (!baseHasSlash && !endpointHasSlash) {
        return baseUrl + L"/" + endpoint;
    }
    return baseUrl + endpoint;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int length = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(length, '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        result.data(),
        length,
        nullptr,
        nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(length, L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        length);
    return result;
}

std::string JsonEscape(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(utf8.size());
    for (char ch : utf8) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

bool ExtractJsonString(const std::string& json, const char* key, std::string& value) {
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    size_t keyPosition = json.find(quotedKey);
    if (keyPosition == std::string::npos) {
        return false;
    }

    size_t colon = json.find(':', keyPosition + quotedKey.size());
    if (colon == std::string::npos) {
        return false;
    }

    size_t start = json.find('"', colon + 1);
    if (start == std::string::npos) {
        return false;
    }
    ++start;

    std::string result;
    bool escaped = false;
    for (size_t index = start; index < json.size(); ++index) {
        const char ch = json[index];
        if (escaped) {
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    result += ch;
                    break;
                case 'b':
                    result += '\b';
                    break;
                case 'f':
                    result += '\f';
                    break;
                case 'n':
                    result += '\n';
                    break;
                case 'r':
                    result += '\r';
                    break;
                case 't':
                    result += '\t';
                    break;
                default:
                    result += ch;
                    break;
            }
            escaped = false;
            continue;
        }

        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            value = result;
            return true;
        }
        result += ch;
    }

    return false;
}

bool ExtractJsonInt64(const std::string& json, const char* key, int64_t& value) {
    const std::string quotedKey = "\"" + std::string(key) + "\"";
    size_t keyPosition = json.find(quotedKey);
    if (keyPosition == std::string::npos) {
        return false;
    }

    size_t colon = json.find(':', keyPosition + quotedKey.size());
    if (colon == std::string::npos) {
        return false;
    }

    size_t start = json.find_first_of("-0123456789\"", colon + 1);
    if (start == std::string::npos) {
        return false;
    }

    bool quoted = json[start] == '"';
    if (quoted) {
        ++start;
    }

    size_t end = start;
    while (end < json.size() && (json[end] == '-' || (json[end] >= '0' && json[end] <= '9'))) {
        ++end;
    }

    if (end == start) {
        return false;
    }

    value = _strtoi64(json.substr(start, end - start).c_str(), nullptr, 10);
    return true;
}

int Base64UrlValue(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '-' || ch == '+') {
        return 62;
    }
    if (ch == '_' || ch == '/') {
        return 63;
    }
    return -1;
}

std::string DecodeBase64Url(const std::string& input) {
    std::string output;
    int value = 0;
    int bits = -8;
    for (char ch : input) {
        if (ch == '=') {
            break;
        }

        const int decoded = Base64UrlValue(ch);
        if (decoded < 0) {
            return {};
        }

        value = (value << 6) + decoded;
        bits += 6;
        if (bits >= 0) {
            output.push_back(static_cast<char>((value >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return output;
}

int64_t ExtractJwtExpiry(const std::wstring& token) {
    const std::string utf8Token = WideToUtf8(token);
    const size_t firstDot = utf8Token.find('.');
    if (firstDot == std::string::npos) {
        return 0;
    }

    const size_t secondDot = utf8Token.find('.', firstDot + 1);
    if (secondDot == std::string::npos) {
        return 0;
    }

    const std::string payload =
        DecodeBase64Url(utf8Token.substr(firstDot + 1, secondDot - firstDot - 1));

    int64_t expiresAt = 0;
    if (ExtractJsonInt64(payload, "exp", expiresAt)) {
        return expiresAt;
    }
    return 0;
}

int64_t RefreshMomentFromExpiry(int64_t expiresAt) {
    const int64_t now = CurrentUnixTime();
    if (expiresAt <= now + 90) {
        return now + 30;
    }

    const int64_t lifetime = expiresAt - now;
    return now + std::max<int64_t>(60, lifetime / 2);
}

HttpResponse SendHttpsJsonRequest(
    const std::wstring& method,
    const std::wstring& url,
    const std::string& body,
    const std::wstring& bearerToken,
    DWORD& lastError) {
    lastError = ERROR_SUCCESS;
    HttpResponse response{};

    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        lastError = ERROR_INVALID_PARAMETER;
        return response;
    }

    std::wstring host(components.lpszHostName, components.dwHostNameLength);
    std::wstring path;
    if (components.dwUrlPathLength != 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    if (components.dwExtraInfoLength != 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) {
        path = L"/";
    }

    HINTERNET session = WinHttpOpen(
        L"PracTrayService/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session == nullptr) {
        lastError = GetLastError();
        return response;
    }

    HINTERNET connection = WinHttpConnect(session, host.c_str(), components.nPort, 0);
    if (connection == nullptr) {
        lastError = GetLastError();
        WinHttpCloseHandle(session);
        return response;
    }

    HINTERNET request = WinHttpOpenRequest(
        connection,
        method.c_str(),
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        lastError = GetLastError();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!body.empty()) {
        headers += L"Content-Type: application/json; charset=utf-8\r\n";
    }
    if (!bearerToken.empty()) {
        headers += L"Authorization: Bearer " + bearerToken + L"\r\n";
    }

    LPVOID requestBody = body.empty() ? nullptr : const_cast<char*>(body.data());
    const DWORD bodyLength = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(
            request,
            headers.c_str(),
            static_cast<DWORD>(-1),
            requestBody,
            bodyLength,
            bodyLength,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        lastError = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &statusCodeSize,
        WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            lastError = GetLastError();
            break;
        }
        if (available == 0) {
            break;
        }

        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            lastError = GetLastError();
            break;
        }
        response.body.append(buffer.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

unsigned long HttpFailureToRpcError(const HttpResponse& response, DWORD lastError) {
    if (lastError != ERROR_SUCCESS || response.statusCode == 0) {
        return kPracRpcNetworkError;
    }
    return kPracRpcBadResponse;
}

bool IsHttpSuccess(DWORD statusCode) {
    return statusCode >= 200 && statusCode < 300;
}

bool ExtractFirstJsonString(
    const std::string& json,
    const std::vector<const char*>& keys,
    std::wstring& value) {
    for (const char* key : keys) {
        std::string utf8;
        if (ExtractJsonString(json, key, utf8)) {
            value = Utf8ToWide(utf8);
            return true;
        }
    }
    return false;
}

bool ExtractFirstJsonInt64(
    const std::string& json,
    const std::vector<const char*>& keys,
    int64_t& value) {
    for (const char* key : keys) {
        if (ExtractJsonInt64(json, key, value)) {
            return true;
        }
    }
    return false;
}

void ClearAuthAndLicenseLocked() {
    g_authState = AuthState{};
    g_licenseState = LicenseState{};
}

void ClearLicenseLocked() {
    g_licenseState = LicenseState{};
}

unsigned long StoreTokensFromResponse(
    const std::string& json,
    const std::wstring& fallbackUserName,
    bool resetLicense) {
    std::wstring accessToken;
    std::wstring refreshToken;
    if (!ExtractFirstJsonString(
            json,
            {"accessToken", "access_token", "access", "jwt"},
            accessToken) ||
        !ExtractFirstJsonString(
            json,
            {"refreshToken", "refresh_token", "refresh"},
            refreshToken)) {
        return kPracRpcBadResponse;
    }

    std::wstring responseUserName;
    ExtractFirstJsonString(
        json,
        {"userName", "username", "login", "name", "email"},
        responseUserName);

    int64_t accessExpiresAt = ExtractJwtExpiry(accessToken);
    int64_t refreshExpiresAt = ExtractJwtExpiry(refreshToken);
    ExtractFirstJsonInt64(json, {"accessExpiresAt", "access_expires_at"}, accessExpiresAt);
    ExtractFirstJsonInt64(json, {"refreshExpiresAt", "refresh_expires_at"}, refreshExpiresAt);

    const int64_t now = CurrentUnixTime();
    int64_t accessExpiresIn = 0;
    int64_t refreshExpiresIn = 0;
    if (ExtractFirstJsonInt64(json, {"accessExpiresIn", "access_expires_in"}, accessExpiresIn)) {
        accessExpiresAt = now + accessExpiresIn;
    }
    if (ExtractFirstJsonInt64(json, {"refreshExpiresIn", "refresh_expires_in"}, refreshExpiresIn)) {
        refreshExpiresAt = now + refreshExpiresIn;
    }

    if (accessExpiresAt <= now) {
        accessExpiresAt = now + 15 * 60;
    }
    if (refreshExpiresAt <= now) {
        refreshExpiresAt = now + 24 * 60 * 60;
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_authState.userName = responseUserName.empty() ? fallbackUserName : responseUserName;
        g_authState.accessToken = accessToken;
        g_authState.refreshToken = refreshToken;
        g_authState.accessExpiresAt = accessExpiresAt;
        g_authState.refreshExpiresAt = refreshExpiresAt;
        if (resetLicense) {
            ClearLicenseLocked();
        }
    }
    SignalStateChanged();
    return kPracRpcSuccess;
}

unsigned long StoreLicenseFromResponse(const std::string& json) {
    std::wstring ticket;
    ExtractFirstJsonString(
        json,
        {"ticket", "licenseTicket", "license_ticket", "license"},
        ticket);

    bool hasLicense = !ticket.empty();
    int64_t expiresAt = ExtractJwtExpiry(ticket);
    ExtractFirstJsonInt64(json, {"expiresAt", "expires_at", "validUntil", "valid_until"}, expiresAt);

    int64_t expiresIn = 0;
    if (ExtractFirstJsonInt64(json, {"expiresIn", "expires_in", "ttl"}, expiresIn)) {
        expiresAt = CurrentUnixTime() + expiresIn;
    }

    std::wstring status;
    ExtractFirstJsonString(json, {"status", "licenseStatus", "license_status"}, status);
    if (status.empty()) {
        status = hasLicense ? L"Active" : L"No active license";
    }

    if (hasLicense && expiresAt <= CurrentUnixTime()) {
        expiresAt = CurrentUnixTime() + 24 * 60 * 60;
    }

    int64_t refreshAt = 0;
    ExtractFirstJsonInt64(json, {"refreshAt", "refresh_at", "nextRefreshAt"}, refreshAt);
    if (hasLicense && refreshAt <= CurrentUnixTime()) {
        refreshAt = RefreshMomentFromExpiry(expiresAt);
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_licenseState.hasLicense = hasLicense;
        g_licenseState.status = status;
        g_licenseState.ticket = ticket;
        g_licenseState.expiresAt = hasLicense ? expiresAt : 0;
        g_licenseState.refreshAt = hasLicense ? refreshAt : 0;
    }
    SignalStateChanged();
    return kPracRpcSuccess;
}

unsigned long AuthenticateUser(const std::wstring& userName, const std::wstring& password) {
    if (userName.empty() || password.empty()) {
        return kPracRpcInvalidArgument;
    }

    const ServiceConfig config = LoadServiceConfig();
    const std::string body =
        "{\"username\":\"" + JsonEscape(userName) + "\",\"password\":\"" +
        JsonEscape(password) + "\"}";

    DWORD lastError = ERROR_SUCCESS;
    const HttpResponse response = SendHttpsJsonRequest(
        config.authMethod,
        BuildUrl(config.apiBaseUrl, config.authEndpoint),
        body,
        L"",
        lastError);
    if (!IsHttpSuccess(response.statusCode)) {
        return HttpFailureToRpcError(response, lastError);
    }

    return StoreTokensFromResponse(response.body, userName, true);
}

unsigned long RefreshTokens() {
    std::wstring refreshToken;
    std::wstring userName;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_authState.IsAuthenticated()) {
            return kPracRpcNotAuthenticated;
        }
        refreshToken = g_authState.refreshToken;
        userName = g_authState.userName;
    }

    const ServiceConfig config = LoadServiceConfig();
    const std::string body =
        "{\"refreshToken\":\"" + JsonEscape(refreshToken) + "\"}";

    DWORD lastError = ERROR_SUCCESS;
    const HttpResponse response = SendHttpsJsonRequest(
        config.refreshMethod,
        BuildUrl(config.apiBaseUrl, config.refreshEndpoint),
        body,
        L"",
        lastError);
    if (!IsHttpSuccess(response.statusCode)) {
        if (response.statusCode == 401 || response.statusCode == 403) {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            ClearAuthAndLicenseLocked();
            SignalStateChanged();
        }
        return HttpFailureToRpcError(response, lastError);
    }

    return StoreTokensFromResponse(response.body, userName, false);
}

unsigned long FetchLicenseStatus(bool retriedAfterRefresh = false) {
    std::wstring accessToken;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_authState.IsAuthenticated()) {
            return kPracRpcNotAuthenticated;
        }
        accessToken = g_authState.accessToken;
    }

    const ServiceConfig config = LoadServiceConfig();
    DWORD lastError = ERROR_SUCCESS;
    const HttpResponse response = SendHttpsJsonRequest(
        config.licenseMethod,
        BuildUrl(config.apiBaseUrl, config.licenseEndpoint),
        "",
        accessToken,
        lastError);
    if ((response.statusCode == 401 || response.statusCode == 403) && !retriedAfterRefresh) {
        const unsigned long refreshResult = RefreshTokens();
        if (refreshResult == kPracRpcSuccess) {
            return FetchLicenseStatus(true);
        }
    }
    if (response.statusCode == 404 || response.statusCode == 204) {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ClearLicenseLocked();
        SignalStateChanged();
        return kPracRpcSuccess;
    }
    if (!IsHttpSuccess(response.statusCode)) {
        return HttpFailureToRpcError(response, lastError);
    }

    return StoreLicenseFromResponse(response.body);
}

unsigned long ActivateLicense(const std::wstring& activationCode) {
    if (activationCode.empty()) {
        return kPracRpcInvalidArgument;
    }

    std::wstring accessToken;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!g_authState.IsAuthenticated()) {
            return kPracRpcNotAuthenticated;
        }
        accessToken = g_authState.accessToken;
    }

    const ServiceConfig config = LoadServiceConfig();
    const std::string body =
        "{\"activationCode\":\"" + JsonEscape(activationCode) + "\"}";

    DWORD lastError = ERROR_SUCCESS;
    const HttpResponse response = SendHttpsJsonRequest(
        config.activateMethod,
        BuildUrl(config.apiBaseUrl, config.activateEndpoint),
        body,
        accessToken,
        lastError);
    if (!IsHttpSuccess(response.statusCode)) {
        return HttpFailureToRpcError(response, lastError);
    }

    std::wstring ticket;
    if (ExtractFirstJsonString(
            response.body,
            {"ticket", "licenseTicket", "license_ticket", "license"},
            ticket)) {
        return StoreLicenseFromResponse(response.body);
    }

    return FetchLicenseStatus();
}

bool TokenRefreshIsDue() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_authState.IsAuthenticated() &&
           g_authState.accessExpiresAt <= CurrentUnixTime() + 60;
}

bool LicenseRefreshIsDue() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_licenseState.hasLicense &&
           g_licenseState.refreshAt > 0 &&
           g_licenseState.refreshAt <= CurrentUnixTime();
}

DWORD CalculateRefreshWaitMs() {
    int64_t nextWake = CurrentUnixTime() + 60;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_authState.IsAuthenticated()) {
            nextWake = std::min(nextWake, g_authState.accessExpiresAt - 60);
        }
        if (g_licenseState.hasLicense && g_licenseState.refreshAt > 0) {
            nextWake = std::min(nextWake, g_licenseState.refreshAt);
        }
    }

    const int64_t now = CurrentUnixTime();
    const int64_t seconds = std::max<int64_t>(5, std::min<int64_t>(60, nextWake - now));
    return static_cast<DWORD>(seconds * 1000);
}

DWORD WINAPI RefreshWorkerThread(LPVOID) {
    HANDLE events[] = {g_rpcStoppedEvent, g_stateChangedEvent};
    for (;;) {
        const DWORD waitResult = WaitForMultipleObjects(
            ARRAYSIZE(events),
            events,
            FALSE,
            CalculateRefreshWaitMs());
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED) {
            break;
        }

        if (TokenRefreshIsDue()) {
            RefreshTokens();
        }
        if (LicenseRefreshIsDue()) {
            FetchLicenseStatus();
        }
    }
    return 0;
}

unsigned long CopyRpcString(
    const std::wstring& value,
    unsigned long capacity,
    wchar_t* buffer) {
    if (buffer == nullptr || capacity == 0) {
        return kPracRpcInvalidArgument;
    }

    const HRESULT result = StringCchCopyW(buffer, capacity, value.c_str());
    return SUCCEEDED(result) ? kPracRpcSuccess : ERROR_INSUFFICIENT_BUFFER;
}

void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = win32ExitCode;
    g_serviceStatus.dwWaitHint = waitHint;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        g_serviceStatus.dwControlsAccepted = 0;
        g_serviceStatus.dwCheckPoint = g_checkpoint++;
    } else {
        g_serviceStatus.dwControlsAccepted =
            state == SERVICE_RUNNING ? SERVICE_ACCEPT_SESSIONCHANGE : 0;
        g_serviceStatus.dwCheckPoint = 0;
    }

    if (g_serviceStatusHandle != nullptr) {
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
    }
}

std::wstring GetModuleDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = 0;

    for (;;) {
        length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return L".";
        }
        if (length < path.size() - 1) {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }

    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    path.resize(slash);
    return path;
}

std::wstring GetTrayExecutablePath() {
    std::wstring directory = GetModuleDirectory();
    if (!directory.empty() && directory.back() != L'\\' && directory.back() != L'/') {
        directory += L'\\';
    }
    return directory + kTrayExecutableName;
}

void PruneExitedProcessesLocked() {
    auto iterator = g_processes.begin();
    while (iterator != g_processes.end()) {
        const DWORD waitResult = WaitForSingleObject(iterator->process, 0);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED) {
            CloseHandle(iterator->process);
            iterator = g_processes.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

bool HasProcessForSessionLocked(DWORD sessionId) {
    return std::any_of(
        g_processes.begin(),
        g_processes.end(),
        [sessionId](const LaunchedProcess& process) {
            return process.sessionId == sessionId &&
                   WaitForSingleObject(process.process, 0) == WAIT_TIMEOUT;
        });
}

bool LaunchTrayForSession(DWORD sessionId) {
    if (sessionId == 0) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_processesMutex);
        PruneExitedProcessesLocked();
        if (HasProcessForSessionLocked(sessionId)) {
            return true;
        }
    }

    ScopedHandle userToken;
    if (!WTSQueryUserToken(sessionId, userToken.put())) {
        return false;
    }

    ScopedHandle primaryToken;
    if (!DuplicateTokenEx(
            userToken.get(),
            MAXIMUM_ALLOWED,
            nullptr,
            SecurityIdentification,
            TokenPrimary,
            primaryToken.put())) {
        WriteEventLog(
            EVENTLOG_WARNING_TYPE,
            L"DuplicateTokenEx failed: " + GetLastErrorMessage(GetLastError()));
        return false;
    }

    LPVOID environment = nullptr;
    DWORD creationFlags = 0;
    if (CreateEnvironmentBlock(&environment, primaryToken.get(), FALSE)) {
        creationFlags |= CREATE_UNICODE_ENVIRONMENT;
    }

    const std::wstring trayPath = GetTrayExecutablePath();
    const std::wstring workingDirectory = GetModuleDirectory();
    std::wstring commandLine = L"\"" + trayPath + L"\" --background";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessAsUserW(
        primaryToken.get(),
        trayPath.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        creationFlags,
        environment,
        workingDirectory.c_str(),
        &startupInfo,
        &processInfo);

    if (environment != nullptr) {
        DestroyEnvironmentBlock(environment);
    }

    if (!created) {
        WriteEventLog(
            EVENTLOG_WARNING_TYPE,
            L"CreateProcessAsUserW failed: " + GetLastErrorMessage(GetLastError()));
        return false;
    }

    CloseHandle(processInfo.hThread);
    {
        std::lock_guard<std::mutex> lock(g_processesMutex);
        g_processes.push_back({sessionId, processInfo.dwProcessId, processInfo.hProcess});
    }
    return true;
}

void LaunchTrayForAllSessions() {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD sessionCount = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount)) {
        WriteEventLog(
            EVENTLOG_WARNING_TYPE,
            L"WTSEnumerateSessionsW failed: " + GetLastErrorMessage(GetLastError()));
        return;
    }

    for (DWORD index = 0; index < sessionCount; ++index) {
        if (sessions[index].SessionId != 0) {
            LaunchTrayForSession(sessions[index].SessionId);
        }
    }

    WTSFreeMemory(sessions);
}

void TerminateLaunchedProcesses() {
    std::vector<LaunchedProcess> processes;
    {
        std::lock_guard<std::mutex> lock(g_processesMutex);
        processes.swap(g_processes);
    }

    for (const LaunchedProcess& process : processes) {
        if (WaitForSingleObject(process.process, 1500) == WAIT_TIMEOUT) {
            TerminateProcess(process.process, 0);
            WaitForSingleObject(process.process, 5000);
        }
        CloseHandle(process.process);
    }
}

RPC_STATUS StartRpcServer() {
    RPC_STATUS status = RpcServerUseProtseqEpW(
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcProtocolSequence)),
        RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr);
    if (status != RPC_S_OK) {
        return status;
    }

    status = RpcServerRegisterIf2(
        PracServiceRpc_v1_0_s_ifspec,
        nullptr,
        nullptr,
        RPC_IF_ALLOW_LOCAL_ONLY,
        RPC_C_LISTEN_MAX_CALLS_DEFAULT,
        static_cast<unsigned>(-1),
        nullptr);
    if (status != RPC_S_OK) {
        return status;
    }

    return RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, TRUE);
}

void StopRpcServer() {
    RpcMgmtStopServerListening(nullptr);
    RpcServerUnregisterIf(PracServiceRpc_v1_0_s_ifspec, nullptr, TRUE);
}

DWORD WINAPI ServiceControlHandler(
    DWORD control,
    DWORD eventType,
    LPVOID eventData,
    LPVOID) {
    if (control == SERVICE_CONTROL_INTERROGATE) {
        SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
        return NO_ERROR;
    }

    if (control == SERVICE_CONTROL_SESSIONCHANGE) {
        if (eventType == WTS_SESSION_LOGON ||
            eventType == WTS_CONSOLE_CONNECT ||
            eventType == WTS_REMOTE_CONNECT ||
            eventType == WTS_SESSION_UNLOCK) {
            const auto* notification =
                static_cast<const WTSSESSION_NOTIFICATION*>(eventData);
            if (notification != nullptr) {
                LaunchTrayForSession(notification->dwSessionId);
            }
        }
        return NO_ERROR;
    }

    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_serviceStatusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName,
        ServiceControlHandler,
        nullptr);
    if (g_serviceStatusHandle == nullptr) {
        return;
    }

    SetServiceState(SERVICE_START_PENDING, NO_ERROR, 30000);

    g_rpcStoppedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_rpcStoppedEvent == nullptr) {
        SetServiceState(SERVICE_STOPPED, GetLastError());
        return;
    }

    g_stateChangedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_stateChangedEvent == nullptr) {
        const DWORD error = GetLastError();
        CloseHandle(g_rpcStoppedEvent);
        g_rpcStoppedEvent = nullptr;
        SetServiceState(SERVICE_STOPPED, error);
        return;
    }

    const RPC_STATUS rpcStatus = StartRpcServer();
    if (rpcStatus != RPC_S_OK) {
        WriteEventLog(
            EVENTLOG_ERROR_TYPE,
            L"RPC server failed to start. RPC status: " + std::to_wstring(rpcStatus));
        CloseHandle(g_stateChangedEvent);
        g_stateChangedEvent = nullptr;
        CloseHandle(g_rpcStoppedEvent);
        g_rpcStoppedEvent = nullptr;
        SetServiceState(SERVICE_STOPPED, rpcStatus);
        return;
    }

    g_refreshThread = CreateThread(nullptr, 0, RefreshWorkerThread, nullptr, 0, nullptr);
    if (g_refreshThread == nullptr) {
        const DWORD error = GetLastError();
        StopRpcServer();
        CloseHandle(g_stateChangedEvent);
        g_stateChangedEvent = nullptr;
        CloseHandle(g_rpcStoppedEvent);
        g_rpcStoppedEvent = nullptr;
        SetServiceState(SERVICE_STOPPED, error);
        return;
    }

    SetServiceState(SERVICE_RUNNING);
    LaunchTrayForAllSessions();

    WaitForSingleObject(g_rpcStoppedEvent, INFINITE);

    SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    SignalStateChanged();
    if (g_refreshThread != nullptr) {
        WaitForSingleObject(g_refreshThread, 10000);
        CloseHandle(g_refreshThread);
        g_refreshThread = nullptr;
    }
    StopRpcServer();
    TerminateLaunchedProcesses();

    if (g_stateChangedEvent != nullptr) {
        CloseHandle(g_stateChangedEvent);
        g_stateChangedEvent = nullptr;
    }
    CloseHandle(g_rpcStoppedEvent);
    g_rpcStoppedEvent = nullptr;
    SetServiceState(SERVICE_STOPPED);
}

}  // namespace

extern "C" void StopPracService(handle_t) {
    if (g_rpcStoppedEvent != nullptr) {
        SetEvent(g_rpcStoppedEvent);
    }
}

extern "C" unsigned long GetAuthenticatedUser(
    handle_t,
    unsigned long userNameCapacity,
    wchar_t* userName,
    unsigned long* isAuthenticated) {
    if (isAuthenticated == nullptr) {
        return kPracRpcInvalidArgument;
    }

    std::wstring currentUserName;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        *isAuthenticated = g_authState.IsAuthenticated() ? 1 : 0;
        currentUserName = *isAuthenticated != 0 ? g_authState.userName : L"";
    }

    return CopyRpcString(currentUserName, userNameCapacity, userName);
}

extern "C" unsigned long LoginUser(
    handle_t,
    wchar_t* userName,
    wchar_t* password) {
    if (userName == nullptr || password == nullptr) {
        return kPracRpcInvalidArgument;
    }

    return AuthenticateUser(userName, password);
}

extern "C" unsigned long LogoutUser(handle_t) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ClearAuthAndLicenseLocked();
    }
    SignalStateChanged();
    return kPracRpcSuccess;
}

extern "C" unsigned long GetLicenseInfo(
    handle_t,
    unsigned long statusCapacity,
    wchar_t* status,
    hyper* expiresAtUnixTime,
    unsigned long* hasLicense) {
    if (expiresAtUnixTime == nullptr || hasLicense == nullptr) {
        return kPracRpcInvalidArgument;
    }

    bool authenticated = false;
    LicenseState license{};
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        authenticated = g_authState.IsAuthenticated();
        license = g_licenseState;
    }

    if (!authenticated) {
        *hasLicense = 0;
        *expiresAtUnixTime = 0;
        CopyRpcString(L"Not authenticated", statusCapacity, status);
        return kPracRpcNotAuthenticated;
    }

    unsigned long refreshResult = kPracRpcSuccess;
    if (!license.hasLicense) {
        refreshResult = FetchLicenseStatus();
        std::lock_guard<std::mutex> lock(g_stateMutex);
        license = g_licenseState;
    }

    *hasLicense = license.hasLicense ? 1 : 0;
    *expiresAtUnixTime = static_cast<hyper>(license.expiresAt);
    const unsigned long copyResult = CopyRpcString(license.status, statusCapacity, status);
    if (copyResult != kPracRpcSuccess) {
        return copyResult;
    }
    return refreshResult == kPracRpcNoLicense ? kPracRpcSuccess : refreshResult;
}

extern "C" unsigned long ActivateProduct(handle_t, wchar_t* activationCode) {
    if (activationCode == nullptr) {
        return kPracRpcInvalidArgument;
    }

    return ActivateLicense(activationCode);
}

extern "C" unsigned long CheckAntivirusFeature(handle_t) {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    if (!g_authState.IsAuthenticated()) {
        return kPracRpcNotAuthenticated;
    }
    if (!g_licenseState.hasLicense) {
        return kPracRpcNoLicense;
    }
    return kPracRpcSuccess;
}

int wmain() {
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };

    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        return static_cast<int>(GetLastError());
    }

    return 0;
}
