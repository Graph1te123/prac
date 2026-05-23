#include <windows.h>
#include <rpc.h>
#include <shellapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include <algorithm>
#include <ctime>
#include <string>

#include "constants.h"
#include "prac_service.h"

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kCommandOpen = 1001;
constexpr UINT kCommandExit = 1002;
constexpr UINT kCommandLogin = 1003;
constexpr UINT kCommandActivate = 1004;
constexpr UINT kCommandLogout = 1005;
constexpr UINT kPollTimerId = 1;
constexpr UINT kPollIntervalMs = 30000;
constexpr int kTextCapacity = 260;

const wchar_t kWindowClassName[] = L"PracTrayWindowClass";
const wchar_t kWindowTitle[] = L"Prac Tray Application";
const wchar_t kTrayTip[] = L"Prac Tray Application";
const wchar_t kOpenLabel[] = L"\x041e\x0442\x043a\x0440\x044b\x0442\x044c";
const wchar_t kExitLabel[] = L"\x0412\x044b\x0445\x043e\x0434";
const wchar_t kFileLabel[] = L"\x0424\x0430\x0439\x043b";
const wchar_t kMainText[] =
    L"Prac tray application is running.\n"
    L"Close this window to keep the app in the background.";

HINSTANCE g_instance = nullptr;
UINT g_taskbarCreatedMessage = 0;
HANDLE g_singleInstanceMutex = nullptr;
bool g_isExiting = false;

struct ClientState {
    bool authenticated = false;
    bool hasLicense = false;
    std::wstring userName;
    std::wstring licenseStatus = L"No active license";
    __int64 licenseExpiresAt = 0;
    std::wstring error;
};

ClientState g_clientState;
HWND g_statusLabel = nullptr;
HWND g_userLabel = nullptr;
HWND g_antivirusLabel = nullptr;
HWND g_loginLabel = nullptr;
HWND g_loginEdit = nullptr;
HWND g_passwordLabel = nullptr;
HWND g_passwordEdit = nullptr;
HWND g_loginButton = nullptr;
HWND g_activationLabel = nullptr;
HWND g_activationEdit = nullptr;
HWND g_activationButton = nullptr;
HWND g_licenseLabel = nullptr;
HWND g_logoutButton = nullptr;

enum class StartupDecision {
    Continue,
    ExitSuccess,
    ExitFailure,
};

struct ScopedServiceHandle {
    SC_HANDLE value = nullptr;

    ScopedServiceHandle() = default;
    explicit ScopedServiceHandle(SC_HANDLE handle) : value(handle) {}
    ScopedServiceHandle(const ScopedServiceHandle&) = delete;
    ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;

    ~ScopedServiceHandle() {
        reset();
    }

    SC_HANDLE get() const {
        return value;
    }

    void reset(SC_HANDLE handle = nullptr) {
        if (value != nullptr) {
            CloseServiceHandle(value);
        }
        value = handle;
    }
};

void ShowLastErrorMessage(const wchar_t* title) {
    const DWORD error = GetLastError();
    if (error == ERROR_SUCCESS) {
        return;
    }

    wchar_t* message = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);

    if (size != 0 && message != nullptr) {
        MessageBoxW(nullptr, message, title, MB_ICONERROR | MB_OK);
        LocalFree(message);
    }
}

bool QueryServiceStatusProcess(SC_HANDLE service, SERVICE_STATUS_PROCESS& status) {
    DWORD bytesNeeded = 0;
    return QueryServiceStatusEx(
               service,
               SC_STATUS_PROCESS_INFO,
               reinterpret_cast<LPBYTE>(&status),
               sizeof(status),
               &bytesNeeded) != FALSE;
}

bool WaitForServiceState(SC_HANDLE service, DWORD desiredState, DWORD timeoutMs) {
    const DWORD startTick = GetTickCount();

    for (;;) {
        SERVICE_STATUS_PROCESS status{};
        if (!QueryServiceStatusProcess(service, status)) {
            return false;
        }

        if (status.dwCurrentState == desiredState) {
            return true;
        }

        if (status.dwCurrentState == SERVICE_STOPPED && desiredState != SERVICE_STOPPED) {
            SetLastError(status.dwWin32ExitCode);
            return false;
        }

        if (GetTickCount() - startTick >= timeoutMs) {
            SetLastError(ERROR_TIMEOUT);
            return false;
        }

        DWORD waitTime = status.dwWaitHint / 10;
        waitTime = std::max<DWORD>(250, std::min<DWORD>(waitTime, 1000));
        Sleep(waitTime);
    }
}

