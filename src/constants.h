#pragma once

constexpr wchar_t kServiceName[] = L"PracTrayService";
constexpr wchar_t kServiceDisplayName[] = L"Prac Tray Service";
constexpr wchar_t kRpcProtocolSequence[] = L"ncalrpc";
constexpr wchar_t kRpcEndpoint[] = L"Graph1te123PracServiceRpc";
constexpr wchar_t kTrayExecutableName[] = L"prac-tray.exe";

constexpr unsigned long kPracRpcSuccess = 0;
constexpr unsigned long kPracRpcNotAuthenticated = 0xE1010001;
constexpr unsigned long kPracRpcNoLicense = 0xE1010002;
constexpr unsigned long kPracRpcNetworkError = 0xE1010003;
constexpr unsigned long kPracRpcBadResponse = 0xE1010004;
constexpr unsigned long kPracRpcInvalidArgument = 0xE1010005;
