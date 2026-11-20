# Virtual Display (OBS) Architecture & Multi-Platform Handoff Guide

**Branch:** `feature/virt-disp-arch-refactor`  
**Target Platforms:**
- **macOS (Primary Production Target: MacBook Pro M5, macOS 27)** — Syphon (`IOSurface` + Metal)
- **Linux (Ubuntu 26.04, Intel Core Ultra 9 + Arc GPU)** — PipeWire (`pw_stream` + `SPA_VIDEO_FORMAT_RGBA`/`BGRA`)
- **Windows 11** — Spout2 (`ID3D11Texture2D` with `D3D11_RESOURCE_MISC_SHARED` + DXGI Shared Handle)
- **Future Hardware Target:** Blackmagic DeckLink SDI output (External Keying / Fill + Key via `bmdFormat8BitARGB` / `BGRA`)

---

## 1. Feature Goal & Core Requirements

1. **Operator Workflow (`Displays` View)**:
   - In FOHMedia's `Displays` view (`qml/DisplaysView.qml`), operators can add or configure any logical screen (e.g., `Audience`, `Overlay`, `Stage`) and assign its `Display:` dropdown to **`"Virtual Display (OBS)"`** (`ScreenConfig::VirtualObsDisplay = -3`) instead of a physical monitor (`0..N-1`) or desktop window (`Unassigned = -1`).
   - When assigned to `"Virtual Display (OBS)"`, the `Geometry` (`width` x `height`) SpinBoxes remain editable (e.g. `1920x1080`, `3840x2160`), the `Fullscreen` checkbox is hidden, and a label displays: `OBS Source: FOHMedia - <ScreenName> (RGBA)`.
   - When Presentation mode is started, **no visible window appears on the desktop**. Instead, the screen renders headlessly at 60 FPS and streams directly to OBS Studio running on the same host.
2. **No NDI / Local Host Only**:
   - Explicitly avoids NDI. Uses native OS/GPU local inter-process video/texture sharing mechanisms.
3. **Full 8-bit Alpha Channel (Transparency) Support**:
   - Must always preserve the 8-bit Alpha channel (`RGBA8` / `BGRA8`) so transparent layouts (`isTransparentLayout` in `qml/LiveOutputWindow.qml`, such as lower-third lyrics overlays) composite cleanly in OBS without chroma-keying.
   - This same offscreen RGBA8/BGRA8 pipeline is designed around `IVirtualFrameSink` so that future **Blackmagic DeckLink SDI cards** can plug in as a `DeckLinkFrameSink` and output hardware **Fill + Key** signals over SDI.

---

## 2. What Has Already Been Implemented & Verified (Linux + Core Engine + Step 1)

### A. Data Model & UI (`inc/ScreenModel.h`, `src/ScreenModel.cpp`, `qml/DisplaysView.qml`)
- Added constants to `ScreenConfig` in `inc/ScreenModel.h`:
  - `ScreenConfig::UnassignedDisplay = -1`
  - `ScreenConfig::DisabledDisplay   = -2`
  - `ScreenConfig::VirtualObsDisplay = -3`
  - Helper: `bool isVirtualObsDisplay() const { return hardwareDisplayIndex == VirtualObsDisplay; }`
- `ScreenModel::getHardwareDisplays()` returns `["Unassigned", "Virtual Display (OBS)", "Display 1 (WxH)", ...]`.
- Added `Q_INVOKABLE int comboIndexForHardwareDisplay(int hardwareDisplayIndex) const` and `Q_INVOKABLE int hardwareDisplayForComboIndex(int comboIndex) const` for clean bidirectional mapping in `qml/DisplaysView.qml`:
  - `comboIndex 0` <-> `hardwareDisplayIndex -1` (`"Unassigned"`)
  - `comboIndex 1` <-> `hardwareDisplayIndex -3` (`"Virtual Display (OBS)"`)
  - `comboIndex >= 2` <-> `hardwareDisplayIndex = comboIndex - 2` (`"Display 1..."`)

### B. DisplayEngine Integration & Cursor Hiding Fix (`inc/DisplayEngine.h`, `src/DisplayEngine.cpp`)
- `DisplayEngine::ScreenViewInfo` holds either:
  - `QPointer<QQuickView> view` (for physical/windowed displays), or
  - `std::shared_ptr<VirtualDisplayOutput> virtualOutput` (for `hardwareDisplayIndex == -3`).
  - Helper `QQuickItem* rootObject() const` returns the root QML item from whichever is active.