StartupDecision EnsureServiceIsRunning() {
    ScopedServiceHandle serviceManager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (serviceManager.get() == nullptr) {
        ShowLastErrorMessage(L"Open service manager failed");
        return StartupDecision::ExitFailure;
    }

    ScopedServiceHandle service(OpenServiceW(
        serviceManager.get(),
        kServiceName,
        SERVICE_QUERY_STATUS | SERVICE_START));
    if (service.get() == nullptr) {
        ShowLastErrorMessage(L"Open service failed");
        return StartupDecision::ExitFailure;
    }

    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusProcess(service.get(), status)) {
        ShowLastErrorMessage(L"Query service status failed");
        return StartupDecision::ExitFailure;
    }

    if (status.dwCurrentState == SERVICE_RUNNING) {
        return StartupDecision::Continue;
    }

    if (status.dwCurrentState == SERVICE_STOPPED) {
        if (!StartServiceW(service.get(), 0, nullptr) &&
            GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            ShowLastErrorMessage(L"Start service failed");
            return StartupDecision::ExitFailure;
        }
    }

    if (!WaitForServiceState(service.get(), SERVICE_RUNNING, 30000)) {
        ShowLastErrorMessage(L"Wait for service failed");
        return StartupDecision::ExitFailure;
    }

    return StartupDecision::ExitSuccess;
}

bool QueryServiceProcessId(DWORD& processId) {
    ScopedServiceHandle serviceManager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (serviceManager.get() == nullptr) {
        return false;
    }

    ScopedServiceHandle service(OpenServiceW(
        serviceManager.get(),
        kServiceName,
        SERVICE_QUERY_STATUS));
    if (service.get() == nullptr) {
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    if (!QueryServiceStatusProcess(service.get(), status) ||
        status.dwCurrentState != SERVICE_RUNNING ||
        status.dwProcessId == 0) {
        return false;
    }

    processId = status.dwProcessId;
    return true;
}

DWORD GetParentProcessId(DWORD processId) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W processEntry{};
    processEntry.dwSize = sizeof(processEntry);

    DWORD parentProcessId = 0;
    if (Process32FirstW(snapshot, &processEntry)) {
        do {
            if (processEntry.th32ProcessID == processId) {
                parentProcessId = processEntry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &processEntry));
    }

    CloseHandle(snapshot);
    return parentProcessId;
}

bool IsStartedByService() {
    DWORD serviceProcessId = 0;
    if (!QueryServiceProcessId(serviceProcessId)) {
        return false;
    }

    return GetParentProcessId(GetCurrentProcessId()) == serviceProcessId;
}

RPC_STATUS CreateServiceBinding(handle_t* binding) {
    if (binding == nullptr) {
        return RPC_S_INVALID_ARG;
    }

    RPC_WSTR stringBinding = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcProtocolSequence)),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr,
        &stringBinding);
    if (status != RPC_S_OK) {
        return status;
    }

    status = RpcBindingFromStringBindingW(stringBinding, binding);
    RpcStringFreeW(&stringBinding);
    return status;
}

unsigned long RpcStopServiceRaw(handle_t binding) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        StopPracService(binding);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

bool StopServiceViaRpc() {
    handle_t binding = nullptr;
    RPC_STATUS status = CreateServiceBinding(&binding);
    if (status != RPC_S_OK) {
        SetLastError(status);
        return false;
    }

    const unsigned long result = RpcStopServiceRaw(binding);
    RpcBindingFree(&binding);
    if (result != kPracRpcSuccess) {
        SetLastError(result);
        return false;
    }
    return true;
}

unsigned long RpcGetAuthenticatedUserRaw(
    handle_t binding,
    unsigned long capacity,
    wchar_t* userName,
    unsigned long* isAuthenticated) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        result = GetAuthenticatedUser(binding, capacity, userName, isAuthenticated);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

unsigned long RpcLoginUserRaw(handle_t binding, wchar_t* userName, wchar_t* password) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        result = LoginUser(binding, userName, password);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

unsigned long RpcLogoutUserRaw(handle_t binding) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        result = LogoutUser(binding);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

