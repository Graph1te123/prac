#include <windows.h>
#include <rpc.h>
#include <shellapi.h>
#include <sddl.h>
#include <strsafe.h>
#include <tlhelp32.h>

#include <algorithm>
#include <string>

#include "constants.h"
#include "prac_service.h"

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kCommandOpen = 1001;
constexpr UINT kCommandExit = 1002;

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

bool StopServiceViaRpc() {
    RPC_WSTR stringBinding = nullptr;
    handle_t binding = nullptr;
    RPC_STATUS status = RpcStringBindingComposeW(
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcProtocolSequence)),
        nullptr,
        reinterpret_cast<RPC_WSTR>(const_cast<wchar_t*>(kRpcEndpoint)),
        nullptr,
        &stringBinding);
    if (status != RPC_S_OK) {
        SetLastError(status);
        return false;
    }

    status = RpcBindingFromStringBindingW(stringBinding, &binding);
    RpcStringFreeW(&stringBinding);
    if (status != RPC_S_OK) {
        SetLastError(status);
        return false;
    }

    RpcTryExcept {
        StopPracService(binding);
        status = RPC_S_OK;
    }
    RpcExcept(1) {
        status = RpcExceptionCode();
    }
    RpcEndExcept

    RpcBindingFree(&binding);

    if (status != RPC_S_OK) {
        SetLastError(status);
        return false;
    }

    return true;
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

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (g_taskbarCreatedMessage != 0 && message == g_taskbarCreatedMessage) {
        AddTrayIcon(window);
        return 0;
    }

    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kCommandOpen:
                    ShowMainWindow(window);
                    return 0;
                case kCommandExit:
                    ExitApplication(window);
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
            HDC deviceContext = BeginPaint(window, &paint);
            RECT clientRect{};
            GetClientRect(window, &clientRect);
            DrawTextW(
                deviceContext,
                kMainText,
                -1,
                &clientRect,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK);
            EndPaint(window, &paint);
            return 0;
        }

        case WM_DESTROY:
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