- **Synchronous Slide Transition Updates (Eliminates Keystroke-to-OBS Latency)**:
  - In `qml/LiveOutputWindow.qml`, property change handlers (`onSlideTextChanged`, `onCurrentSlideIndexChanged`, etc.) coalesce via `scheduleCheckTransition()`.
  - In `DisplayEngine::applyAudienceUpdate()` and `DisplayEngine::updateQmlContext()`, after setting all slide properties (`layoutName`, `slideText`, `nextSlideText`, `transitionType`, `transitionDurationMs`, `globalBackgroundMedia`, `foregroundMedia`, `currentSlideIndex`), `DisplayEngine` immediately calls:
    ```cpp
    QMetaObject::invokeMethod(root, "checkTransition", Qt::DirectConnection);
    if (item.virtualOutput) {
        item.virtualOutput->renderNextFrame();
    }
    ```
    This guarantees that `displayedSlideText` updates synchronously before the offscreen frame is rendered on the keystroke.
- **Cursor Hiding Fix (`DisplayEngine::updateCursorVisibility()` & `eventFilter()`)**:
  - Previously, `updateCursorVisibility()` polled `QCursor::pos()` against `view->geometry().contains(globalPos)`, which on Wayland or overlapping windows falsely triggered `QGuiApplication::setOverrideCursor(Qt::BlankCursor)` over the main FOHMedia operator window.
  - Fixed by tracking `QPointer<QQuickView> m_hoveredDisplayView` strictly via `QEvent::Enter` / `QEvent::HoverEnter` / `QEvent::Leave` / `QEvent::HoverLeave` on the presentation `QQuickView` windows. The cursor is never hidden on the main FOHMedia window or when only virtual displays are running.

### C. Offscreen Qt 6 RHI Renderer (`inc/VirtualDisplayOutput.h`, `src/VirtualDisplayOutput.cpp`)
- `VirtualDisplayOutput` uses Qt 6's `QQuickRenderControl` + windowless `QQuickWindow` (`setColor(QColor(0, 0, 0, 0))`) + `QRhiTexture` (`QRhiTexture::RGBA8`, `RenderTarget | UsedAsTransferSource`) + `QRhiRenderBuffer` (`DepthStencil`) + `QRhiTextureRenderTarget`.
- **Step 1 Optimizations Already Applied**:
  1. `IVirtualFrameSink::requiresCpuReadback() const` added to `inc/VirtualDisplayOutput.h` (defaults to `true`; zero-copy GPU sinks in Step 2 will override this to return `false`).
  2. **GPU Vertical Orientation (`qrt.setMirrorVertically(true)`)**:
     - When `m_rhi->isYUpInFramebuffer()` is true (OpenGL), `QQuickRenderTarget::setMirrorVertically(true)` is set so the GPU renders the frame upright directly, completely eliminating the 8.3 MB/frame CPU `QImage::flipped(Qt::Vertical)` copy.
  3. **Zero-Copy `QByteArray` -> `QImage` Wrapping**:
     - When `requiresCpuReadback()` is true, `readback.data` (`QByteArray`) is wrapped directly in a `QImage` with a custom cleanup function rather than deep-copying 8.3 MB on the CPU.
  4. **Conditional Rendering**:
     - Only re-runs `polishItems() / beginFrame() / sync() / render() / endFrame()` when `forceRender || m_sceneDirty || m_renderedFrameCount == 0`.

### D. Linux Backend (`src/PipeWireFrameSink.cpp`) & Hardware VA-API Decode (`src/main.cpp`)
- **Status on Linux**: **100% Working, Tested, and Verified in OBS Studio 32.1.0**.
- **How OBS Connects on Linux**:
  - Added via stock OBS's built-in **`Video Capture Device (PipeWire)`** source -> Device: **`FOHMedia - Audience`** -> Video Format: **`1920x1080 (16:9) - 60 FPS - ABGR8888`** (or `ARGB8888`).
