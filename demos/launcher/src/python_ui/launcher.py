import os
import sys
import ctypes
import tkinter as tk
from tkinter import ttk
from PIL import Image, ImageTk

# ── Load bifrost ──
try:
    bf = ctypes.CDLL("libbifrost.so")
except Exception as e:
    print(f"Error loading libbifrost.so: {e}")
    sys.exit(1)

class BifrostWriterRef(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_uint32),
        ("gen", ctypes.c_uint32),
    ]

bf.bifrost_join_as_reader.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
bf.bifrost_join_as_reader.restype = ctypes.c_void_p

bf.bifrost_reader_get.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)]
bf.bifrost_reader_get.restype = ctypes.c_void_p

bf.bifrost_reader_leave.argtypes = [ctypes.c_void_p]
bf.bifrost_reader_leave.restype = None

bf.bifrost_reader_resolve_writer.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
bf.bifrost_reader_resolve_writer.restype = BifrostWriterRef

bf.bifrost_reader_writer_alive.argtypes = [ctypes.c_void_p, BifrostWriterRef]
bf.bifrost_reader_writer_alive.restype = ctypes.c_bool

bf.bifrost_reader_send_command.argtypes = [
    ctypes.c_void_p,
    BifrostWriterRef,
    ctypes.c_uint32,
    ctypes.c_void_p,
    ctypes.c_uint32,
    ctypes.c_uint32,
    ctypes.POINTER(ctypes.c_uint64),
]
bf.bifrost_reader_send_command.restype = ctypes.c_int

# ── C Structs Definition ──
LAUNCHER_MAGIC = 0x4C4E4352
MAX_APPS = 512
MAX_ICON_BYTES = 96 * 96 * 4

class SharedAppEntry(ctypes.Structure):
    _fields_ = [
        ("index", ctypes.c_uint32),
        ("name", ctypes.c_char * 64),
        ("exec_", ctypes.c_char * 256),
        ("desktop_path", ctypes.c_char * 256),
        ("package_name", ctypes.c_char * 64),
        ("has_icon", ctypes.c_uint8),
        ("icon_width", ctypes.c_uint32),
        ("icon_height", ctypes.c_uint32),
        ("icon_rowstride", ctypes.c_uint32),
        ("icon_channels", ctypes.c_uint32),
        ("icon_data", ctypes.c_uint8 * MAX_ICON_BYTES),
    ]

class SharedLauncherState(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("app_count", ctypes.c_uint32),
        ("apps", SharedAppEntry * MAX_APPS),
        ("update_counter", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
    ]

class CmdLaunchPayload(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256),
        ("exec_", ctypes.c_char * 256),
    ]

class CmdUninstallPayload(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256),
        ("package_name", ctypes.c_char * 64),
        ("exec_", ctypes.c_char * 256),
    ]

class CmdShortcutPayload(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256),
    ]

class LaunchData(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256),
        ("exec_", ctypes.c_char * 256)
    ]

class UninstallData(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256),
        ("package_name", ctypes.c_char * 64),
        ("exec_", ctypes.c_char * 256)
    ]

class ShortcutData(ctypes.Structure):
    _fields_ = [
        ("desktop_path", ctypes.c_char * 256)
    ]

class LauncherCommandData(ctypes.Union):
    _fields_ = [
        ("launch", LaunchData),
        ("uninstall", UninstallData),
        ("shortcut", ShortcutData)
    ]

class LauncherCommand(ctypes.Structure):
    _fields_ = [
        ("cmd", ctypes.c_uint8),
        ("_pad", ctypes.c_uint8 * 3),
        ("data", LauncherCommandData)
    ]

CMD_LAUNCH_APP = 1
CMD_UNINSTALL_APP = 2
CMD_CREATE_SHORTCUT = 3
CMD_REFRESH_APPS = 4

class LauncherUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Project Zero - Python Launcher")
        self.geometry("800x600")
        self.configure(bg="#1E1E2E") # Modern dark theme
        
        # Connect to Daemon with a unique name to avoid collisions if crashed previously
        reader_name = f"python_ui_{os.getpid()}".encode('utf-8')
        self.reader = bf.bifrost_join_as_reader(b"aethershell", reader_name)
        if not self.reader:
            print("Failed to connect to daemon!")
            sys.exit(1)
        self.daemon_ref = bf.bifrost_reader_resolve_writer(self.reader, b"daemon")
            
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        
        self.state = None
        self.apps = []
        self.images = {}
        self.last_counter = -1
        
        import time
        self.frames = 0
        self.fps = 0
        self.last_fps_time = time.time()
        
        # Add status bar
        self.status_bar = tk.Label(self, text="Read Latency: 0 µs | Polling Rate: 0 Hz", bg="#11111B", fg="#A6ADC8", font=("Helvetica", 10), pady=5)
        self.status_bar.pack(side="bottom", fill="x")
        
        self.create_widgets()
        self.poll_daemon()
        
    def on_close(self):
        if self.reader:
            bf.bifrost_reader_leave(self.reader)
            self.reader = None
        self.destroy()
        
    def on_mousewheel(self, event):
        # Support for Windows/Mac (MouseWheel) and Linux X11 (Button-4/Button-5)
        if event.num == 4 or event.delta > 0:
            self.canvas.yview_scroll(-1, "units")
        elif event.num == 5 or event.delta < 0:
            self.canvas.yview_scroll(1, "units")
            
    def create_widgets(self):
        self.search_var = tk.StringVar()
        self.search_var.trace("w", self.filter_apps)
        
        search_entry = tk.Entry(self, textvariable=self.search_var, font=("Helvetica", 18), bg="#313244", fg="#CDD6F4", insertbackground="#CDD6F4", relief="flat")
        search_entry.pack(fill=tk.X, padx=20, pady=20, ipady=10)
        search_entry.focus()
        
        self.grid_frame = tk.Frame(self, bg="#1E1E2E")
        self.grid_frame.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 20))
        
        self.canvas = tk.Canvas(self.grid_frame, bg="#1E1E2E", highlightthickness=0)
        self.scrollbar = ttk.Scrollbar(self.grid_frame, orient="vertical", command=self.canvas.yview)
        self.scrollable_frame = tk.Frame(self.canvas, bg="#1E1E2E")
        
        self.scrollable_frame.bind(
            "<Configure>",
            lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        )
        
        self.canvas.create_window((0, 0), window=self.scrollable_frame, anchor="nw")
        self.canvas.configure(yscrollcommand=self.scrollbar.set)
        
        self.canvas.pack(side="left", fill="both", expand=True)
        self.scrollbar.pack(side="right", fill="y")
        
        search_entry.bind("<Return>", self.on_enter)
        
        # Bind mouse scrolling to the entire window
        self.bind_all("<MouseWheel>", self.on_mousewheel)
        self.bind_all("<Button-4>", self.on_mousewheel)
        self.bind_all("<Button-5>", self.on_mousewheel)
        
    def poll_daemon(self):
        import time
        start_ns = time.perf_counter_ns()
        size = ctypes.c_size_t()
        ptr = bf.bifrost_reader_get(self.reader, b"daemon", ctypes.byref(size))
        end_ns = time.perf_counter_ns()
        
        latency_us = (end_ns - start_ns) / 1000.0
        
        self.frames += 1
        current_time = time.time()
        if current_time - self.last_fps_time >= 1.0:
            self.fps = self.frames
            self.frames = 0
            self.last_fps_time = current_time
            
        self.status_bar.config(text=f"Read Latency: {latency_us:.2f} µs | Polling Rate: {self.fps} Hz")
        
        if ptr and size.value >= ctypes.sizeof(SharedLauncherState):
            st = ctypes.cast(ptr, ctypes.POINTER(SharedLauncherState)).contents
            if st.magic == LAUNCHER_MAGIC:
                if self.last_counter != st.update_counter:
                    self.last_counter = st.update_counter
                    self.apps = []
                    for i in range(st.app_count):
                        self.apps.append(st.apps[i])
                    self.filter_apps()
                
        self.after(16, self.poll_daemon) # Poll at ~60Hz
        
    def filter_apps(self, *args):
        query = self.search_var.get().lower()
        
        for widget in self.scrollable_frame.winfo_children():
            widget.destroy()
            
        row = 0
        col = 0
        columns = 5 # 5 apps per row
        
        self.filtered_apps = []
        for app in self.apps:
            name = app.name.decode('utf-8')
            if query in name.lower():
                self.filtered_apps.append(app)
                self.add_app_icon(app, name, row, col)
                col += 1
                if col >= columns:
                    col = 0
                    row += 1

    def add_app_icon(self, app, name, row, col):
        frame = tk.Frame(self.scrollable_frame, bg="#1E1E2E", width=130, height=140)
        frame.grid(row=row, column=col, padx=10, pady=10)
        frame.pack_propagate(False)
        
        # Read the raw zero-copy pixel buffer using PIL
        img_tk = None
        if app.has_icon and app.icon_width > 0 and app.icon_height > 0:
            raw_data = bytes(app.icon_data[:app.icon_width * app.icon_height * 4])
            try:
                # Cairo ARGB32 maps to BGRA format in PIL
                img = Image.frombuffer("RGBA", (app.icon_width, app.icon_height), raw_data, "raw", "BGRA", 0, 1)
                
                if app.icon_width != 64:
                    img = img.resize((64, 64), Image.LANCZOS)
                
                img_tk = ImageTk.PhotoImage(img)
                self.images[name] = img_tk 
            except Exception as e:
                print(f"Image error: {e}")
        
        if img_tk:
            lbl_icon = tk.Label(frame, image=img_tk, bg="#1E1E2E", cursor="hand2")
        else:
            lbl_icon = tk.Label(frame, text="?", font=("Helvetica", 32), bg="#313244", fg="#CDD6F4", cursor="hand2")
            
        lbl_icon.pack(expand=True)
        
        lbl_name = tk.Label(frame, text=name, bg="#1E1E2E", fg="#CDD6F4", font=("Helvetica", 10), wraplength=110, justify="center", cursor="hand2")
        lbl_name.pack(side=tk.BOTTOM)
        
        # Click handlers
        lbl_icon.bind("<Button-1>", lambda e, a=app: self.launch_app(a))
        lbl_name.bind("<Button-1>", lambda e, a=app: self.launch_app(a))
        frame.bind("<Button-1>", lambda e, a=app: self.launch_app(a))
        
        # Context menu handlers (Right Click)
        lbl_icon.bind("<Button-3>", lambda e, a=app: self.show_context_menu(e, a))
        lbl_name.bind("<Button-3>", lambda e, a=app: self.show_context_menu(e, a))
        frame.bind("<Button-3>", lambda e, a=app: self.show_context_menu(e, a))
        
    def show_context_menu(self, event, app):
        menu = tk.Menu(self, tearoff=0, bg="#313244", fg="#CDD6F4", activebackground="#45475A", activeforeground="#CDD6F4", font=("Helvetica", 11))
        menu.add_command(label="Launch App", command=lambda: self.launch_app(app))
        menu.add_separator()
        menu.add_command(label="Add Shortcut to Desktop", command=lambda: self.create_shortcut(app))
        menu.add_command(label="Uninstall Application", command=lambda: self.uninstall_app(app))
        menu.tk_popup(event.x_root, event.y_root)

    def get_daemon_ref(self):
        if not hasattr(self, "daemon_ref") or self.daemon_ref.id == 0 or not bf.bifrost_reader_writer_alive(self.reader, self.daemon_ref):
            self.daemon_ref = bf.bifrost_reader_resolve_writer(self.reader, b"daemon")
        return self.daemon_ref
        
    def create_shortcut(self, app):
        dref = self.get_daemon_ref()
        if dref.id != 0:
            payload = CmdShortcutPayload()
            payload.desktop_path = app.desktop_path
            bf.bifrost_reader_send_command(
                self.reader,
                dref,
                CMD_CREATE_SHORTCUT,
                ctypes.byref(payload),
                ctypes.sizeof(payload),
                1, # BIFROST_SEND_F_NO_REPLY
                None
            )
            print(f"Requested Daemon to create shortcut for {app.name.decode('utf-8')}")
            
    def uninstall_app(self, app):
        dref = self.get_daemon_ref()
        if dref.id != 0:
            payload = CmdUninstallPayload()
            payload.desktop_path = app.desktop_path
            payload.package_name = app.package_name
            payload.exec_ = app.exec_
            bf.bifrost_reader_send_command(
                self.reader,
                dref,
                CMD_UNINSTALL_APP,
                ctypes.byref(payload),
                ctypes.sizeof(payload),
                1, # BIFROST_SEND_F_NO_REPLY
                None
            )
            print(f"Requested Daemon to uninstall {app.name.decode('utf-8')}")
            
    def on_enter(self, event):
        if self.filtered_apps:
            self.launch_app(self.filtered_apps[0])
            
    def launch_app(self, app):
        dref = self.get_daemon_ref()
        if dref.id != 0:
            payload = CmdLaunchPayload()
            payload.desktop_path = app.desktop_path
            payload.exec_ = app.exec_
            bf.bifrost_reader_send_command(
                self.reader,
                dref,
                CMD_LAUNCH_APP,
                ctypes.byref(payload),
                ctypes.sizeof(payload),
                1, # BIFROST_SEND_F_NO_REPLY
                None
            )
            print(f"Requested Daemon to launch {app.name.decode('utf-8')}")
            self.on_close()
            return

        import subprocess
        import shlex
        
        exec_cmd = app.exec_.decode('utf-8').strip()
        if not exec_cmd:
            return
            
        # Parse the command line safely
        args = shlex.split(exec_cmd)
        
        # Filter out desktop placeholders like %U, %F, %f, %u
        filtered_args = [arg for arg in args if not (arg.startswith('%') and arg[1:] in ['U', 'F', 'u', 'f'])]
        
        if filtered_args:
            try:
                # Launch detached from terminal, closing stdin/out/err to avoid blocking
                subprocess.Popen(
                    filtered_args,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    start_new_session=True # Equivalent to setsid()
                )
            except Exception as e:
                print(f"Failed to launch {filtered_args}: {e}")
                
        self.on_close()

if __name__ == "__main__":
    app = LauncherUI()
    app.mainloop()
