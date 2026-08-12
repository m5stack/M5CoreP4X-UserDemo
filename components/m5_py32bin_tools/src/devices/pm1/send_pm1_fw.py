#!/usr/bin/env python3
"""
PM1 Firmware Upload Script via UART Bridge
Sends MD5 hash (32 hex ASCII) + newline + firmware binary to ESP32 UART1
ESP32 forwards firmware to PM1 via I2C bootloader

Supports:
- Binary files (.bin)
- Intel HEX files (.hex)
- C header files (.h) with appHexData[] array
"""

import argparse
import hashlib
import re
import sys
import time
from pathlib import Path
from typing import Tuple

try:
    import serial
except ImportError:
    print("Error: pyserial not installed. Run: pip install pyserial")
    sys.exit(1)


def parse_c_header_array(file_path: Path) -> bytes:
    """
    从 C 头文件中提取 appHexData[] 数组的二进制数据
    
    Args:
        file_path: .h 文件路径
        
    Returns:
        appHexData[] 的二进制内容
    """
    content = file_path.read_text(encoding='utf-8', errors='ignore')
    
    # 查找 appHexData[] 数组定义
    # 匹配: static const unsigned char appHexData[] = { ... };
    pattern = r'appHexData\s*\[\s*\]\s*=\s*\{([^}]+)\}'
    match = re.search(pattern, content, re.DOTALL)
    
    if not match:
        raise ValueError(f"Cannot find 'appHexData[]' array in {file_path}")
    
    array_content = match.group(1)
    
    # 提取所有十六进制数值: 0x58, 0x09, 0x00, 0x20, ...
    hex_values = re.findall(r'0x([0-9A-Fa-f]{2})', array_content)
    
    if not hex_values:
        raise ValueError(f"No hex values found in appHexData[] array")
    
    # 转换为字节数组
    binary_data = bytes([int(hv, 16) for hv in hex_values])
    
    print(f"[parse_c_header] Extracted {len(binary_data)} bytes from appHexData[]")
    return binary_data


def parse_intel_hex(file_path: Path) -> bytes:
    """
    解析 Intel HEX 格式文件
    
    Args:
        file_path: .hex 文件路径
        
    Returns:
        二进制数据
    """
    data_dict = {}
    base_address = 0
    
    with open(file_path, 'r') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            if not line or not line.startswith(':'):
                continue
            
            try:
                byte_count = int(line[1:3], 16)
                address = int(line[3:7], 16)
                record_type = int(line[7:9], 16)
                data_hex = line[9:9 + byte_count * 2]
                
                if record_type == 0x00:  # Data record
                    addr = base_address + address
                    data_bytes = bytes.fromhex(data_hex)
                    for i, b in enumerate(data_bytes):
                        data_dict[addr + i] = b
                        
                elif record_type == 0x04:  # Extended linear address
                    base_address = int(data_hex, 16) << 16
                    
                elif record_type == 0x01:  # End of file
                    break
                    
            except (ValueError, IndexError) as e:
                raise ValueError(f"Invalid HEX format at line {line_num}: {e}")
    
    if not data_dict:
        raise ValueError("No data found in HEX file")
    
    # 转换为连续字节数组
    min_addr = min(data_dict.keys())
    max_addr = max(data_dict.keys())
    size = max_addr - min_addr + 1
    
    binary_data = bytearray(size)
    for addr, byte_val in data_dict.items():
        binary_data[addr - min_addr] = byte_val
    
    print(f"[parse_intel_hex] Extracted {len(binary_data)} bytes (0x{min_addr:08X} - 0x{max_addr:08X})")
    return bytes(binary_data)


