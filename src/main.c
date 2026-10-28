#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/drivers/rtc.h>
#include <errno.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/can.h>

#define SLEEP_TIME_MS   1000
#define LED0_NODE DT_ALIAS(led0)
#define CFG_ID_MAIN    1U          // Flash 里的抽屉编号
#define CFG_MAGIC      0x43464731U // 'C''F''G''1' 魔数
#define CFG_VERSION    1U          // 格式版本
#define CFG_DEF_BLINK  500U        // 出厂默认值
#define WDT_TIMEOUT_MS  5000U       // 看门狗超时 5 秒
#define HB_TIMEOUT_MS    3000U       // 心跳超时 3 秒

// 一块"带标签的配置"，存进 Flash 的完整内容
struct sys_config {
	uint32_t magic;      // 魔数：确认"这是我们这个配置"
	uint16_t version;    // 格式版本：确认"这是当前这版格式"
	uint16_t blink_ms;   // 真正的数据
};

// 前置声明：这两个函数定义在文件后面，先告诉编译器它们的签名
static int cfg_load(struct sys_config *out);
static int cfg_store(int blink_ms);

static struct nvs_fs cfg_fs;
static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;// 看门狗通道号
// 看门狗心跳时间戳：每轮循环都更新，主循环里定时检查
static volatile uint32_t hb_led;
static volatile uint32_t hb_sensor;
static volatile uint32_t hb_uart;

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);
static const struct device *const uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
enum cmd_type{
	CMD_LED_ON = 0,
	CMD_LED_OFF,
	CMD_LED_BLINK,
	CMD_STATUS,
	CMD_CONFIG_SAVE,
	CMD_CONFIG_READ,
	CMD_CRASH,
};

enum led_state{
	LED_OFF = 0,
	LED_ON,
	LED_BLINK,
};
struct cmd{
	enum cmd_type type;
	int value;
	
};
//消息队列
K_MSGQ_DEFINE(my_msgq, sizeof(struct cmd), 10, 4);
//---采集控制：命令队列
enum sensor_cmd{
	CMD_SAMPLE_START = 0,
	CMD_SAMPLE_STOP,
	CMD_SAMPLE_TOGGLE,
};

K_MSGQ_DEFINE(sensor_msgq,sizeof(int),8,4);