- **Key Implementation Details in `src/PipeWireFrameSink.cpp`**:
  - Publishes two separate flat `SPA_PARAM_EnumFormat` pods (`SPA_POD_Id(SPA_VIDEO_FORMAT_RGBA)` first, `SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA)` second) because OBS's `pipewire-camera-source` (`camera-portal.c`) requires a fixed `SPA_POD_Id` (not `SPA_POD_CHOICE_ENUM_Id`) per `EnumFormat` pod to populate its Video Format and Frame Rate dropdowns.
  - Uses a dedicated 60 Hz (`16,666,667 ns`) `pw_loop_add_timer` inside the `pw_thread_loop` (`onPwTimerTick`) calling `pw_stream_trigger_process(m_stream)` so graph cycles run at a rock-solid 60 Hz independent of Qt GUI thread load.
  - Uses triple-buffered (`SPA_POD_CHOICE_RANGE_Int(3, 2, 4)`) cache-coherent `SPA_DATA_MemFd` shared memory buffers with per-slot `writtenGeneration` tracking so static slides perform zero redundant `memcpy`s, and 60 FPS video uses a single contiguous `std::memcpy` with zero channel swizzling (`RGBA8` -> `SPA_VIDEO_FORMAT_RGBA`).
  - In `src/main.cpp`, sets `qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "vaapi,cuda,drm,vulkan")` on Linux (when `disableHwVideo` is false), enabling Intel Arc `iHD` VA-API hardware video decoding (0% CPU on `av:h264` threads, ~0.4% total system CPU at 1080p60).

---

## 3. Step 2 Implementation: macOS (M5 MacBook Pro) & Windows 11 Zero-Copy GPU Sinks (Completed & Verified)

Both `src/SyphonFrameSink.cpp` (macOS) and `src/SpoutFrameSink.cpp` (Windows 11) now return `requiresCpuReadback() == false` and perform true zero-copy GPU texture sharing directly from Qt 6's `QRhiTexture` (`QRhi::Metal` on macOS, `QRhi::D3D11` on Windows 11).

On **macOS**, OBS Studio's built-in **`Syphon Client`** source discovers Syphon servers via the **Syphon Framework (`SyphonMetalServer`)**, which broadcasts the server description and `IOSurfaceID` over macOS Distributed Notifications and CFMessagePorts (`info.v002.Syphon.ServerAnnounce`, etc.).

### Task 2A: macOS Zero-Copy Metal + Syphon Implementation (`src/SyphonFrameSink.cpp` / `third_party/Syphon/` / `CMakeLists.txt`) — COMPLETED

On macOS (including the production **MacBook Pro M5 with macOS 27**), Qt 6's default RHI backend is **Metal** (`QRhi::Metal`).

#### 1. How OBS Studio on macOS Captures Virtual Displays
- Stock OBS Studio on macOS includes a built-in **`Syphon Client`** source out of the box.
- When you add a **`Syphon Client`** source in OBS, it lists active Syphon servers in the format `[AppName] ServerName` (e.g. `[FOHMedia] Audience`) and has an **`Allow Transparency`** checkbox that preserves full 8-bit alpha!

#### 2. Vendored `SyphonMetalServer` (`third_party/Syphon/`)
- Vendored the BSD-licensed Syphon Metal server/client/directory Objective-C sources into `third_party/Syphon/` and compiled them directly into `fohmedia_core` on `APPLE` (`enable_language(OBJC OBJCXX)`), with an inline Metal shader compiler fallback in `third_party/Syphon/SyphonServerRendererMetal.m` so no external `.framework` or `.metallib` file is required at runtime.
- `src/SyphonFrameSink.cpp` is compiled as Objective-C++ (`LANGUAGE OBJCXX` with `-fobjc-arc`) on `APPLE` and links `-framework Metal -framework QuartzCore -framework IOSurface -framework Cocoa`.

#### 3. True Zero-Copy Metal GPU Path (`requiresCpuReadback() -> false`)
In `SyphonFrameSink` on macOS:
1. Overrides `bool requiresCpuReadback() const override { return false; }`.
2. In `submitFrame(QRhi* rhi, QRhiTexture* texture, const QImage& rgbaImage)`:
   - Extracts Qt 6's native Metal handles (`QRhiMetalNativeHandles`) and `id<MTLTexture>` from `QRhi`:
     ```objc
     const auto* metalHandles =
         static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
     id<MTLDevice> rhiDevice = (metalHandles && metalHandles->dev)
         ? (__bridge id<MTLDevice>)(__bridge void*)metalHandles->dev
         : m_mtlDevice;
     id<MTLCommandQueue> rhiCmdQueue = (metalHandles && metalHandles->cmdQueue)
         ? (__bridge id<MTLCommandQueue>)(__bridge void*)metalHandles->cmdQueue
         : m_cmdQueue;
     id<MTLTexture> srcTexture =
         (__bridge id<MTLTexture>)reinterpret_cast<void*>(
             static_cast<uintptr_t>(texture->nativeTexture().object));
     ```
   - Publishes the frame directly on the Metal GPU command queue with **zero CPU readback**:
     ```objc
     id<MTLCommandBuffer> cmdBuf = [rhiCmdQueue commandBuffer];
     [m_syphonServer publishFrameTexture:srcTexture
                         onCommandBuffer:cmdBuf
                             imageRegion:NSMakeRect(0, 0, m_width, m_height)
                                 flipped:YES];
     [cmdBuf commit];
     [cmdBuf waitUntilCompleted];
     ```
   - Retains a fallback `QImage` -> `m_fallbackTexture` path when `rhi == nullptr` or `texture == nullptr` for unit tests.

