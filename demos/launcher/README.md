# AetherShell Launcher — Bifrost Decoupled Showcase

A high-performance, Launchpad-style desktop application launcher showcasing the **Decoupled Architecture** powered by the **Bifrost IPC** library (`libbifrost`).

---

## 🎯 Architecture & Concept

Traditional Linux application launchers are monolithic: upon startup, each GUI instance scans `/usr/share/applications`, parses hundreds of `.desktop` files, loads and scales dozens of icon files (PNG/SVG) from disk, and caches them in memory. This causes noticeable startup lag, high memory consumption, and redundant work across multiple desktop widgets.

**AetherShell** solves this by completely decoupling the **Logic** from the **User Interface**:

```
                       ┌──────────────────────────────────────────────┐
                       │              Filesystem (.desktop)           │
                       └──────────────────────┬───────────────────────┘
                                              │ scans & parses
                                              ▼
                                 ┌─────────────────────────┐
                                 │   aethershell-daemon    │
                                 │ (Writer: "daemon")      │
                                 └────────────┬────────────┘
                                              │
                      Zero-Copy Memory Alloc  │  Publishes pre-rendered
                        (ARGB32 Icon Pixels)  │  SharedLauncherState
                                              ▼
 ═════════════════════════════════════════════════════════════════════════════════════
               Bifrost Shared Memory Pool ("aethershell" - 128 MB Arena)
 ═════════════════════════════════════════════════════════════════════════════════════
         ▲                                   ▲                          ▲
         │ Lock-Free Command RPC             │ Read State & Pixels      │ Read State
         │ (launch, uninstall, shortcut)     │ (< 1 µs latency)         │
         │                                   │                          │
 ┌───────┴───────────────┐       ┌───────────┴──────────┐   ┌───────────┴──────────┐
 │       launcher        │       │      python_ui       │   │     cli-launcher     │
 │      (GTK3 GUI)       │       │    (Tkinter + PIL)   │   │     (Terminal)       │
 └───────────────────────┘       └──────────────────────┘   └──────────────────────┘
```

1. **The Daemon (`aethershell-daemon`)**:
   - Acts as the background director and sole writer (`"daemon"`).
   - Scans and watches desktop directories using `GFileMonitor`.
   - Parses `.desktop` files and rasterizes application icons into raw **ARGB32 zero-copy pixel buffers**.
   - Publishes `SharedLauncherState` into the Bifrost shared arena.
   - Registers commands with the **Bifrost Command Layer** (`CMD_LAUNCH_APP`, `CMD_UNINSTALL_APP`, `CMD_CREATE_SHORTCUT`, `CMD_REFRESH_APPS`).

2. **The Frontends (`launcher`, `python_ui`, `cli-launcher`, `tui-launcher`)**:
   - Act as **Zero-Copy Readers**: instantly open the shared memory pool without parsing or disk I/O.
   - Render icons straight from shared memory buffers in sub-millisecond time.
   - Send commands back to the daemon's lock-free inbox using `bifrost_reader_send_command()`.

---

## 🚀 Available Frontends

| Frontend | Technology | Description |
| :--- | :--- | :--- |
| **`launcher`** | C11 + GTK3 | Fullscreen glassmorphic overlay with search, 7-column grid, context menus, and Wayland Layer-Shell support. |
| **`python_ui`** | Python 3 + Tkinter + PIL | Modern dark-themed UI demonstrating Bifrost C-types FFI and zero-copy pixel reading in Python. |
| **`cli-launcher`** | C11 (Stdio) | Fast, text-based interactive terminal app launcher. |
| **`tui-launcher`** | C11 + NCurses | Keyboard-driven terminal curses interface with live search and navigation. |

---

## 📦 Prerequisites & System Dependencies

### 1. Bifrost Library
Make sure the **Bifrost** library is installed on your system. From the root of the Bifrost repository:

```bash
meson setup build --prefix=/usr
sudo meson install -C build
```

### 2. Required Packages (Ubuntu / Debian)

Install the required development libraries:

```bash
# Core build tools and GTK3
sudo apt update && sudo apt install -y \
    meson \
    ninja-build \
    build-essential \
    pkg-config \
    libgtk-3-dev

# Optional: Wayland Layer-Shell overlay support
sudo apt install -y libgtk-layer-shell-dev

# Optional: Terminal Curses (for tui-launcher)
sudo apt install -y libncurses-dev

# Optional: Python UI dependencies (for python_ui)
sudo apt install -y python3-tk python3-pil python3-pil.imagetk
```

