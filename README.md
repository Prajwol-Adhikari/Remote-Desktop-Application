# **Remote Desktop Application**

A high-performance, low-latency C++ Remote Desktop Application built with native **Win32 API**, **Winsock2**, **OpenGL 3**, and **Dear ImGui**.

This workspace provides multi-client remote desktop hosting, real-time screen frame streaming, remote mouse/keyboard input injection, UDP network discovery, and an in-app messaging system.

---

## Key Features

* **Real-Time Screen Share**: Threaded screen capture and socket transmission for low-latency desktop streaming.
* **Remote Input Control**: Native Win32 input injection allowing hosts to delegate mouse and keyboard control to connected clients.
* **Multi-Client Architecture**: Robust server handling supporting multiple concurrent client nodes with individual access control.
* **Permission Management**: Host controls client permissions on the fly (viewing rights, input control).
* **Automatic Server Discovery**: Built-in UDP broadcasting service for auto-discovering hosts on local networks.
* **In-App Messaging**: Full-duplex chat pipeline between server and connected clients.
* **Hardware-Accelerated UI**: Custom GUI rendered using **Dear ImGui** over an **OpenGL 3 / WGL** graphics context.

---

##  Project Structure

```text
├── dependencies/          # Vendored ImGui and graphics library headers
│   ├── imgui.h
│   ├── imgui_impl_opengl3.h
│   └── imgui_impl_win32.h
├── inputInjector.h/.cpp   # Win32 remote input injection logic
├── networkManager.h/.cpp # Winsock TCP/UDP networking engine & protocol
├── screenCapture.h/.cpp   # Desktop frame capture pipeline
├── uiRenderer.h/.cpp      # Dear ImGui interface & workspace controls
├── main.cpp               # Win32 window initialization & OpenGL render loop
└── .gitignore             # Git ignore patterns for C++ binaries
```

---

## Tech Stack

* **Language:** C++17
* **GUI Framework:** Dear ImGui
* **Graphics API:** OpenGL 3.0 / WGL (GLEW extension loading)
* **Networking:** Native Windows Sockets (`Winsock2` / `Ws2_32.lib`)
* **Platform API:** Windows Win32 API (`User32.lib`, `Gdi32.lib`)

---

## Prerequisites & Building

### Requirements
* **Operating System:** Windows 10 / 11
* **Compiler:** MSVC (Visual Studio 2019/2022) or MinGW GCC with C++17 support
* **System Libraries:** `Ws2_32.lib`, `Opengl32.lib`, `Gdi32.lib`

### Quick Build (Command Line / MinGW GCC)

```bash
g++ -std=c++17 main.cpp inputInjector.cpp networkManager.cpp screenCapture.cpp uiRenderer.cpp \
    dependencies/imgui*.cpp -I. -lws2_32 -lopengl32 -lgdi32 -lglew32 -o RemoteDesktopApp.exe
```

---

## Getting Started

### 1. Clone the Repository
```bash
git clone [https://github.com/Prajwol-Adhikari/Remote-Desktop-Application.git](https://github.com/Prajwol-Adhikari/Remote-Desktop-Application.git)
cd Remote-Desktop-Application
```

### 2. Run as Host (Server)
1. Start the application and initialize the server on your desired port.
2. Enable **UDP Broadcast** so local clients can locate your machine automatically.

### 3. Run as Client
1. Search for active servers using the **UDP Discovery** panel or enter the host's IP address directly.
2. Connect and request screen viewing / input control permissions from the server host.