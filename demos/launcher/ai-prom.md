I am building a language-agnostic desktop launcher following a decoupled architecture. 
I have a background Daemon (written in C) that reads all installed Linux applications, parses their .desktop files, renders their icons into raw ARGB32 pixels, and stores everything into a Shared Memory pool using a custom library called `bifrost` (compiled as libbifrost.so).

Your task is to build a modern GUI for this launcher in Python 3 using Tkinter for the UI, and Pillow (PIL) for reading the raw zero-copy pixel buffers into Tkinter PhotoImage.

ARCHITECTURAL PRINCIPLES:
- This launcher is a Thin Client.
- The Daemon is the Single Source of Truth.
- The GUI must never own application data.
- The GUI is stateless whenever possible.
- Shared Memory is the only source of application metadata.
- IPC commands are the only valid way to mutate state.
- Never duplicate daemon logic inside the UI.

PERFORMANCE REQUIREMENTS:
- Never copy icon buffers unless absolutely required (use Zero-Copy image loading).
- Reuse image objects whenever possible.
- Avoid rebuilding widgets unnecessarily.
- Only redraw when `update_counter` changes.
- Avoid allocations inside the polling loop.
- Keep the UI thread responsive.

You do NOT need to parse any desktop files or load any images from the disk. Your ONLY job is to connect to the shared memory, read the structs, render the UI, and send back IPC commands when the user interacts.

Here is the exact memory contract (C Structs) you need to map in your language:

1. CONSTANTS:
LAUNCHER_MAGIC = 0x4C4E4352
MAX_APPS = 512
MAX_ICON_BYTES = 96 * 96 * 4

2. APP ENTRY STRUCT:
struct SharedAppEntry {
    uint32_t index;
    char name[64];
    char exec[256];
    char desktop_path[256];
    char package_name[64];
    uint8_t has_icon;
    uint32_t icon_width;
    uint32_t icon_height;
    uint32_t icon_rowstride;
    uint32_t icon_channels;
    uint8_t icon_data[MAX_ICON_BYTES]; // Raw ARGB32/BGRA pixel buffer
};

3. STATE STRUCT:
struct SharedLauncherState {
    uint32_t magic;
    uint32_t app_count;
    SharedAppEntry apps[MAX_APPS];
    uint64_t update_counter;
    uint64_t timestamp_ns;
};

4. IPC COMMAND STRUCTS (For writing back to Daemon):
struct LauncherCommand {
    uint8_t cmd; // 1=Launch, 2=Uninstall, 3=Create Shortcut
    uint8_t _pad[3];
    union {
        struct { char desktop_path[256]; char exec[256]; } launch;
        struct { char desktop_path[256]; char package_name[64]; char exec[256]; } uninstall;
        struct { char desktop_path[256]; } shortcut;
    } data;
};

HOW TO INTERACT WITH THE BIFROST C-LIBRARY:
You must load `libbifrost.so` and use these functions:
- To Read:
  1. `bifrost_reader_t* bifrost_join_as_reader(const char* pool_name, const char* reader_name)` -> Call with ("aethershell", "my_ui_name").
  2. `void* bifrost_reader_get(bifrost_reader_t* reader, const char* writer_name, size_t* out_len)` -> Call with writer_name="daemon". Cast the returned pointer to `SharedLauncherState`.
  3. `void bifrost_reader_leave(bifrost_reader_t* reader)` -> ALWAYS call this on exit to free the slot!

- To Send Commands via the Bifrost Command Layer:
  1. Resolve daemon writer:
     `bifrost_writer_ref_t dref = bifrost_reader_resolve_writer(reader, "daemon");`
  2. Send command to daemon's inbox:
     `bifrost_reader_send_command(reader, dref, cmd_id, &payload, sizeof(payload), BIFROST_SEND_F_NO_REPLY, NULL);`
     Commands available:
     - `CMD_LAUNCH_APP` (1): payload `CmdLaunchPayload`
     - `CMD_UNINSTALL_APP` (2): payload `CmdUninstallPayload`
     - `CMD_CREATE_SHORTCUT` (3): payload `CmdShortcutPayload`
     - `CMD_REFRESH_APPS` (4): payload NULL (0 bytes)

REQUIREMENTS FOR YOUR CODE:
1. Map the structs properly matching C memory alignment.
2. Read the `icon_data` byte array (Zero-Copy) and convert it directly to an image object in your GUI framework. (Note: Cairo ARGB32 memory layout is physically BGRA on little-endian).
3. Poll the reader continuously (e.g. at 60Hz), but only rebuild the UI if `update_counter` changes.
4. When the user clicks an app, send a CMD_LAUNCH_APP command to daemon using the Bifrost Command Layer API.

... (Previous Prompt Text) ...

5. NON-GOALS (CRITICAL):
To adhere strictly to the "Project Zero" decoupled architecture, you MUST NOT do any of the following. The Daemon already handles all of this:
- DO NOT parse `.desktop` files.
- DO NOT scan the filesystem for applications.
- DO NOT load PNG/JPEG/SVG icons from the disk.
- DO NOT cache icons manually or maintain your own image database.
- DO NOT spawn application processes directly (use the IPC command).
- DO NOT implement complex background search indexing.
- DO NOT maintain application state outside of the Shared Memory.

Your code must be as "dumb" and lightweight as possible. Read the memory -> Draw the UI -> Send IPC commands on interaction. That's it.

6. UI/UX & STYLING REQUIREMENTS:
- Design a clean, modern, and frameless dark theme window (e.g., Catppuccin or Dracula colors).
- Place a large, elegant Search Bar at the top. Focus it immediately upon start.
- Below the search bar, display the apps in a clean scrollable Grid (e.g., 4 or 5 columns).
- Display the application icon (using the raw ARGB32 buffer scaled to 64x64 via Pillow) and the application name below it.
- Allow scrolling via the mouse wheel.
- Allow filtering the apps instantly as the user types in the search bar.
- Add a right-click Context Menu to each app with: "Launch", "Add Shortcut to Desktop", and "Uninstall".
- Ensure you use a unique reader name (like incorporating the process PID) to avoid bifrost connection collisions.
- Make sure to handle cleanup properly (call bifrost_reader_leave) when the user closes the window.