//传感器数据结构体(假设有温湿度传感器)
struct sensor_data{
	int temp;
	int hum;
};
//--can--//
static const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(can_rx_msgq, 8);// CAN 接收消息队列，最多 8 条消息
struct sensor_data g_sensor;
#define LED_THREAD_STACK_SIZE 1024
#define LED_THREAD_PRIORITY 7
//分段睡眠
static void led_pause(int ms){
	while(ms >0){
		int step = (ms > 100) ? 100 : ms;
		k_msleep(step);
		hb_led = k_uptime_get_32();   // 每轮循环报到一次
		ms -= step;	
	}
}
//LED 线程，这个线程自己负责控制LED的闪烁和初始化
void led_thread(void *arg1,void *arg2,void *arg3){
	enum led_state state = LED_BLINK;
	int blink_delay = CFG_DEF_BLINK;
	// 上电试着从 Flash 里读配置（带魔数/版本校验）
	struct sys_config cfg;
	int rc = cfg_load(&cfg);
	if(rc == 0){
		blink_delay = cfg.blink_ms;
		printk("Config loaded: blink=%d ms\n", blink_delay);
	}else{
		printk("No usable config (rc=%d), default=%d ms\n", rc, blink_delay);
	}

	if (!device_is_ready(led.port)) {
		printk("Error: %s device is not ready\n", led.port->name);
		return;
	}
	if (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) < 0) {
		printk("Error: failed to configure %s pin %d\n",
		       led.port->name, led.pin);
		return;
	}
	while(1){
		hb_led = k_uptime_get_32();   // 每轮循环报到一次
		struct cmd c;
		if(k_msgq_get(&my_msgq, &c, K_NO_WAIT) == 0){
			switch(c.type){
				case CMD_LED_ON:
					state = LED_ON;
					gpio_pin_set_dt(&led, 1);
					printk("OK:on\r\n");
					break;
				case CMD_LED_OFF:
					state = LED_OFF;
					gpio_pin_set_dt(&led, 0);
					printk("OK:off\r\n");
					break;
				case CMD_LED_BLINK:
					state = LED_BLINK;
					blink_delay = c.value;
					printk("OK:blink %d ms\r\n", blink_delay);
					break;
				case CMD_STATUS:
					printk("LED state: %s, blink delay: %d ms\n",
					       state == LED_ON ? "ON" :
					       state == LED_OFF ? "OFF" : "BLINK",
					       blink_delay);
					printk("Sensor data: temp=%d.%d, hum=%d.%d\n",
					       g_sensor.temp / 10, g_sensor.temp % 10,
					       g_sensor.hum / 10, g_sensor.hum % 10);
					printk("OK:status\r\n");
					break;
				case CMD_CONFIG_SAVE:
					rc = cfg_store(blink_delay);
					if(rc < 0){
						printk("ERR: save failed rc=%d\r\n", rc);
					}else{
						printk("OK: saved blink=%d ms\r\n", blink_delay);
					}
					break;
				case CMD_CONFIG_READ:
					rc = cfg_load(&cfg);
					if(rc == 0){
						blink_delay = cfg.blink_ms;
						printk("OK: read blink=%d ms\r\n", blink_delay);
					}else{
						blink_delay = CFG_DEF_BLINK;
						printk("OK: no usable config (rc=%d), default=%d ms\r\n",
						       rc, blink_delay);
					}
					break;
				case CMD_CRASH:
					printk("OK:led_thread 卡死，%d秒内应该被看门狗复位\n", WDT_TIMEOUT_MS/1000);
					while(1){
						k_msleep(1000);
					}
				default:
					printk("unknown command\n");
					break;
			}
		}
		//闪烁LED
		if(state == LED_BLINK){
			gpio_pin_toggle_dt(&led);
			led_pause(blink_delay);
		}else{
			led_pause(100);
		}
	}
}


K_THREAD_DEFINE(Led_tid, LED_THREAD_STACK_SIZE, led_thread, NULL, NULL, NULL, LED_THREAD_PRIORITY, 0, 0);

//接收字节队列：中断回调把收到的字节放进这里，解析线程从这里取
K_MSGQ_DEFINE(byte_msgq, sizeof(uint8_t), 64, 1);

//UART 接收中断回调：每收到一个字节就立刻读走，放进队列
static void uart_cb(const struct device *dev, void *user_data){
	uint8_t ch;
	uart_irq_update(uart);
	while(uart_irq_rx_ready(uart)){
		if(uart_fifo_read(uart, &ch, 1) != 1){
			break;
		}
		k_msgq_put(&byte_msgq, &ch, K_NO_WAIT);
	}
}

