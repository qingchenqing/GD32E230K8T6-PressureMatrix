"""16x8 pressure-matrix live viewer for the current 159-byte protocol."""

import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports

from monitor import read_packed_frame, source_row_for_display


ROWS = 16
COLS = 8
INVALID = 65535
COLOR_STOPS = (
    (0.00, (25, 42, 86)),
    (0.25, (27, 123, 188)),
    (0.50, (45, 192, 156)),
    (0.75, (250, 204, 65)),
    (1.00, (221, 65, 65)),
)


def heat_color(value, maximum):
    """Map a display value to a stable blue-to-red color scale."""
    ratio = max(0.0, min(1.0, value / maximum))
    for index in range(1, len(COLOR_STOPS)):
        high_pos, high_rgb = COLOR_STOPS[index]
        if ratio <= high_pos:
            low_pos, low_rgb = COLOR_STOPS[index - 1]
            blend = (ratio - low_pos) / (high_pos - low_pos)
            rgb = tuple(round(a + (b - a) * blend)
                        for a, b in zip(low_rgb, high_rgb))
            return "#{:02x}{:02x}{:02x}".format(*rgb), rgb
    return "#dd4141", COLOR_STOPS[-1][1]


class MatrixViewer:
    def __init__(self, root):
        self.root = root
        self.root.title("16×8 压力矩阵上位机 GD32")
        self.root.resizable(False, False)
        self.port = None
        self.reader = None
        self.stop_reader = threading.Event()
        self.frames = queue.Queue(maxsize=1)
        self.errors = queue.Queue()
        self.current_frame = None
        self.baseline = None
        self.next_display_at = 0.0

        self.port_name = tk.StringVar(value="COM5")
        self.view_mode = tk.StringVar(value="热力图")
        self.subtract_baseline = tk.BooleanVar(value=False)
        self.interval = tk.StringVar(value="1")
        self.color_max = tk.StringVar(value="100")
        self.status = tk.StringVar(value="未连接；请关闭占用 COM5 的串口助手")

        controls = ttk.Frame(root, padding=10)
        controls.grid(row=0, column=0, sticky="ew")
        ttk.Label(controls, text="串口").grid(row=0, column=0, padx=(0, 4))
        self.port_box = ttk.Combobox(controls, textvariable=self.port_name, width=8)
        self.port_box.grid(row=0, column=1)
        ttk.Button(controls, text="刷新串口", command=self.refresh_ports).grid(row=0, column=2, padx=5)
        self.connect_button = ttk.Button(controls, text="连接", command=self.toggle_connection)
        self.connect_button.grid(row=0, column=3, padx=(0, 15))
        ttk.Label(controls, text="刷新间隔(秒)").grid(row=0, column=4)
        ttk.Entry(controls, textvariable=self.interval, width=5).grid(row=0, column=5, padx=5)
        ttk.Label(controls, text="颜色上限").grid(row=0, column=6)
        ttk.Entry(controls, textvariable=self.color_max, width=5).grid(row=0, column=7, padx=5)

        options = ttk.Frame(root, padding=(10, 0, 10, 10))
        options.grid(row=1, column=0, sticky="ew")
        ttk.Label(options, text="显示方式").grid(row=0, column=0, padx=(0, 5))
        for column, name in enumerate(("热力图", "数字表格"), start=1):
            ttk.Radiobutton(options, text=name, value=name, variable=self.view_mode,
                            command=self.draw_frame).grid(row=0, column=column, padx=5)
        ttk.Button(options, text="记录当前零点", command=self.capture_baseline).grid(
            row=0, column=3, padx=(20, 5))
        ttk.Checkbutton(options, text="显示相对零点的增量", variable=self.subtract_baseline,
                        command=self.draw_frame).grid(row=0, column=4, padx=5)

        grid = ttk.Frame(root, padding=(10, 0, 10, 5))
        grid.grid(row=2, column=0)
        ttk.Label(grid, text="", width=7).grid(row=0, column=0)
        for col in range(COLS):
            ttk.Label(grid, text=f"COL{col + 1}", anchor="center", width=8).grid(
                row=0, column=col + 1, padx=1, pady=1)
        self.cells = []
        for row in range(ROWS):
            ttk.Label(grid, text=f"ROW{row + 1:02d}", width=7).grid(row=row + 1, column=0)
            cell_row = []
            for col in range(COLS):
                cell = tk.Label(grid, text="—", width=8, height=2, relief="solid",
                                bd=1, font=("Consolas", 11), bg="#f0f0f0")
                cell.grid(row=row + 1, column=col + 1, padx=1, pady=1)
                cell_row.append(cell)
            self.cells.append(cell_row)

        ttk.Label(root, textvariable=self.status, padding=(10, 5)).grid(
            row=3, column=0, sticky="w")
        ttk.Label(root, text="155字节协议；每组8行倒序显示；蓝色=低，红色=达到颜色上限；灰色 ERR=ADC 超时。数值不是压力单位。",
                  padding=(10, 0, 10, 10)).grid(row=4, column=0, sticky="w")
        self.color_max.trace_add("write", lambda *_: self.draw_frame())
        self.refresh_ports()
        self.root.after(100, self.poll_frames)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def refresh_ports(self):
        names = [item.device for item in list_ports.comports()]
        self.port_box["values"] = names
        if not self.port_name.get().strip() and names:
            self.port_name.set(names[0])

    def toggle_connection(self):
        if self.port is not None:
            self.disconnect()
            return
        name = self.port_name.get().strip()
        if not name:
            messagebox.showerror("串口", "请先选择 COM 端口")
            return
        try:
            self.port = serial.Serial(name, 115200, timeout=0.2)
        except serial.SerialException as exc:
            messagebox.showerror("连接失败", f"无法打开 {name}：\n{exc}\n\n请关闭串口助手后重试。")
            return
        self.stop_reader.clear()
        self.current_frame = None
        self.next_display_at = 0.0
        self.connect_button.configure(text="断开")
        self.status.set(f"已连接 {name}，等待有效数据包……")
        self.reader = threading.Thread(target=self.read_loop, args=(self.port,), daemon=True)
        self.reader.start()

    def read_loop(self, port):
        while not self.stop_reader.is_set():
            try:
                frame = read_packed_frame(port)
            except (serial.SerialException, OSError) as exc:
                if not self.stop_reader.is_set():
                    self.errors.put(str(exc))
                break
            if self.stop_reader.is_set():
                break
            try:
                self.frames.put_nowait(frame)
            except queue.Full:
                try:
                    self.frames.get_nowait()
                except queue.Empty:
                    pass
                self.frames.put_nowait(frame)

    def poll_frames(self):
        while not self.errors.empty():
            error = self.errors.get_nowait()
            self.disconnect()
            self.status.set(f"接收中断：{error}")
        while not self.frames.empty():
            self.current_frame = self.frames.get_nowait()
        if self.current_frame is not None:
            now = time.monotonic()
            if now >= self.next_display_at:
                self.draw_frame()
                try:
                    seconds = float(self.interval.get())
                except ValueError:
                    seconds = 1.0
                self.next_display_at = now + max(0.1, seconds)
        self.root.after(100, self.poll_frames)

    def capture_baseline(self):
        if self.current_frame is None:
            messagebox.showinfo("记录零点", "请先连接并收到一帧数据")
            return
        self.baseline = list(self.current_frame[1])
        self.subtract_baseline.set(True)
        self.draw_frame()

    def draw_frame(self):
        if self.current_frame is None:
            return
        sequence, values = self.current_frame
        try:
            maximum = max(1.0, float(self.color_max.get()))
        except ValueError:
            maximum = 100.0
        heatmap = self.view_mode.get() == "热力图"
        relative = self.subtract_baseline.get() and self.baseline is not None
        for row in range(ROWS):
            for col in range(COLS):
                index = source_row_for_display(row) * COLS + col
                raw = values[index]
                cell = self.cells[row][col]
                if raw == INVALID:
                    cell.configure(text="ERR", bg="#d4d4d4", fg="#333333")
                    continue
                value = raw
                if relative and self.baseline[index] != INVALID:
                    value = max(0, raw - self.baseline[index])
                if heatmap:
                    color, rgb = heat_color(value, maximum)
                    luminance = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]
                    cell.configure(text=str(value), bg=color,
                                   fg="black" if luminance > 145 else "white")
                else:
                    cell.configure(text=str(value), bg="white", fg="black")
        label = "相对零点增量" if relative else "0～255 原始显示码"
        self.status.set(f"{self.port_name.get()} | 帧号 {sequence} | {label} | 每 {self.interval.get()} 秒刷新")

    def disconnect(self):
        self.stop_reader.set()
        if self.port is not None:
            self.port.close()
            self.port = None
        while not self.frames.empty():
            self.frames.get_nowait()
        self.connect_button.configure(text="连接")
        self.current_frame = None
        self.status.set("已断开")

    def close(self):
        self.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    MatrixViewer(root)
    root.mainloop()


if __name__ == "__main__":
    main()
