#include "stm32f10x.h"                  // Device header
#include "Delay.h"
#include "OLED.h"
#include "MyCAN.h"

#define NODE_TX_ID		0x101			//本节点发送用的 CAN ID（11 位标准帧）
#define TX_PERIOD_MS	1000			//发送周期，单位 ms

uint8_t  TxData[8];						//发送缓冲区（经典 CAN 一帧最多 8 字节）
uint32_t RxID;							//收到的帧 ID
uint8_t  RxLength;						//收到的帧数据长度
uint8_t  RxData[8];						//收到的帧数据

int main(void)
{
	OLED_Init();
	MyCAN_Init();

	/* 界面骨架：四行分别显示 发送ID+计数 / 接收ID / 长度 / 数据 */
	OLED_ShowString(1, 1,  "TxID:");
	OLED_ShowHexNum(1, 6,  NODE_TX_ID, 3);
	OLED_ShowString(1, 10, "N:");
	OLED_ShowString(2, 1,  "RxID:");
	OLED_ShowString(2, 6,  "---");		//还没收到帧时占位
	OLED_ShowString(3, 1,  "Leng:");
	OLED_ShowString(4, 1,  "Data:");

	uint16_t TxCount = 0;				//发送计数，同时当载荷
	uint16_t MsCount = 0;				//软定时计数器

	while (1)
	{
		/* ---------- 1. 软定时：每 1s 主动发一帧 ---------- */
		Delay_ms(1);					//1ms 一滴答
		if (++ MsCount >= TX_PERIOD_MS)
		{
			MsCount = 0;
			TxCount ++;

			TxData[0] = TxCount & 0xFF;	//低字节：计数
			TxData[1] = TxCount >> 8;	//高字节：计数
			TxData[2] = 0xAA;			//固定图案，肉眼核对用
			TxData[3] = 0x55;

			MyCAN_Transmit(NODE_TX_ID, 4, TxData);

			OLED_ShowHexNum(1, 12, TxCount, 3);
		}

		/* ---------- 2. 查询 FIFO0 有没有收到的帧 ---------- */
		if (MyCAN_ReceiveFlag())
		{
			MyCAN_Receive(&RxID, &RxLength, RxData);

			OLED_ShowHexNum(2, 6,  RxID, 3);
			OLED_ShowHexNum(3, 6,  RxLength, 1);
			OLED_ShowHexNum(4, 6,  RxData[0], 2);
			OLED_ShowHexNum(4, 9,  RxData[1], 2);
			OLED_ShowHexNum(4, 12, RxData[2], 2);
			OLED_ShowHexNum(4, 15, RxData[3], 2);
		}
	}
}