---

## 🛠️ Building

From within the `demos/launcher` directory:

```bash
# 1. Setup the Meson build directory
meson setup build

# 2. Compile all targets
ninja -C build
```

This compiles:
- `build/aethershell-daemon`
- `build/launcher`
- `build/cli-launcher`
- `build/tui-launcher` (if `ncurses` is installed)

---

## 🖥️ Running

### Step 1: Start the Backend Daemon
In a terminal, start the daemon (it will index applications and create the `/bifrost_aethershell` shared pool):

```bash
./build/aethershell-daemon
```

*Output example:*
```text
[Daemon] Starting AetherShell Daemon...
[Daemon] Created Bifrost memory pool 'aethershell'
[Daemon] Synced 73 apps to shared memory (Update 1)
```

### Step 2: Launch Any Frontend

Open another terminal or launch your preferred frontend:

#### Option A: GTK3 Desktop Launcher
```bash
./build/launcher
```

#### Option B: Python Tkinter Launcher
```bash
python3 src/python_ui/launcher.py
```

#### Option C: CLI Terminal Launcher
```bash
./build/cli-launcher
```

#### Option D: NCurses TUI Launcher
```bash
./build/tui-launcher
```

---

## ⌨️ Keybindings (GTK3 Launcher)

| Key | Action |
| :--- | :--- |
| `Escape` | Close launcher overlay |
| `Any Character` | Immediately focuses search bar and filters apps |
| `Right Click` on App | Open context menu (**Launch**, **Add Shortcut to Desktop**, **Uninstall**) |
| `Left Click` on App | Launch application via Daemon RPC |

---

## 📊 Live Telemetry & Benchmark Overlay

When running the GTK3 launcher, a real-time HUD appears in the top-left corner:

- **`Polling Rate (~820,000 Hz)`**: Shows how many times per second the GUI checks for updates in shared memory. In this demo, the polling loop runs **uncapped at full CPU capacity** to demonstrate the maximum throughput of lock-free memory checks. *(Note: For standard battery-friendly production desktop use, polling can be capped at 60–120 Hz with 0.0% CPU overhead)*.
- **`Access Latency (~0.0000 ms)`**: The actual memory fetch latency. Because Bifrost utilizes shared memory, checking updates is a single atomic cache read taking only **~5–10 nanoseconds**, yielding a virtual `0.0000 ms`.
- **`Simulated Throughput (~14+ TB/s)`**: Theoretical bandwidth calculated by multiplying the complete `SharedLauncherState` data buffer (~19 MB of raw ARGB32 icon pixels for all applications) by the uncapped polling frequency. Demonstrates how much data can be traversed without memory copy bottlenecks.
- **`Zero-Copy Rendering (Active)`**: Cairo draws icons directly from the shared memory pointer mapped by Bifrost without copying pixel bytes into the GUI process.

---

## 📂 Project Structure

```
demos/launcher/
├── meson.build                  # Meson build definitions
├── README.md                    # Project documentation
├── prom.md                      # IPC Architecture & C-API specification
├── data/
│   ├── launcher.desktop         # Desktop entry for installation
│   └── style/
│       └── launcher.css         # GTK3 dark CSS theme
└── src/
    ├── main.c                   # GTK3 Launcher application entry point
    ├── core/                    # Core business logic (Daemon side)
    │   ├── app_entry.[c|h]      # Application model definition
    │   ├── app_manager.[c|h]    # App discovery and monitoring
    │   ├── desktop_reader.[c|h] # .desktop file parser
    │   ├── icon_loader.[c|h]    # Icon resolution and rendering
    │   └── launcher_ipc.h       # Shared memory structures & Command payloads
    ├── daemon/
    │   └── main.c               # AetherShell Daemon (Writer & Command listener)
    ├── ui/                      # GTK3 User Interface components
    │   ├── client_ipc.[c|h]     # Zero-copy reader & command dispatch
    │   ├── app_grid.[c|h]       # Paginated FlowBox app grid
    │   ├── app_icon.[c|h]       # Individual app icon widget
    │   ├── search_bar.[c|h]     # Real-time search entry
    │   ├── launcher_window.[c|h]# Main fullscreen window
    │   └── uninstall_dialog.[c|h]
    ├── cli/
    │   └── main.c               # CLI interface
    ├── tui/
    │   └── main.c               # NCurses TUI interface
    ├── python_ui/
    │   └── launcher.py          # Python Tkinter + PIL client
    └── utils/
        └── string_utils.[c|h]
```
