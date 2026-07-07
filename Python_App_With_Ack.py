import tkinter as tk
from tkinter import filedialog, ttk
from PIL import Image, ImageTk
import os
import sys
import can
import time

def resource_path(relative_path):
    if hasattr(sys, '_MEIPASS'):
        return os.path.join(sys._MEIPASS, relative_path)
    return os.path.abspath(relative_path)

logo_path = resource_path("Logo_Black.png")
ecus = {
    "CMSCharger_180": {
        "boot_id": 0x12EF34AB,
        "key": 0x5A5A5A5A5A5A5A5A,
        "response_id": 0x12EF34AA,
        "response_key": 0xA5A5A5A5A5A5A5A5,
        "writeSuccKey":0x0000000000000000,
        "block_ack": 0x5555555555555555,
        "backup_key": 0x8000000000000000
    },
}

can_status_msgs = {
    bytes([1, 0, 0, 0, 0, 0, 0, 0]): "MCU is in Bootloader",
    bytes([2, 0, 0, 0, 0, 0, 0, 0]): "No firmware available. Jumping to main",
    bytes([3, 0, 0, 0, 0, 0, 0, 0]): "Flash API Initialized",
    bytes([4, 0, 0, 0, 0, 0, 0, 0]): "Flash erase successful",
    bytes([5, 0, 0, 0, 0, 0, 0, 0]): "Firmware & backup updated successfully",
    bytes([6, 0, 0, 0, 0, 0, 0, 0]): "Jumping to App after FW update success",
    bytes([7, 0, 0, 0, 0, 0, 0, 0]): "Initial backup created (none found)",
    bytes([8, 0, 0, 0, 0, 0, 0, 0]): "Jump to App due to no CAN msg",
    bytes([9, 0, 0, 0, 0, 0, 0, 0]): "CAN Communication Cut",
    bytes([10, 0, 0, 0, 0, 0, 0, 0]): "Flash API failed, retrying",
    bytes([11, 0, 0, 0, 0, 0, 0, 0]): "Flash API Init failed after 5 attempts",
    bytes([12, 1, 0, 0, 0, 0, 0, 0]): "Initial backup failed, continuing",
    bytes([12, 2, 0, 0, 0, 0, 0, 0]): "Backup clear success, but copy failed",
    bytes([13, 0, 0, 0, 0, 0, 0, 0]): "Flash erase failed",
    bytes([14, 0, 0, 0, 0, 0, 0, 0]): "Flash erase failed, restored backup",
    bytes([15, 0, 0, 0, 0, 0, 0, 0]): "Backup also failed. Permanent fault",
    bytes([0xF, 0xF, 0, 0, 0, 0, 0, 0]): "Backup erase failed",
    bytes([2, 2, 2, 2, 2, 2, 2, 2]): "Backup copied, but verify failed",
    bytes([1, 1, 1, 1, 1, 1, 1, 1]): "No data in flash, copied from backup",
    bytes([0xA5]*8): "✅ Flash erase confirmation received",
    bytes([0xAB]*8): "❌ Flash erase failed",
    bytes([0x55]*8): "✅ Block ACK received"
}
class FirmwareUpdater:
    def __init__(self, master):
        self.master = master
        master.title("Tetra TMS320F28004C CAN Firmware Updater")
        self.setup_ui()
        self.can_bus = None
        self.hex_file_path = None
        self.blocks = []
        self.entry_point = 0
        self.backup_loaded = False
        self.block_bars = []
        self.block_labels = []
        self.init_can_bus()

    def setup_ui(self):
        sw, sh = self.master.winfo_screenwidth(), self.master.winfo_screenheight()
        w, h = int(sw * 0.5), int(sh * 0.8)
        self.master.geometry(f"{w}x{h}+{int((sw - w)//2)}+{int((sh - h)//2)}")

        if os.path.exists(logo_path):
            try:
                img = Image.open(logo_path)
                logo_w = int(w * 0.3)
                logo_h = int(logo_w * img.height / img.width)
                img = img.resize((logo_w, logo_h), Image.Resampling.LANCZOS)
                self.logo = ImageTk.PhotoImage(img)
                tk.Label(self.master, image=self.logo).pack(pady=5)
            except Exception:
                tk.Label(self.master, text="[Logo failed]").pack()
        else:
            tk.Label(self.master, text="[Logo not found]").pack()

        self.ecu_var = tk.StringVar()
        self.ecu_var.set(next(iter(ecus)))
        tk.Label(self.master, text="Select ECU").pack()
        tk.OptionMenu(self.master, self.ecu_var, *ecus).pack(pady=5)

        self.select_hex_btn = tk.Button(self.master, text="Select HEX File", command=self.select_hex_file, state="disabled")
        self.select_hex_btn.pack(pady=5)
        self.hex_file_label = tk.Label(self.master, text="No file selected", fg="gray")
        self.hex_file_label.pack()

        self.start_btn = tk.Button(self.master, text="Start Update", command=self.send_firmware_info, state="disabled")
        self.start_btn.pack(pady=10)

        self.progress_frame = tk.Frame(self.master)
        self.progress_frame.pack(pady=5, fill=tk.X, padx=10)

        self.status_frame = tk.Frame(self.master)
        self.status_frame.pack(pady=10, fill=tk.BOTH, expand=False)
        tk.Label(self.status_frame, text="Status Log:").pack(anchor="w")
        self.status_text = tk.Text(self.status_frame, height=6, wrap="word", bg="#f5f5f5", fg="black", font=("Arial", 10))
        self.status_text.pack(fill=tk.BOTH, expand=True, padx=10)
        self.status_text.insert(tk.END, "🔌 Initializing CAN...\n")
        self.status_text.config(state="disabled")

    def log_status(self, message, color="black"):
        self.status_text.config(state="normal")
        self.status_text.insert(tk.END, message + "\n")
        self.status_text.tag_add(color, f"end-{len(message)+1}c", "end-1c")
        self.status_text.tag_config(color, foreground=color)
        self.status_text.see(tk.END)
        self.status_text.config(state="disabled")

    def init_can_bus(self):
        try:
            self.can_bus = can.Bus(interface='pcan', channel='PCAN_USBBUS1', bitrate=125000)
            self.log_status("✅ CAN connected successfully!", "green")
            self.select_hex_btn.config(state="normal")
        except Exception as e:
            self.log_status(f"❌ CAN init failed: {e}", "red")

    def select_hex_file(self):
        path = filedialog.askopenfilename(filetypes=[["HEX files", "*.hex"]])
        if path:
            self.hex_file_path = path
            self.hex_file_label.config(text=f"Selected: {os.path.basename(path)}", fg="green")
            self.parse_hex_file()
        else:
            self.hex_file_label.config(text="No file selected", fg="gray")

    def parse_hex_file(self):
        self.blocks.clear()
        for widget in self.progress_frame.winfo_children():
            widget.destroy()
        self.block_bars.clear()
        self.block_labels.clear()
        try:
            with open(self.hex_file_path, 'r') as f:
                lines = f.readlines()

            def hex_bytes(line):
                return bytearray.fromhex(line[1:].strip())

            def validate_checksum(rec):
                return (sum(rec[:-1]) & 0xFF) == ((~rec[-1] + 1) & 0xFF)

            pos = 0
            while pos < len(lines):
                line = lines[pos].strip()
                if not line.startswith(":"):
                    pos += 1
                    continue

                rec = hex_bytes(line)
                if not validate_checksum(rec):
                    raise ValueError(f"Invalid checksum at line {pos+1}")

                length = rec[0]
                rtype = rec[3]
                data = rec[4:4+length]

                if pos == 0:
                    temp = int.from_bytes(data[18:22], 'little')
                    self.entry_point = ((temp & 0xFFFF) << 16) | ((temp >> 16) & 0xFFFF)

                    size1 = int.from_bytes(data[22:24], 'little')
                    word0 = int.from_bytes(data[24:26], 'little')
                    word1 = int.from_bytes(data[26:28], 'little')
                    data1 = data[28:28 + size1*2]
                    self.blocks.append({'size': size1, 'addr_words': [word0, word1], 'data': data1})
                    pos += 1
                    continue

                if rtype == 1 or length == 0:
                    break

                size_words = int.from_bytes(data[0:2], 'little')
                word0 = int.from_bytes(data[2:4], 'little')
                word1 = int.from_bytes(data[4:6], 'little')
                pool = bytearray(data[6:])
                pos += 1

                while len(pool) < size_words*2:
                    rec2 = hex_bytes(lines[pos].strip())
                    if not validate_checksum(rec2):
                        raise ValueError(f"Invalid checksum at line {pos+1}")
                    pool += rec2[4:4+rec2[0]]
                    pos += 1

                self.blocks.append({'size': size_words, 'addr_words': [word0, word1], 'data': pool[:size_words*2]})

            for i in range(len(self.blocks)):
                if i == len(self.blocks)-1:
                    continue
                label = tk.Label(self.progress_frame, text=f"Block {i+1}", anchor="w")
                label.pack(fill=tk.X, padx=5)
                self.block_labels.append(label)

                bar = ttk.Progressbar(self.progress_frame, orient="horizontal", mode="determinate", maximum=100)
                bar.pack(fill=tk.X, padx=5, pady=2)
                self.block_bars.append(bar)

            self.log_status(f"✅ Parsed {len(self.blocks)} blocks", "green")
            
            for i in range(6):
                hex_words = [f"0x{word:X}" for word in self.blocks[i]['addr_words']]
                print(f"Add words of parsed data: {i} {hex_words}")    
            
            self.start_btn.config(state="normal")

        except Exception as e:
            self.log_status(f"❌ Parse error: {e}", "red")

    def send_firmware_info(self):
        if not self.blocks:
            self.log_status("❌ No blocks to send", "red")
            return
        self.master.after(800, lambda: self.start_btn.config(state="disabled"))
        ecu = ecus[self.ecu_var.get()]
        self.send_can_message(ecu['key'].to_bytes(8, 'big'))
        self.log_status("✅ KEY SENT TO MCU, WAITING FOR APPLICATION ACK!", "blue")
        self.listen_for_fw_init()

    def listen_for_fw_init(self):
        ecu = ecus[self.ecu_var.get()]
        while True:
            msg = self.can_bus.recv()
            if msg and msg.arbitration_id == ecu['response_id']:
                if list(msg.data) == [0]*8:
                    self.log_status("✅ MCU IS IN THE APPLICATION", "green")
                    self.listen_for_flash_in_boot()
                    return

    def listen_for_flash_in_boot(self):
        ecu = ecus[self.ecu_var.get()]
        while True:
            msg = self.can_bus.recv()
            if msg and msg.arbitration_id == ecu['response_id']:
                if list(msg.data) == [1, 0, 0, 0, 0, 0, 0, 0]:
                    self.log_status("✅ MCU IS IN BOOTLOADER", "green")
                    self.listen_for_flash_init_boot()
                    return

    def listen_for_flash_init_boot(self):
        ecu = ecus[self.ecu_var.get()]
        while True:
            msg = self.can_bus.recv()
            if msg and msg.arbitration_id == ecu['response_id']:
                if list(msg.data) == [3, 0, 0, 0, 0, 0, 0, 0]:
                    self.log_status("✅ MCU FLASH INIT", "green")
                    self.listen_for_flash_erase_success()
                    return
                elif list(msg.data) == [2, 0, 0, 0, 0, 0, 0, 0]:
                    self.log_status("FAILED AND GAVE 2,0,0,0,0,0,0,0", "green")
                    self.master.after(5000, self.master.destroy)     

    def listen_for_flash_erase_success(self):
        ecu = ecus[self.ecu_var.get()]
        while True:
            msg = self.can_bus.recv()
            if msg and msg.arbitration_id == ecu['response_id']:
                if list(msg.data) == [4, 0, 0, 0, 0, 0, 0, 0]:
                    self.log_status("✅ MCU FLASH ERASE SUCCESS", "green")
                    self.wait_for_response_key()
                    return

    def wait_for_response_key(self):
        ecu = ecus[self.ecu_var.get()]
        while True:
            msg = self.can_bus.recv()
            if msg and msg.arbitration_id == ecu['response_id']:
                if int.from_bytes(msg.data, 'big') == ecu['response_key']:
                    self.log_status("✅ FIRMWARE LOADING", "green")
                    self.block_index = 0
                    time.sleep(0.05)
                    self.start_transfer()
                    return

    def start_transfer(self):
        self.send_block()

    def wait_for_msg(self):
        ecu = ecus[self.ecu_var.get()]       # select current ECU config based on GUI variable
        start_time = time.time()             # record timestamp (for future timeout logic, if added)
        
        while True:                                 # loop indefinitely until matching msg found
            msg = self.can_bus.recv()     # wait up to 0.2 s for a CAN frame :contentReference[oaicite:1]{index=1}
            if msg and msg.arbitration_id == ecu['response_id']:  # check message exists & matches expected CAN ID
                value = int.from_bytes(msg.data, 'big')           # convert raw data bytes to integer
                if value == ecu['writeSuccKey']:                  # check if the value equals the success-key
                    print("Message received: {}".format(value))
                    # self.master.after(1, self.send_block)         # schedule send_block in GUI after 1 ms :contentReference[oaicite:2]{index=2}
                    break                                         # exit loop now that success condition is met

    def send_block(self):
        if self.backup_loaded:
            return
        if self.block_index >= len(self.blocks):
            return self.send_end_marker()
        if self.block_index == len(self.blocks)-1:
            self.log_status("Sending END key", "blue")
            return self.send_end_marker()

        blk = self.blocks[self.block_index]
        ecu = ecus[self.ecu_var.get()]
        size_bytes = blk['size'].to_bytes(2, 'big')
        addr_bytes = blk['addr_words'][0].to_bytes(2, 'big') + blk['addr_words'][1].to_bytes(2, 'big')
        self.send_can_message(size_bytes + addr_bytes + b'\xFF\xFF')

        raw = blk['data']
        frames = []
        for i in range(0, len(raw), 2):
            word = int.from_bytes(raw[i:i+2], 'little')
            frames.extend(word.to_bytes(2, 'big'))

        sent = 0
        for i in range(0, len(frames), 8):
            chunk = bytearray(frames[i:i+8])
            chunk += b'\xFF' * (8 - len(chunk))
            self.send_can_message(chunk)
            sent += 1
            progress = int((sent / ((len(frames)+7)//8)) * 100)
            self.block_bars[self.block_index]['value'] = progress
            self.master.update_idletasks()
            self.wait_for_msg()
            # time.sleep(0.05)

        self.wait_for_block_ack()

    

    def wait_for_block_ack(self):
        ecu = ecus[self.ecu_var.get()]
        start_time = time.time()
        while time.time() - start_time < 45.0:
            msg = self.can_bus.recv(timeout=1.0)
            if msg and msg.arbitration_id == ecu['response_id']:
                value = int.from_bytes(msg.data, 'big')
                if value == ecu['block_ack']:
                    self.block_labels[self.block_index].config(text=f"Block {self.block_index + 1} success ✅")
                    self.log_status("✅ BLOCK ACK RECEIVED", "green")
                    self.block_index += 1
                    self.master.after(1, self.send_block)
                    return
                elif value == [0xAB] * 8:
                    self.log_status("🛑 ABORT signal received (ABABABAB) ,Stopping communication!", "red")
                    self.master.after(5000, self.master.destroy)

        self.backup_loaded = True
        self.log_status("✅ Backup is loaded successfully", "orange")

    def send_can_message(self, data):
        print(f"[CAN SEND] {' '.join(f'{b:02X}' for b in data)}")
        msg = can.Message(
            arbitration_id=ecus[self.ecu_var.get()]['boot_id'],
            data=data,
            is_extended_id=True
        )
        try:
            self.can_bus.send(msg)
        except Exception as e:
            print("[SEND ERROR]", e)

    def send_end_marker(self):
        ecu = ecus[self.ecu_var.get()]
        end = ecu['response_key'].to_bytes(8, 'big')
        self.send_can_message(end)
        time.sleep(0.2)
        self.send_can_message(end)
        if not self.backup_loaded:
            self.log_status("✅ FIRMWARE UPDATE COMPLETED", "green")
        self.master.after(5000, self.master.destroy)

if __name__ == "__main__":
    root = tk.Tk()
    app = FirmwareUpdater(root)
    root.mainloop()
