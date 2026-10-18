# Zephyr 智能节点 + Linux 上位机 UART 通信系统

基于 STM32H563ZI（Cortex-M33）与 Zephyr RTOS 的工业数据采集节点，
通过串口与 PC 端上位机通信，实现「采集 → 时间戳 → 校验 → 落盘」的完整链路。

## 功能特性

- **四线程架构**：UART 接收、LED 控制、传感器采集、按键输入互相解耦，
  通过消息队列通信，避免共享可变状态
- **中断驱动串口接收**：`uart_irq_callback` + 消息队列，避免轮询导致的 overrun 丢字节
- **自定义文本协议**：帧内校验和、帧序号、`OK:` / `ERR:` 指令回执
- **RTC 时间戳**：片内 RTC + LSE，每帧数据自带精确时间
- **参数持久化**：Zephyr NVS（Log-structured）+ 魔数 + 格式版本号校验，上电自动加载
- **看门狗保护**：IWDG + 多线程心跳监督，任一线程失联则停止喂狗，硬件自动复位
- **复位原因识别**：区分上电 / 复位按钮 / 看门狗复位
- **上位机**：pyserial 双线程、校验和验证、丢帧检测、ACK 超时告警、CSV 落盘

## 系统架构

```
                        ┌──────────── 中断 ───────────┐
   PC (USB-CDC) ───────►│ USART3 RX                   │
                        └─────────────┬───────────────┘
                                      ▼
                                 byte_msgq
                                      │
                               uart_thread ──► my_msgq ──┐
                                      │                  │
                                      └──► sensor_msgq ──┤
                                                         │
   PC13 按键 ──► button_thread ──► sensor_msgq ──────────┤
                                                         ▼
                                                ┌────────────────┐
                                                │  led_thread    │──► LED0
                                                │  sensor_thread │──► $S 帧
                                                └────────────────┘
                                                         │
                                        心跳 hb_led / hb_sensor / hb_uart
                                                         ▼
                                    main：心跳监督者 ──► wdt_feed ──► IWDG
```

## 通信协议

### 上行：数据帧

```
$S,<seq>,<YYYY-MM-DD HH:MM:SS>,<温度>,<湿度>*<校验和>
```

示例：

```
$S,17,2026-10-15 12:00:36,21.7,41.7*5C
```

| 字段 | 说明 |
|------|------|
| `seq` | 帧序号，从 0 递增，上位机据此检测丢帧 |
| 时间戳 | 来自片上 RTC |
| 温度 / 湿度 | 定点数，单位 0.1（`21.7` 内部是 217） |
| 校验和 | `$` 之后到 `*` 之前所有字符的字节和，取低 8 位，两位十六进制 |

### 下行：控制指令

| 指令 | 作用 |
|------|------|
| `on` / `off` | LED 常亮 / 常灭 |
| `blink <ms>` | LED 闪烁，周期 1~10000 ms |
| `status` | 查询 LED 状态与最新传感器数据 |
| `start` / `stop` / `toggle` | 开始 / 停止 / 切换数据采集 |
| `save` / `read` | 保存 / 读取 Flash 中的配置 |
| `crash` | 人为制造线程卡死，用于验证看门狗（演示用） |

所有指令均有回执：成功打印 `OK:<内容>`，失败打印 `ERR:<原因>`。

## 硬件环境

| 项目 | 说明 |
|------|------|
| 开发板 | ST NUCLEO-H563ZI（STM32H563ZI，Cortex-M33） |
| LED | PB0（板载 LD1） |
| 按键 | PC13（板载用户按键，高电平有效） |
| 串口 | USART3（PD8 / PD9），经 ST-Link VCP 虚拟串口输出 |
| 存储 | 片内 Flash `storage_partition`（偏移 0x1F0000，64 KB） |

## 软件环境

- Zephyr RTOS 4.4.99
- Zephyr SDK 1.0.1
- 上位机：Python 3 + pyserial

## 编译与烧录

```bash
west build -b nucleo_h563zi
west flash
```

## 上位机使用

```bash
pip install pyserial
python3 host.py
```

默认串口为 `/dev/ttyACM0`（Linux）。首次使用需要串口权限：

```bash
sudo usermod -aG dialout $USER   # 重新登录后生效
```

运行后在 `>` 提示符输入指令，校验通过的数据会追加写入 `sensor_log.csv`。

## 目录结构

```
.
├── src/main.c              # 固件逻辑
├── prj.conf                # Kconfig 配置
├── nucleo_h563zi.overlay   # 设备树覆盖
├── CMakeLists.txt
├── host.py                 # Python 上位机
└── README.md
```

## 已知限制

- 传感器数据为模拟值（`200 + i % 100` / `400 + i % 100`），尚未接入真实温湿度传感器
- RTC 无电池，掉电后回到默认时间；目前靠「年份合理性检查」判断是否需要重新校准，
  尚无上位机对时指令
- CAN / Modbus 从站尚未实现

## 后续计划

- [ ] 接入真实温湿度传感器（I2C）
- [ ] 上位机对时指令
- [ ] CAN 总线通信（回环 → 双节点）
- [ ] 系统框图与调试记录

## 开发记录

| 日期 | 内容 |
|------|------|
| 2026-09-08 | 项目启动 |
| 2026-10-11 | 双向通信闭环（校验和 + ACK + 超时） |
| 2026-10-16 | Linux 上位机 CSV 落盘、按键启停 |
| 2026-10-18 | NVS 参数持久化、IWDG 看门狗与复位原因识别 |
