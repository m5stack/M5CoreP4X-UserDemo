#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
M5 PY32BIN Tools GUI

基于 PyQt6 的可视化固件升级工具
支持 PM1 和 IOExt 设备

依赖: pip install pyqt6 pyserial
"""

import sys
import hashlib
import re
import time
import threading
import queue
import struct
from pathlib import Path
from typing import Optional, Tuple

try:
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QTabWidget, QGroupBox, QLabel, QLineEdit, QPushButton, QComboBox,
        QSpinBox, QTextEdit, QFileDialog, QMessageBox, QProgressBar, QCheckBox
    )
    from PyQt6.QtCore import Qt, pyqtSignal, QObject
    from PyQt6.QtGui import QFont
except ImportError:
    print("错误: 未安装 PyQt6，请运行: pip install pyqt6")
    sys.exit(1)

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("错误: 未安装 pyserial，请运行: pip install pyserial")
    sys.exit(1)


# ============================================================================
# PM1 固件常量
# ============================================================================

PM1_FIRMWARE_SIZE = 20 * 1024  # 固件总大小 20KB
PM1_CRC_SIZE = 4               # CRC32 占用 4 字节
PM1_DATA_SIZE = PM1_FIRMWARE_SIZE - PM1_CRC_SIZE  # 数据区大小 20476 字节


# ============================================================================
# CRC32 计算 (与 PY32 Bootloader 兼容)
# ============================================================================

def compute_crc32_py32(data: bytes) -> int:
    """
    计算 PY32 Bootloader 兼容的 CRC32
    
    使用 CRC-32/MPEG-2 算法:
    - 多项式: 0x04C11DB7
    - 初始值: 0xFFFFFFFF
    - 输入反转: 无
    - 输出反转: 无
    - 最终异或: 无
    
    这与 PY32 硬件 CRC 单元 + __REV() 字节反转的效果等价。
    """
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def prepare_firmware_with_crc(raw_data: bytes) -> bytes:
    """
    准备带 CRC32 的固件:
    1. 如果数据 < 20476 字节，填充 0xFF 到 20476 字节
    2. 如果数据 >= 20480 字节且最后 4 字节看起来像 CRC，直接返回前 20480 字节
    3. 计算 CRC32 并附加到末尾
    """
    # 如果固件已经是 20KB 且末尾有填充，可能已包含 CRC
    if len(raw_data) >= PM1_FIRMWARE_SIZE:
        # 检查是否已经是正确格式
        return raw_data[:PM1_FIRMWARE_SIZE]
    
    # 截取或填充到数据区大小
    if len(raw_data) > PM1_DATA_SIZE:
        data = raw_data[:PM1_DATA_SIZE]
    else:
        # 填充 0xFF
        data = raw_data + bytes([0xFF] * (PM1_DATA_SIZE - len(raw_data)))
    
    # 计算 CRC32
    crc = compute_crc32_py32(data)
    
    # 附加 CRC32 (小端序)
    firmware = data + struct.pack('<I', crc)
    
    return firmware


# ============================================================================
# 固件解析工具
# ============================================================================

def parse_c_header_array(file_path: Path) -> bytes:
    """从 C 头文件中提取 appHexData[] 数组"""
    content = file_path.read_text(encoding='utf-8', errors='ignore')
    pattern = r'appHexData\s*\[\s*\]\s*=\s*\{([^}]+)\}'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        raise ValueError(f"找不到 'appHexData[]' 数组")
    hex_values = re.findall(r'0x([0-9A-Fa-f]{2})', match.group(1))
    if not hex_values:
        raise ValueError("appHexData[] 中没有十六进制数据")
    return bytes([int(hv, 16) for hv in hex_values])


def parse_intel_hex(file_path: Path) -> bytes:
    """解析 Intel HEX 格式文件"""
    data_dict = {}
    base_address = 0
    with open(file_path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line.startswith(':'):
                continue
            byte_count = int(line[1:3], 16)
            address = int(line[3:7], 16)
            record_type = int(line[7:9], 16)
            data_hex = line[9:9 + byte_count * 2]
            if record_type == 0x00:
                addr = base_address + address
                for i, b in enumerate(bytes.fromhex(data_hex)):
                    data_dict[addr + i] = b
            elif record_type == 0x04:
                base_address = int(data_hex, 16) << 16
            elif record_type == 0x01:
                break
    if not data_dict:
        raise ValueError("HEX 文件中没有数据")
    min_addr, max_addr = min(data_dict.keys()), max(data_dict.keys())
    binary = bytearray(max_addr - min_addr + 1)
    for addr, val in data_dict.items():
        binary[addr - min_addr] = val
    return bytes(binary)


def load_firmware(file_path: Path) -> Tuple[bytes, str]:
    """加载固件文件，自动检测格式"""
    suffix = file_path.suffix.lower()
    if suffix == '.h':
        return parse_c_header_array(file_path), "C Header"
    elif suffix == '.hex':
        return parse_intel_hex(file_path), "Intel HEX"
    else:
        return file_path.read_bytes(), "Binary"


def compute_md5(data: bytes) -> str:
    """计算 MD5"""
    return hashlib.md5(data).hexdigest()


def list_serial_ports() -> list:
    """获取可用串口列表"""
    return [p.device for p in serial.tools.list_ports.comports()]


# ============================================================================
# 信号类
# ============================================================================

class WorkerSignals(QObject):
    """工作线程信号"""
    log = pyqtSignal(str)
    progress = pyqtSignal(int)
    finished = pyqtSignal(bool, str)


# ============================================================================
# PM1 设备页面
# ============================================================================

class Pm1DevicePage(QWidget):
    """PM1 固件升级页面"""
    
    def __init__(self):
        super().__init__()
        self.firmware_data: Optional[bytes] = None
        self.serial: Optional[serial.Serial] = None
        self.serial_lock = threading.Lock()
        self.monitor_running = False
        self.response_queues = []
        self._crc_error = False
        
        self.signals = WorkerSignals()
        self.signals.log.connect(self.append_log)
        self.signals.progress.connect(self.update_progress)
        self.signals.finished.connect(self.on_finished)
        self.init_ui()
    
    def init_ui(self):
        layout = QVBoxLayout()
        
        # 串口配置
        port_group = QGroupBox("串口配置")
        port_layout = QHBoxLayout()
        
        port_layout.addWidget(QLabel("串口:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(120)
        self.port_combo.currentIndexChanged.connect(self.on_port_changed)
        port_layout.addWidget(self.port_combo)
        
        self.refresh_btn = QPushButton("刷新")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        port_layout.addWidget(self.refresh_btn)
        
        port_layout.addWidget(QLabel("波特率:"))
        self.baudrate_spin = QSpinBox()
        self.baudrate_spin.setRange(9600, 921600)
        self.baudrate_spin.setValue(115200)
        self.baudrate_spin.valueChanged.connect(self.update_baudrate)
        port_layout.addWidget(self.baudrate_spin)
        
        # 串口控制按钮
        self.open_btn = QPushButton("打开串口")
        self.open_btn.clicked.connect(self.toggle_serial)
        port_layout.addWidget(self.open_btn)
        
        self.reset_btn = QPushButton("软件复位")
        self.reset_btn.clicked.connect(self.software_reset)
        port_layout.addWidget(self.reset_btn)
        
        port_layout.addStretch()
        port_group.setLayout(port_layout)
        layout.addWidget(port_group)
        
        # 固件配置
        fw_group = QGroupBox("固件配置")
        fw_layout = QVBoxLayout()
        
        file_layout = QHBoxLayout()
        file_layout.addWidget(QLabel("固件文件:"))
        self.file_edit = QLineEdit()
        self.file_edit.setPlaceholderText("支持 app_without_flash.h 格式")
        file_layout.addWidget(self.file_edit)
        self.browse_btn = QPushButton("浏览...")
        self.browse_btn.clicked.connect(self.browse_file)
        file_layout.addWidget(self.browse_btn)
        fw_layout.addLayout(file_layout)
        
        info_layout = QHBoxLayout()
        self.file_info = QLabel("未选择文件")
        info_layout.addWidget(self.file_info)
        info_layout.addStretch()
        fw_layout.addLayout(info_layout)
        
        md5_layout = QHBoxLayout()
        md5_layout.addWidget(QLabel("MD5:"))
        self.md5_edit = QLineEdit()
        self.md5_edit.setPlaceholderText("自动计算 (或手动输入)")
        self.md5_edit.setMaxLength(32)
        md5_layout.addWidget(self.md5_edit)
        fw_layout.addLayout(md5_layout)
        
        # CRC 选项
        crc_layout = QHBoxLayout()
        self.auto_crc_checkbox = QCheckBox("自动填充到20KB并添加CRC32")
        self.auto_crc_checkbox.setChecked(False)
        self.auto_crc_checkbox.stateChanged.connect(self.on_crc_option_changed)
        crc_layout.addWidget(self.auto_crc_checkbox)
        self.crc_hint_label = QLabel("")
        self.crc_hint_label.setStyleSheet("color: red; font-weight: bold;")
        crc_layout.addWidget(self.crc_hint_label)
        crc_layout.addStretch()
        fw_layout.addLayout(crc_layout)
        
        # Reset Option
        reset_layout = QHBoxLayout()
        self.reset_before_flash_checkbox = QCheckBox("烧录前软件复位主机")
        self.reset_before_flash_checkbox.setChecked(False)
        reset_layout.addWidget(self.reset_before_flash_checkbox)
        reset_layout.addStretch()
        fw_layout.addLayout(reset_layout)
        
        fw_group.setLayout(fw_layout)
        layout.addWidget(fw_group)
        
        # 升级选项
        opt_group = QGroupBox("升级选项")
        opt_layout = QHBoxLayout()
        
        opt_layout.addWidget(QLabel("发送延迟(ms):"))
        self.delay_spin = QSpinBox()
        self.delay_spin.setRange(10, 1000)
        self.delay_spin.setValue(100)
        opt_layout.addWidget(self.delay_spin)
        
        opt_layout.addStretch()
        opt_group.setLayout(opt_layout)
        layout.addWidget(opt_group)
        
        # 进度条
        self.progress_bar = QProgressBar()
        self.progress_bar.setRange(0, 100)
        self.progress_bar.setValue(0)
        layout.addWidget(self.progress_bar)
        
        # 控制按钮
        btn_layout = QHBoxLayout()
        self.start_btn = QPushButton("开始升级")
        self.start_btn.clicked.connect(self.start_upgrade)
        self.start_btn.setStyleSheet("QPushButton { padding: 10px; font-weight: bold; }")
        btn_layout.addWidget(self.start_btn)
        layout.addLayout(btn_layout)
        
        # 日志输出
        log_group = QGroupBox("日志")
        log_layout = QVBoxLayout()
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setFont(QFont("Consolas", 9))
        log_layout.addWidget(self.log_text)
        log_group.setLayout(log_layout)
        layout.addWidget(log_group)
        
        self.setLayout(layout)
        self.refresh_ports()
    
    def on_port_changed(self):
        """串口改变回调"""
        # 切换串口时关闭当前连接
        if self.serial and self.serial.is_open:
            self.close_serial()

    def update_baudrate(self):
        """波特率改变回调"""
        if self.serial and self.serial.is_open:
            try:
                self.serial.baudrate = self.baudrate_spin.value()
                self.append_log(f"[系统] 波特率更新为 {self.baudrate_spin.value()}")
            except Exception as e:
                self.append_log(f"[错误] 更新波特率失败: {e}")

    def toggle_serial(self):
        """切换串口开关状态"""
        if self.serial and self.serial.is_open:
            self.close_serial()
        else:
            self.open_serial()

    def software_reset(self):
        """软件复位 (RTS High->Low, Default Low)"""
        if self.serial and self.serial.is_open:
            try:
                # 兼容两种走线: 先做常见的 "低-高-低" 脉冲，再做 "高-低-高" 以覆盖反相接线
                # pyserial: rts=True -> 低电平; rts=False -> 高电平
                self.serial.dtr = False  # 避免 DTR 干扰

                # 脉冲 1: 低 -> 高 -> 低
                self.serial.rts = True   # 低
                time.sleep(0.06)
                self.serial.rts = False  # 高
                time.sleep(0.06)
                self.serial.rts = True   # 低
                time.sleep(0.06)

                # 脉冲 2: 高 -> 低 -> 高 (某些接线需要反向)
                self.serial.rts = False  # 高
                time.sleep(0.06)
                self.serial.rts = True   # 低
                time.sleep(0.06)
                self.serial.rts = False  # 高

                # 额外：有的板卡把复位绑在 DTR，做一个短脉冲
                self.serial.dtr = True   # 低
                time.sleep(0.05)
                self.serial.dtr = False  # 高

                self.append_log("[系统] 软件复位 (RTS/DTR Pulse)")
            except Exception as e:
                self.append_log(f"[错误] 复位失败: {e}")
        else:
            self.append_log("[错误] 串口未打开，无法复位")

    def open_serial(self):
        """打开串口"""
        self.close_serial()
        port = self.port_combo.currentText()
        if not port:
            return
            
        try:
            baud = self.baudrate_spin.value()
            # rtscts=False 禁用硬件流控
            self.serial = serial.Serial(port, baud, timeout=0.1, rtscts=False)
            
            # 默认状态: RTS 高(不复位)，DTR 高，避免误复位
            self.serial.rts = False 
            self.serial.dtr = False
            
            self.monitor_running = True
            self.monitor_thread = threading.Thread(target=self.serial_monitor_task, daemon=True)
            self.monitor_thread.start()
            self.append_log(f"[系统] 已打开串口 {port}")
            
            self.open_btn.setText("关闭串口")
            self.open_btn.setStyleSheet("color: red; font-weight: bold;")
        except Exception as e:
            self.append_log(f"[错误] 无法打开串口 {port}: {e}")
            self.serial = None
            self.open_btn.setText("打开串口")
            self.open_btn.setStyleSheet("")

    def close_serial(self):
        """关闭串口"""
        self.monitor_running = False
        if self.serial:
            try:
                self.serial.close()
            except:
                pass
            self.serial = None
        
        self.open_btn.setText("打开串口")
        self.open_btn.setStyleSheet("")
        self.append_log("[系统] 串口已关闭")

    def serial_monitor_task(self):
        """串口监听线程"""
        while self.monitor_running and self.serial and self.serial.is_open:
            try:
                if self.serial.in_waiting:
                    line = self.serial.readline()
                    if line:
                        try:
                            text = line.decode('utf-8', errors='ignore').strip()
                        except:
                            text = line.hex()
                        
                        if text:
                            # 绿色显示串口日志
                            html = f'<span style="color:green;">[SERIAL] {text}</span>'
                            self.signals.log.emit(html)
                            
                            # 分发给等待的队列
                            with self.serial_lock:
                                for q in self.response_queues:
                                    q.put(text)
                else:
                    time.sleep(0.01)
            except Exception as e:
                # 串口可能被拔出或关闭
                if self.monitor_running:
                    self.signals.log.emit(f"[错误] 串口监听异常: {e}")
                break

    def refresh_ports(self):
        """刷新串口列表"""
        current = self.port_combo.currentText()
        self.port_combo.blockSignals(True)
        self.port_combo.clear()
        ports = list_serial_ports()
        self.port_combo.addItems(ports)
        
        # 尝试恢复之前的选择
        if current in ports:
            self.port_combo.setCurrentText(current)
        elif ports:
            self.port_combo.setCurrentIndex(0)
            
        self.port_combo.blockSignals(False)
        # 刷新后不自动打开，等待用户点击

    
    def browse_file(self):
        """浏览固件文件"""
        path, _ = QFileDialog.getOpenFileName(
            self, "选择固件文件", "",
            "固件文件 (*.bin *.hex *.h);;所有文件 (*.*)"
        )
        if path:
            self.file_edit.setText(path)
            self.load_firmware_file(Path(path))
    
    def load_firmware_file(self, path: Path):
        """加载固件文件"""
        try:
            raw_data, file_type = load_firmware(path)
            raw_size = len(raw_data)
            
            # 保存原始数据用于重新处理
            self._raw_firmware_data = raw_data
            self._firmware_path = path
            self._crc_error = False
            
            # 检查固件 CRC 是否已正确
            crc_status = self._check_firmware_crc(raw_data)
            
            # 根据 CRC 状态设置 checkbox
            if crc_status['valid']:
                # CRC 匹配，不需要自动添加
                self.auto_crc_checkbox.setChecked(False)
                self.crc_hint_label.setText("")
                self.firmware_data = raw_data[:PM1_FIRMWARE_SIZE] if len(raw_data) >= PM1_FIRMWARE_SIZE else raw_data
                self.append_log(f"[CRC32] 固件已包含有效 CRC: 0x{crc_status['stored']:08X}")
            else:
                # CRC 不匹配，需要自动添加
                self._crc_error = True
                self.auto_crc_checkbox.setChecked(True)
                self.crc_hint_label.setText("固件校验错误，若固件无误，请先勾选再烧录")
                self.firmware_data = prepare_firmware_with_crc(raw_data)
                crc_value = compute_crc32_py32(self.firmware_data[:-PM1_CRC_SIZE])
                self.append_log(f"[CRC32] {crc_status['reason']}")
                self.append_log(f"[CRC32] 原始: {raw_size} bytes -> 填充+CRC: {len(self.firmware_data)} bytes")
                self.append_log(f"[CRC32] 计算值: 0x{crc_value:08X}")
            
            md5 = compute_md5(self.firmware_data)
            self.md5_edit.setText(md5)
            final_size = len(self.firmware_data)
            self.file_info.setText(f"{file_type} | 原始: {raw_size} bytes | 最终: {final_size} bytes | MD5: {md5[:16]}...")
            self.append_log(f"[加载] {path.name}: {raw_size} bytes ({file_type})")
        except Exception as e:
            self.firmware_data = None
            self._raw_firmware_data = None
            self.md5_edit.clear()
            self.crc_hint_label.setText("")
            self.file_info.setText(f"错误: {e}")
            QMessageBox.warning(self, "加载失败", str(e))
    
    def _check_firmware_crc(self, raw_data: bytes) -> dict:
        """
        检查固件 CRC 是否有效
        返回: {'valid': bool, 'reason': str, 'stored': int, 'calculated': int}
        """
        # 固件必须至少 20KB 才可能包含有效 CRC
        if len(raw_data) < PM1_FIRMWARE_SIZE:
            return {
                'valid': False,
                'reason': f"固件不足20KB ({len(raw_data)} bytes)",
                'stored': 0,
                'calculated': 0
            }
        
        # 提取数据区和存储的 CRC
        data_part = raw_data[:PM1_DATA_SIZE]
        stored_crc = int.from_bytes(raw_data[PM1_DATA_SIZE:PM1_FIRMWARE_SIZE], 'little')
        
        # 计算 CRC
        calculated_crc = compute_crc32_py32(data_part)
        
        if stored_crc == calculated_crc:
            return {
                'valid': True,
                'reason': '',
                'stored': stored_crc,
                'calculated': calculated_crc
            }
        else:
            return {
                'valid': False,
                'reason': f"CRC不匹配: 存储0x{stored_crc:08X} != 计算0x{calculated_crc:08X}",
                'stored': stored_crc,
                'calculated': calculated_crc
            }
    
    def on_crc_option_changed(self, state):
        """CRC 选项变化时重新处理固件"""
        if not hasattr(self, '_raw_firmware_data') or not self._raw_firmware_data:
            return
        
        raw_data = self._raw_firmware_data
        raw_size = len(raw_data)
        
        if self.auto_crc_checkbox.isChecked():
            # 需要添加 CRC
            self.firmware_data = prepare_firmware_with_crc(raw_data)
            crc_value = compute_crc32_py32(self.firmware_data[:-PM1_CRC_SIZE])
            self.append_log(f"[CRC32] 原始: {raw_size} bytes -> 填充+CRC: {len(self.firmware_data)} bytes")
            self.append_log(f"[CRC32] 计算值: 0x{crc_value:08X}")
            if self._crc_error:
                self.crc_hint_label.setText("固件校验错误，若固件无误，请先勾选再烧录")
        else:
            # 不添加 CRC，使用原始数据
            self.firmware_data = raw_data
            if not self._crc_error:
                self.crc_hint_label.setText("")  # 仅在无错误状态下清除提示
        
        md5 = compute_md5(self.firmware_data)
        self.md5_edit.setText(md5)
        final_size = len(self.firmware_data)
        self.file_info.setText(f"原始: {raw_size} bytes | 最终: {final_size} bytes | MD5: {md5[:16]}...")
    
    def append_log(self, text: str):
        """添加日志"""
        self.log_text.append(text)
        self.log_text.verticalScrollBar().setValue(
            self.log_text.verticalScrollBar().maximum()
        )
    
    def update_progress(self, value: int):
        """更新进度条"""
        self.progress_bar.setValue(value)
    
    def on_finished(self, success: bool, msg: str):
        """升级完成回调"""
        self.start_btn.setEnabled(True)
        self.progress_bar.setValue(100 if success else 0)
        if success:
            QMessageBox.information(self, "成功", msg)
        else:
            QMessageBox.warning(self, "失败", msg)
    
    def start_upgrade(self):
        """开始升级"""
        if not self.firmware_data:
            QMessageBox.warning(self, "错误", "请先选择固件文件")
            return
        
        if not self.serial or not self.serial.is_open:
            QMessageBox.warning(self, "错误", "串口未打开")
            return
        
        md5 = self.md5_edit.text().strip().lower()
        if len(md5) != 32:
            QMessageBox.warning(self, "错误", "MD5 必须是 32 位十六进制")
            return
        
        self.start_btn.setEnabled(False)
        self.progress_bar.setValue(0)
        self.append_log(f"\n[升级] 开始升级流程...")
        
        reset_before = self.reset_before_flash_checkbox.isChecked()

        # 在后台线程执行
        thread = threading.Thread(
            target=self.do_upgrade,
            args=(md5, self.firmware_data, self.delay_spin.value(), reset_before),
            daemon=True
        )
        thread.start()
    
    def do_upgrade(self, md5: str, firmware: bytes, delay_ms: int, reset_before: bool):
        """执行升级（后台线程）"""
        resp_queue = queue.Queue()
        with self.serial_lock:
            self.response_queues.append(resp_queue)
            
        try:
            if reset_before:
                self.signals.log.emit("[系统] 执行烧录前复位 (RTS Pulse)...")
                if self.serial and self.serial.is_open:
                    self.serial.dtr = False
                    self.serial.rts = True  # Low (Reset)
                    time.sleep(0.1)
                    self.serial.rts = False # High (Run)
                    time.sleep(0.5)
                else:
                    self.signals.log.emit("[警告] 串口未打开，跳过复位")

            def check_for_errors(line):
                # 检查明确的错误日志
                # 例如: [SERIAL] E (24299) py32-pm1: non-hex character detected while parsing MD5
                if "E (" in line and "py32-pm1" in line:
                    return line
                return None

            def wait_for_keywords(keywords, timeout):
                start = time.time()
                while time.time() - start < timeout:
                    try:
                        line = resp_queue.get(timeout=0.1)
                        
                        # 检查错误
                        err = check_for_errors(line)
                        if err:
                            raise Exception(f"设备报错: {err}")

                        for kw in keywords:
                            if kw in line:
                                return True
                    except queue.Empty:
                        continue
                return False

            # 等待 ESP32 提示
            self.signals.log.emit("[等待] ESP32 响应 (请复位设备)...")
            # 这里不强制阻塞，因为可能已经错过了启动信息，直接尝试发送 MD5
            wait_for_keywords(["[iap]"], 2)

            # 发送 MD5
            self.signals.log.emit(f"[发送] MD5: {md5}")
            self.serial.write((md5 + '\n').encode('ascii'))
            
            # 等待确认
            time.sleep(delay_ms / 1000.0)
            # 这里也不强制报错，继续尝试发送
            wait_for_keywords(["MD5 OK", "Need firmware"], 2)
            
            # 发送固件 (进度 0-50%)
            self.signals.log.emit(f"[发送] 固件 {len(firmware)} bytes...")
            chunk_size = 1024
            total_chunks = (len(firmware) + chunk_size - 1) // chunk_size
            
            for i, offset in enumerate(range(0, len(firmware), chunk_size)):
                if not self.serial or not self.serial.is_open:
                    raise Exception("串口已断开")
                
                # 检查是否有错误 (非阻塞)
                while not resp_queue.empty():
                    try:
                        line = resp_queue.get_nowait()
                        err = check_for_errors(line)
                        if err:
                            raise Exception(f"设备报错: {err}")
                    except queue.Empty:
                        break

                chunk = firmware[offset:offset + chunk_size]
                self.serial.write(chunk)
                
                # 进度 0-50%
                progress = int((i + 1) / total_chunks * 50)
                self.signals.progress.emit(progress)
                time.sleep(0.01)
            
            self.signals.log.emit("[等待] ESP32 烧录中 (进度 50-100%)...")
            
            # 等待完成并解析进度 (进度 50-100%)
            start = time.time()
            success = False
            while time.time() - start < 90:
                try:
                    line = resp_queue.get(timeout=0.1)
                    
                    # 检查错误
                    err = check_for_errors(line)
                    if err:
                        raise Exception(f"设备报错: {err}")

                    # 解析烧录进度: [iap] chunk 001/160 (0.62%)
                    if "[iap] chunk" in line:
                        match = re.search(r'\((\d+\.\d+)%\)', line)
                        if match:
                            percent = float(match.group(1))
                            # 映射 0-100% 到 50-100%
                            total_progress = 50 + int(percent / 2)
                            self.signals.progress.emit(total_progress)
                    
                    # 检查完成标志
                    if "[iap] OK" in line:
                        success = True
                        break
                        
                except queue.Empty:
                    continue
            
            if success:
                self.signals.progress.emit(100)
                self.signals.finished.emit(True, "固件升级成功!")
            else:
                self.signals.finished.emit(False, "升级超时或未检测到完成标志 ([iap] OK)")
                
        except Exception as e:
            self.signals.log.emit(f"[错误] {e}")
            self.signals.finished.emit(False, str(e))
        finally:
            with self.serial_lock:
                if resp_queue in self.response_queues:
                    self.response_queues.remove(resp_queue)


# ============================================================================
# IOExt 设备页面 (暂留空)
# ============================================================================

class IoExtDevicePage(QWidget):
    """IOExt GPIO 扩展器页面 (暂未实现)"""
    
    def __init__(self):
        super().__init__()
        self.init_ui()
    
    def init_ui(self):
        layout = QVBoxLayout()
        
        placeholder = QLabel("IOExt 设备功能暂未实现")
        placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        placeholder.setStyleSheet("color: gray; font-size: 14px;")
        layout.addWidget(placeholder)
        
        layout.addStretch()
        self.setLayout(layout)


# ============================================================================
# 主窗口
# ============================================================================

class MainWindow(QMainWindow):
    """主窗口"""
    
    def __init__(self):
        super().__init__()
        self.setWindowTitle("M5 PY32BIN Tools GUI")
        self.setMinimumSize(600, 500)
        self.init_ui()
    
    def init_ui(self):
        central = QWidget()
        layout = QVBoxLayout()
        
        # 标签页
        tabs = QTabWidget()
        tabs.addTab(Pm1DevicePage(), "PM1 电源管理")
        tabs.addTab(IoExtDevicePage(), "IOExt GPIO扩展")
        layout.addWidget(tabs)
        
        central.setLayout(layout)
        self.setCentralWidget(central)


# ============================================================================
# 入口
# ============================================================================

def main():
    app = QApplication(sys.argv)
    app.setStyle('Fusion')
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == '__main__':
    main()