void uart_thread(void *arg1,void *arg2,void *arg3){
	char line [32];
	int pos = 0;
	if(!device_is_ready(uart)){
		printk("Error: %s device is not ready\n", uart->name);
		return;
	}
	uart_irq_callback_set(uart, uart_cb);
	uart_irq_rx_enable(uart);
	while(1){
		uint8_t ch;
		int got = k_msgq_get(&byte_msgq, &ch, K_MSEC(500));
		hb_uart = k_uptime_get_32();   // 每轮循环报到一次
		if(got != 0){
			continue;
		}
		if(ch == '\r' || ch == '\n'){
			if(pos > 0){
				line[pos] = '\0';
				struct cmd c;
					if(strcmp(line, "on") == 0){
						c.type = CMD_LED_ON;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}else if(strcmp(line, "off") == 0){
						c.type = CMD_LED_OFF;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}else if(strncmp(line, "blink",5) == 0){
						c.type = CMD_LED_BLINK;
						c.value = atoi(line + 5);
						if(c.value <= 0||c.value > 10000){
							printk("ERR: Invalid blink delay: %d ms. Must be between 1 and 10000 ms\n", c.value);
						}else{
							k_msgq_put(&my_msgq, &c, K_FOREVER);
						}
					}else if(strcmp(line, "status") == 0){
						c.type = CMD_STATUS;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}else if(strcmp(line, "start") == 0){
						int cmd = CMD_SAMPLE_START;
						k_msgq_put(&sensor_msgq, &cmd, K_NO_WAIT);
					}else if(strcmp(line, "stop") == 0){
						int cmd = CMD_SAMPLE_STOP;
						k_msgq_put(&sensor_msgq, &cmd, K_NO_WAIT);
					}else if(strcmp(line, "toggle") == 0){
						int cmd = CMD_SAMPLE_TOGGLE;
						k_msgq_put(&sensor_msgq, &cmd, K_NO_WAIT);
					}else if(strcmp(line, "save") == 0){
						c.type = CMD_CONFIG_SAVE;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}else if(strcmp(line, "read") == 0){
						c.type = CMD_CONFIG_READ;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}else if(strcmp(line, "crash") == 0){
						c.type = CMD_CRASH;
						k_msgq_put(&my_msgq, &c, K_FOREVER);
					}
					else{
						printk("ERR: unknown command: %s\n", line);
						pos = 0;
					}		
				pos = 0;
			}
		}else if(pos < sizeof(line) - 1){
			line[pos++] = ch;
		}
	}
}



K_THREAD_DEFINE(Uart_tid, 1024, uart_thread, NULL, NULL, NULL, 7, 0, 0);

static uint8_t checksum(const char *data){
	uint8_t sum = 0;
	while(*data){
		sum += *data++;
	}
	return sum;
}

void sensor_thread(void *arg1,void *arg2,void *arg3){
	const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
	int i = 0;
	int seq = 0;
	bool running = true;                  // 上电默认采集
	while(1){
		// 阻塞最多 2 秒等命令：有命令就处理，没命令就往下采样
		hb_sensor = k_uptime_get_32();   // 每轮循环报到一次
		int cmd;
		if(k_msgq_get(&sensor_msgq, &cmd, K_MSEC(2000)) == 0){
			if(cmd == CMD_SAMPLE_START){
				running = true;
			}else if(cmd == CMD_SAMPLE_STOP){
				running = false;
			}else if(cmd == CMD_SAMPLE_TOGGLE){
				running = !running;
			}
			printk("OK:sampling %s\r\n", running ? "on" : "off");
			continue;                     // 处理完命令，回到开头重新等
		}
		if(!running){
			continue;                     // 停止状态：跳过采样
		}
		g_sensor.temp = 200 + (i % 100);
		g_sensor.hum  = 400 + (i % 100);
		struct rtc_time time = {0};
		rtc_get_time(rtc, &time);
		char frame[160];
		snprintf(frame, sizeof(frame),
			 "$S,%d,%04d-%02d-%02d %02d:%02d:%02d,%d.%d,%d.%d",
			 seq,
			 time.tm_year + 1900, time.tm_mon + 1, time.tm_mday,
			 time.tm_hour, time.tm_min, time.tm_sec,
			 g_sensor.temp/10, g_sensor.temp%10,
			 g_sensor.hum/10, g_sensor.hum%10);
		uint8_t sum = checksum(frame + 1);
		printk("%s*%02X\r\n", frame, sum);
		i++;
		seq++;
	}
}

K_THREAD_DEFINE(Sensor_tid, 1024, sensor_thread, NULL, NULL, NULL, 7, 0, 0);

//按键，本地启停采集
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

void button_thread(void *arg1,void *arg2,void *arg3){
	if(!device_is_ready(button.port)){
		printk("Error: %s device is not ready\n", button.port->name);
		return;
	}
	if(gpio_pin_configure_dt(&button, GPIO_INPUT) < 0){
		printk("Error: failed to configure %s pin %d\n",
		       button.port->name, button.pin);
		return;
	}
	int last = gpio_pin_get_dt(&button);
	while(1){
		int now = gpio_pin_get_dt(&button);
		if(now != last){
			k_msleep(20);
			if(gpio_pin_get_dt(&button) == now){
				last = now;
				if(now == 1){
					int cmd = CMD_SAMPLE_TOGGLE;
					k_msgq_put(&sensor_msgq, &cmd, K_NO_WAIT);
				}
			}
		}
	k_msleep(10);
	}
}