---

### Task 2B: Windows 11 Zero-Copy D3D11 + Spout2 Implementation (`src/SpoutFrameSink.cpp`) — COMPLETED

On Windows 11, Qt 6's default RHI backend is **Direct3D 11** (`QRhi::D3D11`).
In `src/SpoutFrameSink.cpp`:
1. Overrides `bool requiresCpuReadback() const override { return false; }`.
2. In `submitFrame(QRhi* rhi, QRhiTexture* texture, const QImage& rgbaImage)`:
   - Extracts Qt's own `ID3D11Device*` and `ID3D11DeviceContext*` from `static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles())` (`dev` and `context`).
   - Allocates `m_sharedTexture` (matching `srcDesc.Width`, `srcDesc.Height`, and `srcDesc.Format`, with `D3D11_RESOURCE_MISC_SHARED`) on Qt's `ID3D11Device*` and updates the Spout2 `SpoutSharedTextureInfo` shared memory mapping.
   - Grabs Qt's native `ID3D11Texture2D* srcTex = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(texture->nativeTexture().object))` and copies GPU-to-GPU via:
     ```cpp
     qtContext->CopyResource(m_sharedTexture, srcTex);
     qtContext->Flush();
     ```
   - Retains the standalone `D3D11CreateDevice` + `UpdateSubresource` fallback when `rhi == nullptr` or `texture == nullptr` for unit tests.

---

## 4. Key Files Reference

| File | Purpose |
| :--- | :--- |
| `inc/ScreenModel.h` / `src/ScreenModel.cpp` | Defines `ScreenConfig::VirtualObsDisplay = -3`, `"Virtual Display (OBS)"` dropdown option, and combobox index helpers. |
| `qml/DisplaysView.qml` | UI configuration for physical vs. `"Virtual Display (OBS)"` screens, custom geometry SpinBoxes, and `OBS Source: FOHMedia - <Name> (RGBA)` badge. |
| `inc/VirtualDisplayOutput.h` / `src/VirtualDisplayOutput.cpp` | Headless 60 FPS `QQuickRenderControl` + `QRhiTexture` renderer and `IVirtualFrameSink` interface. |
| `src/PipeWireFrameSink.cpp` | Linux PipeWire (`pw_stream`) backend — completed, tested, and verified at 1080p60 in OBS Studio 32.1.0. |
| `src/SyphonFrameSink.cpp` / `third_party/Syphon/` | macOS Objective-C++ (`OBJCXX`) backend using `SyphonMetalServer` + zero-copy Metal `id<MTLTexture>` (`requiresCpuReadback() == false`). |
| `src/SpoutFrameSink.cpp` | Windows 11 Spout2 DXGI backend using zero-copy `ID3D11DeviceContext::CopyResource` via `QRhiD3D11NativeHandles` (`requiresCpuReadback() == false`). |
| `inc/DisplayEngine.h` / `src/DisplayEngine.cpp` | Manages active `QQuickView` and `VirtualDisplayOutput` instances, synchronous `checkTransition` invocation, `audienceDelayMs`, and cursor hiding. |
| `tests/GUITests.cpp` | Unit & GUI tests including `testVirtualObsDisplayScreenModelAndSerialization` and `testVirtualObsDisplayHeadlessEngineAndPipeWireSink`. |

---

## 5. Build & Test Commands

Per project rules in `AGENTS.md`:
- Always build in `cmake-build-debug` using CMake (`cmake --build cmake-build-debug -j$(nproc)` on Linux / `cmake --build cmake-build-debug` on macOS).
- Run the test suite after building: `./cmake-build-debug/FOHMedia_GUITest` (or `./cmake-build-debug/FOHMedia_GUITest.app/Contents/MacOS/FOHMedia_GUITest` if bundled on macOS).