unsigned long RpcGetLicenseInfoRaw(
    handle_t binding,
    unsigned long capacity,
    wchar_t* status,
    hyper* expiresAtUnixTime,
    unsigned long* hasLicense) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        result = GetLicenseInfo(binding, capacity, status, expiresAtUnixTime, hasLicense);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

unsigned long RpcActivateProductRaw(handle_t binding, wchar_t* activationCode) {
    unsigned long result = kPracRpcSuccess;
    RpcTryExcept {
        result = ActivateProduct(binding, activationCode);
    }
    RpcExcept(1) {
        result = RpcExceptionCode();
    }
    RpcEndExcept
    return result;
}

std::wstring RpcStatusMessage(unsigned long status) {
    switch (status) {
        case kPracRpcSuccess:
            return L"Success";
        case kPracRpcNotAuthenticated:
            return L"User is not authenticated.";
        case kPracRpcNoLicense:
            return L"No active license.";
        case kPracRpcNetworkError:
            return L"Network error while contacting the web service.";
        case kPracRpcBadResponse:
            return L"Unexpected response from the web service.";
        case kPracRpcInvalidArgument:
            return L"Required value is empty or invalid.";
        default:
            return L"Operation failed. Code: " + std::to_wstring(status);
    }
}

std::wstring RpcBindingStatusMessage(RPC_STATUS status) {
    return L"RPC connection failed. Code: " + std::to_wstring(status);
}

std::wstring GetWindowTextString(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(length + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
    }
    text.resize(length);
    return text;
}

std::wstring FormatUnixTime(__int64 unixTime) {
    if (unixTime <= 0) {
        return L"Unknown";
    }

    const time_t timeValue = static_cast<time_t>(unixTime);
    tm localTime{};
    if (localtime_s(&localTime, &timeValue) != 0) {
        return L"Unknown";
    }

    wchar_t buffer[64]{};
    if (wcsftime(buffer, ARRAYSIZE(buffer), L"%Y-%m-%d %H:%M", &localTime) == 0) {
        return L"Unknown";
    }
    return buffer;
}

void RefreshClientStateFromService() {
    ClientState nextState{};

    handle_t binding = nullptr;
    const RPC_STATUS bindingStatus = CreateServiceBinding(&binding);
    if (bindingStatus != RPC_S_OK) {
        nextState.error = RpcBindingStatusMessage(bindingStatus);
        g_clientState = nextState;
        return;
    }

    wchar_t userName[kTextCapacity]{};
    unsigned long isAuthenticated = 0;
    unsigned long result = RpcGetAuthenticatedUserRaw(
        binding,
        ARRAYSIZE(userName),
        userName,
        &isAuthenticated);
    if (result != kPracRpcSuccess) {
        nextState.error = RpcStatusMessage(result);
        RpcBindingFree(&binding);
        g_clientState = nextState;
        return;
    }

    nextState.authenticated = isAuthenticated != 0;
    nextState.userName = userName;

    if (nextState.authenticated) {
        wchar_t licenseStatus[kTextCapacity]{};
        hyper expiresAt = 0;
        unsigned long hasLicense = 0;
        result = RpcGetLicenseInfoRaw(
            binding,
            ARRAYSIZE(licenseStatus),
            licenseStatus,
            &expiresAt,
            &hasLicense);
        nextState.hasLicense = hasLicense != 0;
        nextState.licenseStatus = licenseStatus;
        nextState.licenseExpiresAt = expiresAt;
        if (result != kPracRpcSuccess && result != kPracRpcNoLicense) {
            nextState.error = RpcStatusMessage(result);
        }
    }

    RpcBindingFree(&binding);
    g_clientState = nextState;
}

void LoginWithService(HWND window) {
    std::wstring userName = GetWindowTextString(g_loginEdit);
    std::wstring password = GetWindowTextString(g_passwordEdit);

    handle_t binding = nullptr;
    const RPC_STATUS bindingStatus = CreateServiceBinding(&binding);
    if (bindingStatus != RPC_S_OK) {
        g_clientState.error = RpcBindingStatusMessage(bindingStatus);
        InvalidateRect(window, nullptr, TRUE);
        return;
    }

    const unsigned long result =
        RpcLoginUserRaw(binding, userName.data(), password.data());
    RpcBindingFree(&binding);

    if (result != kPracRpcSuccess) {
        g_clientState.authenticated = false;
        g_clientState.hasLicense = false;
        g_clientState.error = RpcStatusMessage(result);
    } else {
        SetWindowTextW(g_passwordEdit, L"");
        RefreshClientStateFromService();
    }
}