K_THREAD_DEFINE(Button_tid, 1024, button_thread, NULL, NULL, NULL, 7, 0, 0);

static int cfg_mount(void){
	struct flash_pages_info info;
	cfg_fs.flash_device = PARTITION_DEVICE(storage_partition);
	if(!device_is_ready(cfg_fs.flash_device)){
		printk("ERR:flash device not ready\n");
		return -1;
	}
	cfg_fs.offset = PARTITION_OFFSET(storage_partition);
	if (flash_get_page_info_by_offs(cfg_fs.flash_device, cfg_fs.offset, &info) != 0) {
		printk("ERR:flash_get_page_info_by_offs failed\n");
		return -1;
	}
	cfg_fs.sector_size = info.size;
	cfg_fs.sector_count = PARTITION_SIZE(storage_partition) / info.size;
	int rc = nvs_mount(&cfg_fs);
	if(rc != 0){
		printk("ERR:nvs_mount failed: %d\n", rc);
		return -1;
	}
	return 0;
}

// 从 Flash 读配置：成功返回 0，失败返回负数
static int cfg_load(struct sys_config *out){
	int rc = nvs_read(&cfg_fs, CFG_ID_MAIN, out, sizeof(*out));
	if(rc != (int)sizeof(*out)){
		return -ENOENT;              // 长度不对：没存过，或者是老格式
	}
	if(out->magic != CFG_MAGIC){
		return -EINVAL;              // 魔数不对：不是我们的数据
	}
	if(out->version != CFG_VERSION){
		return -ENOTSUP;             // 版本不对：格式升级过
	}
	return 0;
}

// 把配置写进 Flash
static int cfg_store(int blink_ms){
	struct sys_config cfg = {
		.magic    = CFG_MAGIC,
		.version  = CFG_VERSION,
		.blink_ms = blink_ms,
	};
	return nvs_write(&cfg_fs, CFG_ID_MAIN, &cfg, sizeof(cfg));
}

static int wdt_init(void){
	if(!device_is_ready(wdt)){
		printk("ERR: WDT device not ready\n");
		return -1;
	}
	struct wdt_timeout_cfg wdt_config = {
		.window.min = 0,
		.window.max = WDT_TIMEOUT_MS,
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt, &wdt_config);
	if(wdt_channel < 0){
		printk("ERR: wdt_install_timeout failed: %d\n", wdt_channel);
		return -1;
	}
	int rc = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if(rc != 0){
		printk("ERR: wdt_setup failed: %d\n", rc);
		return -1;
	}
	printk("WDT armed: timeout=%d ms,heartbeat=%d ms\n", WDT_TIMEOUT_MS, HB_TIMEOUT_MS);
	return 0;
}
//复位方式
static void print_reset_cause(void){
	uint32_t cause = 0;
	if(hwinfo_get_reset_cause(&cause) != 0){
		printk("Reset cause: unavailable\n");
		return;
	}else if(cause & RESET_WATCHDOG){
		printk("Reset cause: pin reset<<<(上次运行卡死)_\n");
	}else if(cause & RESET_PIN){
		printk("Reset cause: power-on reset(按了rst)\n");
	}else if(cause & RESET_POR){
		printk("Reset cause: power-on reset(上电)\n");
	}else if(cause & RESET_SOFTWARE){
		printk("Reset cause: software reset\n");
	}else{
		printk("Reset cause: unknown (0x%08X)\n", cause);
	}
	hwinfo_clear_reset_cause();
}
//can_thread
void can_thread(void *arg1,void *arg2,void *arg3){
	if(!device_is_ready(can_dev)){
		printk("Error: %s device is not ready\n", can_dev->name);
		return;
	}
	//测试回环模式
	int rc = can_set_mode(can_dev, CAN_MODE_NORMAL);
	if(rc != 0){
		printk("ERR: can_set_mode failed: %d\n", rc);
		return;
	}
	//启动控制器
	rc = can_start(can_dev);
	if(rc != 0){
		printk("ERR: can_start failed: %d\n", rc);
		return;
	}

	//配置过滤器：只接收ID=0x123的报文
	struct can_filter filter = {
		.flags = 0,//标准帧11位id
		.id = 0,
		.mask = 0,//只匹配11位id
	};
	rc = can_add_rx_filter_msgq(can_dev, &can_rx_msgq, &filter);
	if(rc < 0){
		printk("ERR: can_add_rx_filter_msgq failed: %d\n", rc);
		return;
	}
	printk("CAN loopback ready,waiting for messages...\n");
	uint8_t counter = 0;
	while(1){
		//造一帧
		struct can_frame tx = {0};
		tx.id = 0x123;
		tx.dlc = 8;
		tx.data[0] = counter;
		tx.data[1] = 0xAA;
		tx.data[2] = 0x55;
		rc = can_send(can_dev, &tx, K_MSEC(100), NULL, NULL);
		if(rc != 0){
			printk("ERR: can_send failed: %d\n", rc);
		}else{
			printk("CAN sent: id=0x%03X, data[0]=%d\n", tx.id, tx.data[0]);
		}
		//从消息队列里取接收到的报文
		struct can_frame rx = {0};
		rc = k_msgq_get(&can_rx_msgq, &rx, K_MSEC(100));
		if(rc == 0){
			printk("RX id=0x%03X, data[0]=%d\n", rx.id, rx.data[0]);
		}else{
			printk("No CAN message received\n");
		}
		counter++;
		k_msleep(2000);
	}
}