def load_firmware(file_path: Path) -> Tuple[bytes, str]:
    """
    加载固件文件,自动检测格式
    
    Args:
        file_path: 固件文件路径
        
    Returns:
        (二进制数据, 文件类型描述)
    """
    suffix = file_path.suffix.lower()
    
    if suffix == '.h':
        # C 头文件 - 提取 appHexData[] 数组
        data = parse_c_header_array(file_path)
        file_type = "C header (appHexData[])"
    elif suffix == '.hex':
        # Intel HEX 格式
        data = parse_intel_hex(file_path)
        file_type = "Intel HEX"
    elif suffix == '.bin':
        # 原始二进制文件
        data = file_path.read_bytes()
        file_type = "Binary"
    else:
        # 尝试作为二进制文件读取
        data = file_path.read_bytes()
        file_type = f"Binary ({suffix})"
    
    return data, file_type


def compute_md5_bytes(data: bytes) -> str:
    """计算字节数据的 MD5 哈希值"""
    return hashlib.md5(data).hexdigest()


def send_firmware(port: str, baudrate: int, md5_hex: str, firmware_data: bytes, delay_ms: int = 100):
    """
    发送固件到 ESP32 UART
    
    Args:
        port: 串口号 (例如 COM3 或 /dev/ttyUSB0)
        baudrate: 波特率 (默认 115200)
        md5_hex: 32位十六进制 MD5 字符串 (appHexData[] 数据的 MD5)
        firmware_data: 固件二进制数据 (从 .h/.hex/.bin 提取的 appHexData[])
        delay_ms: MD5 和固件之间的延迟(毫秒)
    """
    if len(md5_hex) != 32:
        raise ValueError(f"MD5 must be 32 hex characters, got {len(md5_hex)}")
    
    if not all(c in '0123456789abcdefABCDEF' for c in md5_hex):
        raise ValueError("MD5 contains non-hex characters")
    
    firmware_size = len(firmware_data)
    
    print(f"[send_pm1_fw] Opening serial port {port} @ {baudrate} baud...")
    with serial.Serial(port, baudrate, timeout=2) as ser:
        time.sleep(0.5)  # 等待串口稳定
        
        # 读取并显示 ESP32 发送的提示信息
        print("[send_pm1_fw] Waiting for ESP32 prompts...")
        start_time = time.time()
        while time.time() - start_time < 5:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"  ESP32: {line}")
                    if "[iap]" in line and "Example:" in line:
                        break
        
        # 发送 MD5 (32 hex ASCII + \n)
        md5_message = md5_hex.lower() + '\n'
        print(f"[send_pm1_fw] Sending MD5: {md5_hex.lower()}")
        ser.write(md5_message.encode('ascii'))
        ser.flush()
        
        # 等待 ESP32 确认 MD5
        print("[send_pm1_fw] Waiting for MD5 acknowledgment...")
        start_time = time.time()
        md5_acked = False
        while time.time() - start_time < 3:
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"  ESP32: {line}")
                    if "MD5 OK" in line:
                        md5_acked = True
                        break
                    if "Need firmware" in line:
                        print("[send_pm1_fw] ESP32 waiting for firmware payload...")
                        break
        
        if not md5_acked:
            print("[send_pm1_fw] Warning: No MD5 OK confirmation received")
        
        # 延迟后分块发送固件二进制（避免 ESP32 缓冲区溢出）
        time.sleep(delay_ms / 1000.0)
        
        print(f"[send_pm1_fw] Sending firmware binary ({firmware_size} bytes)...")
        
        # 分块发送，每块 512 字节，带延迟（USB-CDC 需要较慢的速度）
        chunk_size = 512
        offset = 0
        while offset < firmware_size:
            # 检查并读取 ESP32 的日志输出，防止缓冲区溢出
            while ser.in_waiting > 0:
                try:
                    line = ser.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        print(f"  ESP32: {line}")
                except Exception:
                    pass

            chunk_end = min(offset + chunk_size, firmware_size)
            chunk = firmware_data[offset:chunk_end]
            ser.write(chunk)
            ser.flush()
            offset = chunk_end
            
            # 打印进度
            if offset % (4 * 1024) == 0 or offset == firmware_size:
                print(f"[send_pm1_fw] Sent {offset}/{firmware_size} bytes ({100*offset//firmware_size}%)")
            
            # 每块之间延迟 15ms，给 ESP32 USB-CDC 足够时间处理
            if offset < firmware_size:
                time.sleep(0.015)
        
        print(f"[send_pm1_fw] Firmware sent ({firmware_size} bytes)")
        
        # 等待 ESP32 处理响应（I2C 传输和烧录过程可能较慢）
        print("[send_pm1_fw] Waiting for ESP32 response...")
        start_time = time.time()
        last_activity = time.time()
        while time.time() - start_time < 90:  # 最多等待90秒（I2C 传输较慢）
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                if line:
                    print(f"  ESP32: {line}")
                    last_activity = time.time()  # 重置活动时间
                    
                    if "[iap] OK" in line or "upgrade complete" in line.lower():
                        print("[send_pm1_fw] ✓ Upgrade successful!")
                        return
                    if "chunk" in line.lower() or "iap" in line.lower():
                        # 显示进度信息，继续等待
                        continue
                    if "error" in line.lower() or "failed" in line.lower():
                        print(f"[send_pm1_fw] ✗ Upgrade failed: {line}")
                        return
            
            # 如果超过 30 秒没有任何输出，认为可能超时
            if time.time() - last_activity > 30:
                print("[send_pm1_fw] Warning: No activity for 30 seconds")
                break
            
            time.sleep(0.1)
        
        print("[send_pm1_fw] Timeout waiting for completion (check ESP32 logs)")