void ActivateWithService(HWND window) {
    std::wstring activationCode = GetWindowTextString(g_activationEdit);

    handle_t binding = nullptr;
    const RPC_STATUS bindingStatus = CreateServiceBinding(&binding);
    if (bindingStatus != RPC_S_OK) {
        g_clientState.error = RpcBindingStatusMessage(bindingStatus);
        InvalidateRect(window, nullptr, TRUE);
        return;
    }

    const unsigned long result = RpcActivateProductRaw(binding, activationCode.data());
    RpcBindingFree(&binding);

    if (result != kPracRpcSuccess) {
        g_clientState.hasLicense = false;
        g_clientState.error = RpcStatusMessage(result);
    } else {
        SetWindowTextW(g_activationEdit, L"");
        RefreshClientStateFromService();
    }
}

void LogoutWithService() {
    handle_t binding = nullptr;
    const RPC_STATUS bindingStatus = CreateServiceBinding(&binding);
    if (bindingStatus != RPC_S_OK) {
        g_clientState.error = RpcBindingStatusMessage(bindingStatus);
        return;
    }

    const unsigned long result = RpcLogoutUserRaw(binding);
    RpcBindingFree(&binding);
    if (result != kPracRpcSuccess) {
        g_clientState.error = RpcStatusMessage(result);
        return;
    }

    g_clientState = ClientState{};
}

std::wstring GetCurrentUserSidString() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return L"unknown-user";
    }

    DWORD tokenInfoLength = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &tokenInfoLength);
    if (tokenInfoLength == 0) {
        CloseHandle(token);
        return L"unknown-user";
    }

    std::wstring result = L"unknown-user";
    auto* buffer = static_cast<TOKEN_USER*>(LocalAlloc(LPTR, tokenInfoLength));
    if (buffer != nullptr &&
        GetTokenInformation(token, TokenUser, buffer, tokenInfoLength, &tokenInfoLength)) {
        LPWSTR sidString = nullptr;
        if (ConvertSidToStringSidW(buffer->User.Sid, &sidString) && sidString != nullptr) {
            result = sidString;
            LocalFree(sidString);
        }
    }

    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    CloseHandle(token);
    return result;
}

bool EnsureSingleInstance() {
    const std::wstring mutexName =
        L"Global\\Graph1te123PracTrayApp." + GetCurrentUserSidString();

    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (g_singleInstanceMutex == nullptr) {
        return false;
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        return false;
    }

    return true;
}

bool ShouldStartHidden() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return false;
    }

    bool hidden = false;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], L"--background") == 0 ||
            lstrcmpiW(argv[i], L"--hidden") == 0 ||
            lstrcmpiW(argv[i], L"--minimized") == 0 ||
            lstrcmpiW(argv[i], L"/background") == 0 ||
            lstrcmpiW(argv[i], L"/tray") == 0) {
            hidden = true;
            break;
        }
    }

    LocalFree(argv);
    return hidden;
}

NOTIFYICONDATAW CreateTrayIconData(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    StringCchCopyW(data.szTip, ARRAYSIZE(data.szTip), kTrayTip);
    return data;
}

bool AddTrayIcon(HWND window) {
    NOTIFYICONDATAW data = CreateTrayIconData(window);
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        return false;
    }

    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    return true;
}

void RemoveTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowMainWindow(HWND window) {
    RefreshClientStateFromService();
    ApplyClientStateToControls(window);

    if (IsIconic(window)) {
        ShowWindow(window, SW_RESTORE);
    } else {
        ShowWindow(window, SW_SHOWNORMAL);
    }

    SetForegroundWindow(window);
}

void ExitApplication(HWND window) {
    if (!StopServiceViaRpc()) {
        ShowLastErrorMessage(L"Stop service failed");
    }
    g_isExiting = true;
    DestroyWindow(window);
}

void ShowTrayMenu(HWND window) {
    POINT cursorPosition{};
    GetCursorPos(&cursorPosition);

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kCommandOpen, kOpenLabel);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCommandExit, kExitLabel);

    SetForegroundWindow(window);
    TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
        cursorPosition.x,
        cursorPosition.y,
        0,
        window,
        nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