K_THREAD_DEFINE(Can_tid, 1024, can_thread, NULL, NULL, NULL, 7, 0, 0);
int main(void){
	printk("Hello World! %s\n", CONFIG_BOARD);
	printk("版本: 1.0.4\n");
	print_reset_cause();
	if(cfg_mount() != 0){
		printk("ERR: failed to mount NVS\n");
		return -1;
	}
	const struct device *rtc = DEVICE_DT_GET(DT_ALIAS(rtc));
	if (!device_is_ready(rtc)) {
		printk("Error: RTC device is not ready\n");
		return -1;
	}

	// RTC 没有电池时掉电会归零到 2000-01-01。
	// 年份不合理 = 这个时间是假的，需要重新校准；合理 = 它在正常走，别动它。
	struct rtc_time t = {0};
	int rc = rtc_get_time(rtc, &t);
	if(rc != 0 || t.tm_year + 1900 < 2020){
		t.tm_year = 2026 - 1900;
		t.tm_mon  = 10 - 1;
		t.tm_mday = 15;
		t.tm_hour = 12;
		t.tm_min  = 0;
		t.tm_sec  = 0;
		t.tm_wday = 4;
		rtc_set_time(rtc, &t);
		printk("RTC invalid, calibrated to 2026-10-15 12:00:00\n");
	}else{
		printk("RTC keeps running: %04d-%02d-%02d %02d:%02d:%02d\n",
		       t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
		       t.tm_hour, t.tm_min, t.tm_sec);
	}
	if(wdt_init() != 0){
		printk("ERR: failed to init WDT\n");
		return -1;
	}
	bool warned = false;
	while(1){
		uint32_t now = k_uptime_get_32();
		uint32_t delta_led = now - hb_led;
		uint32_t delta_sensor = now - hb_sensor;
		uint32_t delta_uart = now - hb_uart;

		bool alive = (delta_led <= HB_TIMEOUT_MS) && (delta_sensor <= HB_TIMEOUT_MS) && (delta_uart <= HB_TIMEOUT_MS);
		if(alive){
			if(wdt_channel >= 0){
				wdt_feed(wdt, wdt_channel);
			}
			warned = false;

		}else if(!warned){
			printk("WARN: heartbeat timeout: led=%d ms, sensor=%d ms, uart=%d ms\n",
			       delta_led, delta_sensor, delta_uart);
			warned = true;
		}
	
		k_msleep(500);
	}
}
