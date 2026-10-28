"""
Zephyr 智能节点 —— PC 上位机

功能：
  1. 接收上行数据帧 $S,<seq>,<时间戳>,<温度>,<湿度>*<校验和>
     - 逐帧校验和验证
     - 帧序号丢帧检测
     - 校验通过的数据追加写入 sensor_log.csv
  2. 下发控制指令（on/off/blink/status/start/stop/toggle/save/read/crash）
     - 等待板子的 OK:/ERR: 回执，2 秒超时告警

用法：
  python host.py
  改成串口：改下面的 PORT 常量（Linux 上是 /dev/ttyACM0）
"""

import os
import queue
import threading

import serial

# ---------------------------- 配置 ----------------------------
PORT = "COM10"                  # Windows: COM10(板载 ST-Link VCP)
                                # Linux  : "/dev/ttyACM0"
BAUDRATE = 115200
LOG_FILE = "sensor_log.csv"     # 落盘文件名
ACK_TIMEOUT = 2.0               # 等回执的超时（秒）

ser = serial.Serial(PORT, BAUDRATE, timeout=0.1)

# 回执"信箱"：读线程把 OK/ERR 塞进来，主线程从这里取
ack_queue = queue.Queue()

# 丢帧统计
last_seq = None
rx_count = 0
drop_count = 0


def checksum(data: str) -> int:
    """与固件 checksum() 完全一致：字节和，取低 8 位。"""
    return sum(data.encode("utf-8")) & 0xFF


# ---------------------------- CSV 落盘 ----------------------------
def init_log():
    """日志文件不存在就建一个并写表头，已存在则继续追加。"""
    if not os.path.exists(LOG_FILE):
        with open(LOG_FILE, "w", encoding="utf-8") as f:
            f.write("timestamp,temp,hum\n")
        print(f"[日志] 新建 {LOG_FILE}")


def save_record(timestamp, temp, hum):
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        f.write(f"{timestamp},{temp},{hum}\n")


# ---------------------------- 帧解析 ----------------------------
def check_seq(seq: int):
    """用帧序号判断有没有丢帧：正常应该每次 +1。"""
    global last_seq, drop_count
    if last_seq is not None:
        if seq < last_seq:
            print(f"[重启] 序号从 {last_seq} 回到 {seq}，板子重启过")
        else:
            gap = seq - last_seq - 1
            if gap > 0:
                drop_count += gap
                print(f"[丢帧] {last_seq} -> {seq}，丢了 {gap} 帧（累计 {drop_count}）")
    last_seq = seq


def handle_frame(line: str):
    """处理一个 $S 数据帧。"""
    global rx_count

    body, _, cs_hex = line.rpartition("*")      # body 形如 "$S,17,...,41.7"
    try:
        recv = int(cs_hex, 16)
    except ValueError:
        print("[丢弃] 校验和不是十六进制:", line)
        return

    calc = checksum(body[1:])                   # 跳过 '$'
    if calc != recv:
        print(f"[校验失败] 收到 {cs_hex}，算得 {calc:02X}: {line}")
        return

    parts = body[1:].split(",")                 # ['S', seq, 时间戳, 温度, 湿度]
    if len(parts) != 5:
        print("[丢弃] 字段数不对:", line)
        return

    _, seq_str, timestamp, temp, hum = parts
    try:
        seq = int(seq_str)
    except ValueError:
        print("[丢弃] 序号不是整数:", line)
        return

    check_seq(seq)
    rx_count += 1
    print(f"[#{seq:5d}] {timestamp}   温 {temp} °C   湿 {hum} %")
    save_record(timestamp, temp, hum)


def handle_line(line: str):
    if line.startswith("$") and "*" in line:
        handle_frame(line)
    elif line.startswith("OK:"):
        ack_queue.put(("OK", line[3:]))
    elif line.startswith("ERR:"):
        ack_queue.put(("ERR", line[4:]))
    else:
        print("[其他]", line)


# ---------------------------- 读线程 ----------------------------
def reader_thread():
    """一直读串口。

    串口是字节流不是消息流：一次 read 可能只拿到半行，
    所以自己攒缓冲区，按 '\\n' 切出完整的行再解析。
    """
    buf = ""
    while True:
        chunk = ser.read(ser.in_waiting or 1)
        if not chunk:
            continue
        buf += chunk.decode("utf-8", errors="ignore")
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.strip()
            if line:
                handle_line(line)


# ---------------------------- 主流程 ----------------------------
init_log()
threading.Thread(target=reader_thread, daemon=True).start()

print(f"已连接 {PORT}@{BAUDRATE}")
print("指令：on / off / blink 300 / status / start / stop / toggle / save / read / crash")
print("（crash 是故意让板子线程卡死，用来验证看门狗复位，别在正式演示时乱按）\n")

try:
    while True:
        cmd = input("> ").strip()
        if not cmd:
            continue

        ser.write((cmd + "\r\n").encode("utf-8"))
        ser.flush()
        print(f"已发送: {cmd}")

        try:
            status, msg = ack_queue.get(timeout=ACK_TIMEOUT)
            if status == "OK":
                print(f"[执行成功] {msg}")
            else:
                print(f"[执行失败] {msg}")
        except queue.Empty:
            print(f"[超时] 板子 {ACK_TIMEOUT} 秒内没回应")

except KeyboardInterrupt:
    print(f"\n退出。共收帧 {rx_count} 个，丢帧 {drop_count} 个，日志见 {LOG_FILE}")
finally:
    ser.close()
