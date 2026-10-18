import serial
import threading
import queue

ser = serial.Serial('/dev/ttyACM0', 115200, timeout=0.1)

def checksum(data: str) -> int:
    return sum(data.encode('utf-8')) & 0xFF

# "信箱"：读线程把回执塞进来，主线程从这里取
ack_queue = queue.Queue()

def reader_thread():
    """读线程：一直读串口。$S 帧校验打印；OK/ERR 塞进信箱。"""
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if not line:
            continue
        if line.startswith('$') and '*' in line:
            body, _, cs_hex = line.rpartition('*')
            try:
                recv = int(cs_hex, 16)
            except ValueError:
                print("[校验失败] 校验和不是十六进制:", line)
                continue
            calc = checksum(body[1:])
            if calc == recv:
                parts = body[3:].split(',')
                if len(parts) == 2:
                    print(f"[校验通过] 温度 {parts[0]} C   湿度 {parts[1]} %")
                else:
                    print("[校验通过] 但数据段异常:", line)
            else:
                print(f"[校验失败] 收到 {cs_hex} 算得 {calc:02X}: {line}")
        elif line.startswith('OK:'):
            ack_queue.put(('OK', line[3:]))
        elif line.startswith('ERR:'):
            ack_queue.put(('ERR', line[4:]))
        else:
            print("[其他] " + line)

# 起读线程：daemon=True 表示主线程退出时它跟着结束
threading.Thread(target=reader_thread, daemon=True).start()

print("已连接。输入 on / off / blink 300 / status 后回车，Ctrl+C 退出")
try:
    while True:
        cmd = input("> ").strip()
        if not cmd:
            continue
        ser.write((cmd + '\r\n').encode('utf-8'))
        ser.flush()
        print(f"已发送: {cmd}")
        try:
            status, msg = ack_queue.get(timeout=2)   # 最多等 2 秒
            if status == 'OK':
                print(f"[执行成功] {msg}")
            else:
                print(f"[执行失败] {msg}")
        except queue.Empty:
            print("[超时] 板子 2 秒内没回应")
except KeyboardInterrupt:
    print("\n退出程序")
finally:
    ser.close()