HMENU CreateMainMenu() {
    HMENU menu = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    if (menu == nullptr || fileMenu == nullptr) {
        if (fileMenu != nullptr) {
            DestroyMenu(fileMenu);
        }
        if (menu != nullptr) {
            DestroyMenu(menu);
        }
        return nullptr;
    }

    AppendMenuW(fileMenu, MF_STRING, kCommandExit, kExitLabel);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), kFileLabel);
    return menu;
}

HWND CreateChild(
    HWND parent,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int id = 0) {
    return CreateWindowExW(
        0,
        className,
        text,
        WS_CHILD | style,
        0,
        0,
        0,
        0,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance,
        nullptr);
}

void SetControlVisible(HWND control, bool visible) {
    if (control != nullptr) {
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

void LayoutControls(HWND window) {
    if (g_statusLabel == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);

    const int margin = 24;
    const int width = std::max(260, client.right - client.left - margin * 2);
    int y = 24;

    MoveWindow(g_statusLabel, margin, y, width, 44, TRUE);
    y += 52;
    MoveWindow(g_userLabel, margin, y, width, 24, TRUE);
    y += 30;
    MoveWindow(g_antivirusLabel, margin, y, width, 24, TRUE);
    y += 42;

    MoveWindow(g_loginLabel, margin, y, width, 22, TRUE);
    y += 26;
    MoveWindow(g_loginEdit, margin, y, 280, 24, TRUE);
    y += 34;
    MoveWindow(g_passwordLabel, margin, y, width, 22, TRUE);
    y += 26;
    MoveWindow(g_passwordEdit, margin, y, 280, 24, TRUE);
    y += 36;
    MoveWindow(g_loginButton, margin, y, 120, 30, TRUE);

    MoveWindow(g_activationLabel, margin, y - 122, width, 22, TRUE);
    MoveWindow(g_activationEdit, margin, y - 92, 280, 24, TRUE);
    MoveWindow(g_activationButton, margin, y - 56, 120, 30, TRUE);

    MoveWindow(g_licenseLabel, margin, y - 122, width, 52, TRUE);
    MoveWindow(g_logoutButton, margin, client.bottom - 54, 120, 30, TRUE);
}

void ApplyClientStateToControls(HWND window) {
    if (g_statusLabel == nullptr) {
        return;
    }

    std::wstring statusText;
    if (!g_clientState.error.empty()) {
        statusText = g_clientState.error;
    } else if (!g_clientState.authenticated) {
        statusText = L"Authentication is required.";
    } else if (!g_clientState.hasLicense) {
        statusText = L"Product activation is required.";
    } else {
        statusText = L"Product is active.";
    }

    SetWindowTextW(g_statusLabel, statusText.c_str());

    const std::wstring userText =
        g_clientState.authenticated ? L"User: " + g_clientState.userName : L"User: none";
    SetWindowTextW(g_userLabel, userText.c_str());

    const std::wstring antivirusText = g_clientState.hasLicense
                                           ? L"Antivirus functionality: unlocked"
                                           : L"Antivirus functionality: blocked";
    SetWindowTextW(g_antivirusLabel, antivirusText.c_str());

    const std::wstring licenseText =
        L"License: " + g_clientState.licenseStatus +
        L"\r\nExpires: " + FormatUnixTime(g_clientState.licenseExpiresAt);
    SetWindowTextW(g_licenseLabel, licenseText.c_str());

    const bool showLogin = !g_clientState.authenticated;
    const bool showActivation = g_clientState.authenticated && !g_clientState.hasLicense;
    const bool showLicense = g_clientState.authenticated && g_clientState.hasLicense;

    SetControlVisible(g_userLabel, g_clientState.authenticated);
    SetControlVisible(g_loginLabel, showLogin);
    SetControlVisible(g_loginEdit, showLogin);
    SetControlVisible(g_passwordLabel, showLogin);
    SetControlVisible(g_passwordEdit, showLogin);
    SetControlVisible(g_loginButton, showLogin);
    SetControlVisible(g_activationLabel, showActivation);
    SetControlVisible(g_activationEdit, showActivation);
    SetControlVisible(g_activationButton, showActivation);
    SetControlVisible(g_licenseLabel, showLicense);
    SetControlVisible(g_logoutButton, g_clientState.authenticated);

    LayoutControls(window);
}

void CreateMainControls(HWND window) {
    g_statusLabel = CreateChild(window, L"STATIC", L"", WS_VISIBLE | SS_LEFT);
    g_userLabel = CreateChild(window, L"STATIC", L"", WS_VISIBLE | SS_LEFT);
    g_antivirusLabel = CreateChild(window, L"STATIC", L"", WS_VISIBLE | SS_LEFT);
    g_loginLabel = CreateChild(window, L"STATIC", L"Login", SS_LEFT);
    g_loginEdit = CreateChild(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    g_passwordLabel = CreateChild(window, L"STATIC", L"Password", SS_LEFT);
    g_passwordEdit =
        CreateChild(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD);
    g_loginButton = CreateChild(window, L"BUTTON", L"Sign in", WS_TABSTOP, kCommandLogin);
    g_activationLabel =
        CreateChild(window, L"STATIC", L"Activation code", SS_LEFT);
    g_activationEdit = CreateChild(window, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    g_activationButton =
        CreateChild(window, L"BUTTON", L"Activate", WS_TABSTOP, kCommandActivate);
    g_licenseLabel = CreateChild(window, L"STATIC", L"", SS_LEFT);
    g_logoutButton = CreateChild(window, L"BUTTON", L"Logout", WS_TABSTOP, kCommandLogout);

    RefreshClientStateFromService();
    ApplyClientStateToControls(window);
    SetTimer(window, kPollTimerId, kPollIntervalMs, nullptr);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
        AddTrayIcon(window);
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            CreateMainControls(window);
            return 0;

        case WM_SIZE:
            LayoutControls(window);
            return 0;

        case WM_TIMER:
            if (wParam == kPollTimerId) {
                RefreshClientStateFromService();
                ApplyClientStateToControls(window);
                return 0;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kCommandOpen:
                    ShowMainWindow(window);
                    return 0;
                case kCommandExit:
                    ExitApplication(window);
                    return 0;
                case kCommandLogin:
                    LoginWithService(window);
                    ApplyClientStateToControls(window);
                    return 0;
                case kCommandActivate:
                    ActivateWithService(window);
                    ApplyClientStateToControls(window);
                    return 0;
                case kCommandLogout:
                    LogoutWithService();
                    ApplyClientStateToControls(window);
                    return 0;
                default:
                    break;
            }
            break;

        case kTrayCallbackMessage:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ShowMainWindow(window);
                    return 0;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU:
                    ShowTrayMenu(window);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            if (g_isExiting) {
                DestroyWindow(window);
            } else {
                ShowWindow(window, SW_HIDE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_DESTROY:
            KillTimer(window, kPollTimerId);
            RemoveTrayIcon(window);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool RegisterMainWindowClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = g_instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kWindowClassName;
    windowClass.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);

    return RegisterClassExW(&windowClass) != 0;
}

HWND CreateMainWindow() {
    HWND window = CreateWindowExW(
        0,
        kWindowClassName,
        kWindowTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        400,
        nullptr,
        nullptr,
        g_instance,
        nullptr);

    if (window == nullptr) {
        return nullptr;
    }

    HMENU menu = CreateMainMenu();
    if (menu != nullptr) {
        SetMenu(window, menu);
    }

    return window;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    g_instance = instance;
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    const StartupDecision serviceDecision = EnsureServiceIsRunning();
    if (serviceDecision == StartupDecision::ExitSuccess) {
        return 0;
    }
    if (serviceDecision == StartupDecision::ExitFailure) {
        return 1;
    }

    if (!IsStartedByService()) {
        return 0;
    }

    if (!EnsureSingleInstance()) {
        return 0;
    }

    if (!RegisterMainWindowClass()) {
        ShowLastErrorMessage(L"Register window class failed");
        return 1;
    }

    HWND window = CreateMainWindow();
    if (window == nullptr) {
        ShowLastErrorMessage(L"Create window failed");
        return 1;
    }

    if (!AddTrayIcon(window)) {
        ShowLastErrorMessage(L"Add tray icon failed");
        DestroyWindow(window);
        return 1;
    }

    if (!ShouldStartHidden()) {
        ShowWindow(window, showCommand);
        UpdateWindow(window);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_singleInstanceMutex != nullptr) {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }

    return static_cast<int>(message.wParam);
}
