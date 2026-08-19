# FOHMedia

FOHMedia is an open-source, cross-platform live presentation and media playback software designed for Front of House (FOH) environments. Built with a modern Qt 6 QML interface, it features robust media handling, live presentation capabilities, and native support for importing and rendering ProPresenter 7 files.

## Features
- **Cross-Platform:** Runs seamlessly on Windows, macOS, and Linux.
- **Modern UI:** Hardware-accelerated user interface built with Qt 6 QML.
- **ProPresenter Integration:** Import and parse ProPresenter 7 presentations via Protocol Buffers (Protobuf).
- **Media Playback:** High-performance media playback powered by FFmpeg.

---

## Getting Started

To build FOHMedia from source, you will need to install the core build tools and dependencies specific to your operating system. 

### Core Dependencies
- **C++20 Compiler**
- **CMake** (3.21 or newer)
- **Qt 6** (6.10 or newer)
- **Protocol Buffers (Protobuf)**
- **FFmpeg**

---

### 🍎 macOS

**1. Install Build Tools & Dependencies via Homebrew**
First, install the Xcode Command Line Tools and [Homebrew](https://brew.sh/):
```bash
xcode-select --install
brew install cmake ninja pkg-config
brew install qt@6 protobuf ffmpeg
```

**2. Build the Project**
```bash
mkdir build && cd build
cmake -G Ninja -B . -S ..
cmake --build .
```

---

### 🪟 Windows

**1. Install Build Tools**
- Install [Visual Studio 2022](https://visualstudio.microsoft.com/vs/community/) with the **"Desktop development with C++"** workload. Ensure CMake and Ninja are selected in the optional components.
- Alternatively, you can install CMake and Ninja standalone and use them from a Developer Command Prompt.

**2. Install Qt 6**
- Download the [Qt Online Installer](https://www.qt.io/download).
- Install the **Qt 6.10.x MSVC 2022 64-bit** component. *(Default path: `C:\Qt\6.10.3\msvc2022_64`)*.

**3. Install Protobuf & FFmpeg (via vcpkg)**
It is recommended to use [vcpkg](https://github.com/microsoft/vcpkg) to manage C++ dependencies on Windows:
```cmd
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install protobuf:x64-windows ffmpeg:x64-windows
```

**4. Build the Project**
When configuring CMake, point it to your Qt installation and the vcpkg toolchain. 
*Note: Make sure to adjust the paths below based on where you installed vcpkg and Qt.*
```cmd
mkdir build
cd build
cmake -G Ninja -B . -S .. -DCMAKE_TOOLCHAIN_FILE="C:\path\to\vcpkg\scripts\buildsystems\vcpkg.cmake" -DCMAKE_PREFIX_PATH="C:\Qt\6.10.3\msvc2022_64"
cmake --build .
```

---

### 🐧 Linux (Ubuntu/Debian)

**1. Install Build Tools & Dependencies**
Install the necessary compilers, CMake, and the required development libraries directly via `apt`:
```bash
sudo apt update
sudo apt install build-essential cmake ninja-build
sudo apt install qt6-base-dev qt6-declarative-dev qt6-multimedia-dev
sudo apt install libprotobuf-dev protobuf-compiler
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libavdevice-dev
```

**2. Build the Project**
```bash
mkdir build && cd build
cmake -G Ninja -B . -S ..
cmake --build .
```

---

## License

FOHMedia is licensed under the [GNU General Public License v3.0 (GPLv3)](LICENSE.txt). 

This project also relies on third-party open-source libraries, including Qt 6, Protocol Buffers, and FFmpeg, which are distributed under their respective licenses. See the `LICENSE.txt` file for full details.