def main():
    parser = argparse.ArgumentParser(
        description='Send PM1 firmware via UART to ESP32 bridge',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # 从 .h 文件自动提取 appHexData[] 并计算 MD5
  python send_pm1_fw.py --port COM3 --file app_without_flash.h
  
  # 从 Intel HEX 文件
  python send_pm1_fw.py --port COM3 --file firmware.hex
  
  # 从二进制文件
  python send_pm1_fw.py --port COM3 --file firmware.bin
  
  # 手动指定 appHexData[] 的 MD5
  python send_pm1_fw.py --port COM3 --md5 15b6fd266c9a381e0f5378635f646237 --file app.h
  
  # 使用自定义波特率和延迟
  python send_pm1_fw.py --port /dev/ttyUSB0 --baudrate 115200 --file fw.h --delay 200
        """
    )
    
    parser.add_argument('--port', '-p', required=True,
                        help='Serial port (e.g., COM3, /dev/ttyUSB0)')
    parser.add_argument('--baudrate', '-b', type=int, default=115200,
                        help='Baud rate (default: 115200)')
    parser.add_argument('--file', '-f', type=Path, required=True,
                        help='Firmware file (.bin/.hex/.h with appHexData[])')
    parser.add_argument('--md5', '-m', type=str,
                        help='MD5 hash of appHexData[] (32 hex chars). Auto-computed if not provided')
    parser.add_argument('--delay', '-d', type=int, default=100,
                        help='Delay between MD5 and firmware in ms (default: 100)')
    
    args = parser.parse_args()
    
    try:
        # 加载固件数据 (自动检测 .h/.hex/.bin 格式)
        print(f"[send_pm1_fw] Loading firmware from {args.file}...")
        firmware_data, file_type = load_firmware(args.file)
        print(f"[send_pm1_fw] Loaded {len(firmware_data)} bytes ({file_type})")
        
        # 如果没有提供 MD5,自动计算 appHexData[] 的 MD5
        if args.md5:
            md5_hex = args.md5.strip().lower()
            print(f"[send_pm1_fw] Using provided MD5: {md5_hex}")
        else:
            print(f"[send_pm1_fw] Computing MD5 of firmware data...")
            md5_hex = compute_md5_bytes(firmware_data)
            print(f"[send_pm1_fw] Computed MD5: {md5_hex}")
        
        send_firmware(
            port=args.port,
            baudrate=args.baudrate,
            md5_hex=md5_hex,
            firmware_data=firmware_data,
            delay_ms=args.delay
        )
        
    except KeyboardInterrupt:
        print("\n[send_pm1_fw] Interrupted by user")
        sys.exit(1)
    except Exception as e:
        print(f"[send_pm1_fw] Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
