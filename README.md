# Prac Tray Application

Win32 API tray application and Windows service for Windows.

## Features

- Adds an icon to the taskbar notification area on startup.
- Left-clicking the tray icon opens the main window.
- Right-clicking the tray icon opens a menu with Open and Exit commands.
- Restores the tray icon after the Windows taskbar is recreated.
- Supports hidden startup with `--background`, `--hidden`, `--minimized`,
  `/background`, or `/tray`.
- Closing the main window hides it and keeps the app running in the background.
- The main window has a File menu with an Exit command.
- Uses a named mutex to prevent multiple instances for the current user.
- The service starts the tray app in user terminal sessions, except session 0.
- The tray app starts the service when it is stopped, then exits.
- Exit commands in the tray app stop the service through Windows RPC over
  `ncalrpc`.

## Build

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executables will be created at:

- `build/Release/prac-tray.exe`
- `build/Release/prac-service.exe`

## Service

Install the service from an elevated PowerShell prompt after building:

```powershell
sc.exe create PracTrayService binPath= "C:\path\to\prac-service.exe" start= auto DisplayName= "Prac Tray Service"
sc.exe start PracTrayService
```

The service does not accept Stop or Shutdown controls from the Service Control
Manager. Use the tray application's Exit command to stop it through the RPC
interface.
