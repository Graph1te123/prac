#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "constants.h"

namespace {

    struct ScopedHandle {
        HANDLE value = nullptr;

        ScopedHandle() = default;

        explicit ScopedHandle(HANDLE handle)
            : value(handle) {}

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

    SERVICE_STATUS_HANDLE g_serviceStatusHandle = nullptr;
    SERVICE_STATUS g_serviceStatus{};
    DWORD g_checkpoint = 1;
    HANDLE g_stopEvent = nullptr;

    std::mutex g_processesMutex;
    std::vector<LaunchedProcess> g_processes;

    std::wstring GetLastErrorMessage(DWORD error) {
        wchar_t* buffer = nullptr;

        const DWORD size = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        std::wstring message =
            size != 0 && buffer != nullptr ? buffer : L"Unknown error";

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

        LPCWSTR strings[] = { message.c_str() };

        ReportEventW(
            source,
            type,
            0,
            0,
            nullptr,
            1,
            0,
            strings,
            nullptr);

        DeregisterEventSource(source);
    }

    void SetServiceState(DWORD state, DWORD win32ExitCode = NO_ERROR, DWORD waitHint = 0) {
        g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_serviceStatus.dwCurrentState = state;
        g_serviceStatus.dwWin32ExitCode = win32ExitCode;
        g_serviceStatus.dwWaitHint = waitHint;

        if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
            g_serviceStatus.dwControlsAccepted = 0;
            g_serviceStatus.dwCheckPoint = g_checkpoint++;
        }
        else {
            g_serviceStatus.dwControlsAccepted =
                state == SERVICE_RUNNING
                ? SERVICE_ACCEPT_STOP |
                SERVICE_ACCEPT_SHUTDOWN |
                SERVICE_ACCEPT_SESSIONCHANGE
                : 0;

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
            length = GetModuleFileNameW(
                nullptr,
                path.data(),
                static_cast<DWORD>(path.size()));

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

        if (!directory.empty() &&
            directory.back() != L'\\' &&
            directory.back() != L'/') {
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
            }
            else {
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

            g_processes.push_back({
                sessionId,
                processInfo.dwProcessId,
                processInfo.hProcess,
                });
        }

        return true;
    }

    void LaunchTrayForAllSessions() {
        WTS_SESSION_INFOW* sessions = nullptr;
        DWORD sessionCount = 0;

        if (!WTSEnumerateSessionsW(
            WTS_CURRENT_SERVER_HANDLE,
            0,
            1,
            &sessions,
            &sessionCount)) {
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

    DWORD WINAPI ServiceControlHandler(
        DWORD control,
        DWORD eventType,
        LPVOID eventData,
        LPVOID) {
        if (control == SERVICE_CONTROL_INTERROGATE) {
            SetServiceStatus(g_serviceStatusHandle, &g_serviceStatus);
            return NO_ERROR;
        }

        if (control == SERVICE_CONTROL_STOP ||
            control == SERVICE_CONTROL_SHUTDOWN) {
            SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 30000);

            if (g_stopEvent != nullptr) {
                SetEvent(g_stopEvent);
            }

            return NO_ERROR;
        }

        if (control == SERVICE_CONTROL_SESSIONCHANGE) {
            if (eventType == WTS_SESSION_LOGON ||
                eventType == WTS_CONSOLE_CONNECT ||
                eventType == WTS_REMOTE_CONNECT ||
                eventType == WTS_SESSION_UNLOCK) {
                const auto* notification =
                    static_cast<WTSSESSION_NOTIFICATION*>(eventData);

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

        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (g_stopEvent == nullptr) {
            SetServiceState(SERVICE_STOPPED, GetLastError());
            return;
        }

        SetServiceState(SERVICE_RUNNING);

        LaunchTrayForAllSessions();

        WaitForSingleObject(g_stopEvent, INFINITE);

        SetServiceState(SERVICE_STOP_PENDING, NO_ERROR, 30000);

        TerminateLaunchedProcesses();

        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;

        SetServiceState(SERVICE_STOPPED);
    }

} // namespace

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