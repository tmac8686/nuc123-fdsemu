#include <stdio.h>
#include <string.h>
#include "NUC123.h"
#include "fds.h"
#include "fdsutil.h"
#include "flash.h"
#include "spiutil.h"
#include "fifo.h"
#include "sram.h"

static const uint8_t expand[]={ 0xaa, 0xa9, 0xa6, 0xa5, 0x9a, 0x99, 0x96, 0x95, 0x6a, 0x69, 0x66, 0x65, 0x5a, 0x59, 0x56, 0x55 };

static volatile uint32_t dataout,dataout2;
static volatile int count;
static volatile int needbyte;

#define DISKBUFFERSIZE	4096

static volatile uint8_t diskbuffer[DISKBUFFERSIZE];
static volatile int bufpos, sentbufpos;

//for sending data out to disk drive
void TMR3_IRQHandler(void)
{
	TIMER_ClearIntFlag(TIMER3);

	//output current bit
	PB14 = dataout & 1;

	//shift the data byte over to the next next bit
	dataout >>= 1;

	//increment bit counter
	count++;
		
	//if we have sent all eight bits of this byte, get next byte
	if(count == 16) {
		count = 0;

		//read next byte from the page
		dataout = dataout2;
		
		//signal we need another mfm byte
		needbyte++;
	}
}

//for data coming out of the disk drive
void GPAB_IRQHandler(void)
{
    if(GPIO_GET_INT_FLAG(PA, BIT11)) {
		int ra;

//		printf("GPAB_IRQHandler");
		GPIO_CLR_INT_FLAG(PA, BIT11);
		ra = TIMER_GetCounter(TIMER0);
		TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
		TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;
		diskbuffer[bufpos++] = (uint8_t)ra;
		if(bufpos >= DISKBUFFERSIZE) {
			bufpos = 0;
		}
    }
}

//setup for writing a disk
void fds_start_diskwrite(void)
{
	dataout2 = 0xAAAA;
	needbyte = 0;
	count = 0;

	CLEAR_WRITE();
	CLEAR_STOPMOTOR();
	SET_SCANMEDIA();

	TIMER_Stop(TIMER0);
	TIMER_Stop(TIMER1);
	TIMER_Start(TIMER3);
	NVIC_DisableIRQ(TMR1_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
    NVIC_DisableIRQ(EINT0_IRQn);
	NVIC_EnableIRQ(TMR3_IRQn);
}

void fds_stop_diskwrite(void)
{
	TIMER_Stop(TIMER3);
	NVIC_DisableIRQ(TMR3_IRQn);
	CLEAR_WRITE();
	CLEAR_SCANMEDIA();
	SET_STOPMOTOR();
}

int fds_diskwrite(void)
{
	static int bytes;
	uint8_t byte;
	
	sram_read_start(0);

	bytes = 0;
	printf("waiting on drive to be ready\n");
	while(IS_READY() == 0);
	printf("writing...\n");
	SET_WRITE();
	
	while(IS_READY()) {
		if(needbyte) {
			needbyte = 0;
			if(bytes < 0x10000) {
				sram_read_byte(&byte);
				bytes++;
			}
			else {
				byte = 0;
			}
			dataout2 = expand[byte & 0x0F];
			dataout2 |= expand[(byte & 0xF0) >> 4] << 8;
		}
	}

	sram_read_end();

	return(0);
}

int needfinish;
//static int wasready;

void fds_start_diskread(void)
{
	//clear decodebuf
	memset((uint8_t*)diskbuffer,0,DISKBUFFERSIZE);
	bufpos = 0;
	sentbufpos = 0;
	needfinish = 0;
//	wasready = 0;

	CLEAR_WRITE();
	CLEAR_STOPMOTOR();
	SET_SCANMEDIA();

    NVIC_DisableIRQ(EINT0_IRQn);
    NVIC_DisableIRQ(TMR1_IRQn);

	TIMER_Start(TIMER0);
    NVIC_EnableIRQ(GPAB_IRQn);
}

void fds_stop_diskread(void)
{
	TIMER_Stop(TIMER0);
    NVIC_DisableIRQ(GPAB_IRQn);

	CLEAR_WRITE();
	CLEAR_SCANMEDIA();
	SET_STOPMOTOR();
}

static int get_buf_size()
{
	int ret = 0;

	if(bufpos >= sentbufpos) {
		ret = bufpos - sentbufpos;
	}
	else {
		ret = 4096 - sentbufpos;
		ret += bufpos;
	}
	return(ret);
}

void fds_diskread_getdata(uint8_t *bufbuf, int len)
{
	int t,v,w;
	int timeout = 10000;

//	if(wasready == 0) {
		if(IS_READY() == 0) {
			printf("waiting drive to be ready\n");
			while(IS_READY() == 0);
			if(timeout-- == 0) {
				printf("timed out\n");
			}
		}
/*	}
	else {
		memset(bufbuf,0,len);
	}*/
	
	while(get_buf_size() < len) {
//		printf("waiting for data\n");
	}

	t = sentbufpos + len;

	//if this read will loop around to the beginning of the buffer, handle it
	if(t >= 4096) {
		v = 4096 - sentbufpos;
		w = len - v;
		memcpy(bufbuf,(uint8_t*)diskbuffer + sentbufpos,v);
		memcpy(bufbuf + v,(uint8_t*)diskbuffer,w);
		sentbufpos = w;
	}
	
	//this read will be one unbroken chunk of the buffer
	else {
		memcpy(bufbuf,(uint8_t*)diskbuffer + sentbufpos,len);
		sentbufpos += len;
	}
}
