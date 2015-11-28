#include <stdio.h>
#include <string.h>
#include "NUC123.h"
#include "fds.h"
#include "fdsutil.h"
#include "flash.h"
#include "spiutil.h"
#include "fifo.h"
#include "main.h"
#include "sram.h"

/*
PIN 43 = -write
PIN 42 = -scan media
PIN 41 = motor on
PIN 40 = -writeable media
PIN 39 = -media set
PIN 38 = -ready
PIN 34 = -stop motor
? = WRITEDATA
PIN 48 = data
PIN 47 = rate
*/

#define WRITEBUFSIZE	4096

volatile uint8_t *writeptr;
volatile int writepos, writepage;
volatile uint8_t writebuf[WRITEBUFSIZE];

volatile uint8_t disklistbuf[4096 + 3];
volatile uint8_t *disklistblock = disklistbuf;
volatile uint8_t *disklist = disklistbuf + 1;
int disklistpos = -1;

uint8_t tempbuffer[4096];

volatile int diskblock = 0;

//for sending data to the ram adaptor
volatile int rate = 0;
volatile int count = 0;

volatile uint8_t data, data2;
volatile int outbit;
volatile int bytes;
volatile int needpage;
volatile int needbyte;
volatile fifo_t writefifo;
volatile int writing;
volatile int decodepos;

__inline uint8_t raw_to_raw03_byte(uint8_t raw)
{
	if(raw < 0x50)
		return(3);
	else if(raw < 0x70)
		return(0);
	else if(raw < 0xA0)
		return(1);
	else if(raw < 0xD0)
		return(2);
	return(3);
}

__inline void decode(uint8_t *dst, uint8_t src, int *outptr, uint8_t *bit)
{
	int out = *outptr;

	switch(src | (*bit << 4)) {
		case 0x11:
			out++;
		case 0x00:
			out++;
			*bit = 0;
			break;
		case 0x12:
			out++;
		case 0x01:
		case 0x10:
			dst[out/8] |= (1 << (out & 7));
			out++;
			*bit = 1;
			break;
		default:
			out++;
			*bit = 0;
			break;
	}
	*outptr = out;
}

//for sending data out to ram adaptor
void TMR1_IRQHandler(void)
{
	TIMER_ClearIntFlag(TIMER1);

	//output current bit
	PA11 = (outbit ^ rate) & 1;

	//toggle rate
	rate ^= 1;
	
	//every other toggle we need to process the data
	if(rate) {
		count++;
		
		//if we have sent all eight bits of this byte, get next byte
		if(count == 8) {
			count = 0;

			//read next byte from the page
			data = data2;
			
			//signal we need another data byte
			needbyte++;
		}

		//get next bit to be output
		outbit = data & 1;
		
		//and shift the data byte over to the next next bit
		data >>= 1;
	}
}

//for writes coming out of the ram adaptor
void EINT0_IRQHandler(void)
{
	int ra;

	//clear interrupt flag
    GPIO_CLR_INT_FLAG(PB, BIT14);
	
	//if we are writing
	if(IS_WRITE()) {
		
		//get flux transition time
		ra = TIMER_GetCounter(TIMER0);
		
		//restart the counter
		TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
		TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;

		//put the data into the fifo buffer
		fifo_write_byte((fifo_t*)&writefifo,(uint8_t)ra);
	}
}

volatile int bufpos, sentbufpos;

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
		tempbuffer[bufpos++] = (uint8_t)ra;
		if(bufpos >= 4096) {
			bufpos = 0;
		}
    }
}

void hexdump(char *desc, void *addr, int len);

__inline void check_needbyte(void)
{
	if(needbyte) {
			
		//clear flag
		needbyte = 0;
		
		//read next byte to be output
		sram_read(bytes,(uint8_t*)&data2,1);
		
		//increment the byte counter
		bytes++;
	}
}

static void begin_transfer(void)
{
	int leadin = (DEFAULT_LEAD_IN / 8) - 1;
	int dirty = 0;

	printf("beginning transfer...\r\n");
	fifo_init((fifo_t*)&writefifo,(uint8_t*)writebuf,4096);

	//initialize variables
	outbit = 0;
	rate = 0;
	bytes = 0;
	count = 0;

	//lead-in byte counter
	leadin = (DEFAULT_LEAD_IN / 8) - 1;
	
	//finish off the 0.15 second delay
	TIMER_Delay(TIMER2, 130 * 1000);

	//enable/disable necessary irq's
    NVIC_DisableIRQ(USBD_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
	NVIC_DisableIRQ(TMR3_IRQn);
    NVIC_EnableIRQ(EINT0_IRQn);
    NVIC_EnableIRQ(TMR1_IRQn);
	
	//start timers
	TIMER_Start(TIMER0);
	TIMER_Start(TIMER1);

	//activate ready signal
	SET_READY();
	
	LED_RED(1);

	data2 = 0;

	//transfer lead-in
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		
		//if irq handler needs more data
		if(needbyte) {
			needbyte = 0;
			bytes++;
		}

		//check if enough leadin data has been sent
		if(bytes >= leadin) {
			break;
		}
	}
	
	//reset byte counter
	bytes = 512;

	//transfer disk data
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {

		//check irq handler requesting another byte
		check_needbyte();

		//check if we have started writing
		if(IS_WRITE()) {
			int len = 0;
			uint8_t bitval = 0;
			uint8_t decoded[8] = {0,0,0,0,0,0,0,0};
			uint8_t byte;

			LED_GREEN(0);

			//set dirty flag to write data back to flash
			dirty = 1;

			//get initial write position
			writepos = bytes;

			//initialize the timer to get the flux transitions
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			TIMER0->TCMPR = 0xFFFFFF;
			TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;

			//while the write line is asserted
			while(IS_WRITE()) {

				//check irq handler requesting another byte
				check_needbyte();

				//decode data in the fifo buffer in there is any
				if(fifo_read_byte((fifo_t*)&writefifo,&byte)) {

					//decode the data
					decode((uint8_t*)decoded,raw_to_raw03_byte(byte),&len,&bitval);
					
					//if we have a full byte, write it to sram
					if(len >= 8) {
						len -= 8;
						sram_write(writepos++,decoded,1);
						decoded[0] = decoded[1];
						decoded[1] = 0;
					}
				}
			}

			//stop the timer for gathering flux transitions
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;

			//decode data in the fifo buffer
			while(fifo_read_byte((fifo_t*)&writefifo,&byte)) {

				//check irq handler requesting another byte
				check_needbyte();

				decode((uint8_t*)decoded,raw_to_raw03_byte(byte),&len,&bitval);
				if(len >= 8) {
					len -= 8;
					sram_write(writepos++,decoded,1);
					decoded[0] = decoded[1];
					decoded[1] = 0;
				}
			}

			check_needbyte();

			if(len) {
				sram_write(writepos,decoded,1);
			}

			LED_GREEN(1);
		}

		//check if insane
		if(bytes >= 0xFF00) {
			printf("reached end of data block, something went wrong...\r\n");
			break;
		}
	}

    NVIC_DisableIRQ(EINT0_IRQn);
    NVIC_DisableIRQ(TMR1_IRQn);
	TIMER_Stop(TIMER0);
	TIMER_Stop(TIMER1);
    NVIC_EnableIRQ(USBD_IRQn);

	//clear the ready signal
	CLEAR_READY();
	
	LED_RED(1);
	LED_GREEN(0);

	//if data was written, write it back to the flash
	if(dirty) {
		int flashpage = (diskblock * 0x10000) / 512;
		int i;

		printf("sram data is dirty, writing to flash....\r\n");
		printf("starting flash page: %d\r\n",flashpage);

		//write 512 bytes at a time
		for(i=0;i<128;i++) {
			sram_read(i * 512,tempbuffer,512);
			flash_write_page(flashpage++,tempbuffer);
			flash_busy_wait();
		}
	}

	LED_RED(0);
	LED_GREEN(1);
	printf("transferred %d bytes\r\n",bytes);
}


//string to find to start sending the fake disklist
uint8_t diskliststr[17] = {0x80,0x03,0x07,0x10,'D','I','S','K','L','I','S','T',0x00,0x80,0x00,0x10,0x00};

int find_disklist()
{
	int pos = 0;
	int count = 0;
	uint8_t byte;

	flash_read_start(0);
	for(pos=0;pos<65500;) {

		//read a byte from the flash
		flash_read((uint8_t*)&byte,1);
		pos++;
		
		//first byte matches
		if(byte == diskliststr[0]) {
			count = 1;
			do {
				flash_read((uint8_t*)&byte,1);
				pos++;
			} while(byte == diskliststr[count++]);
			if(count == 18) {
				printf("found disklist block header at %d (count = %d)\n",pos - count,count);

				//skip over the crc
				flash_read((uint8_t*)&byte,1);
				flash_read((uint8_t*)&byte,1);
				pos += 2;

				//skip the gap
				do {
					flash_read((uint8_t*)&byte,1);
					pos++;
				} while(byte == 0 && pos < 65500);

				//make sure this is a blocktype of 4
				if(byte == 0x80) {
					flash_read((uint8_t*)&byte,1);
					pos++;
					if(byte == 4) {
						flash_read((uint8_t*)&byte,1);
						printf("hard coded disk count = %d\n",byte);
						flash_read_stop();
						return(pos);
					}
				}
			}
		}
	}
	flash_read_stop();
	return(-1);
}

void create_disklist(void)
{
	uint8_t *list = (uint8_t*)disklist + 32;
	flash_header_t header;
	int blocks = flash_get_total_blocks();
	int i,num = 0;
	uint32_t crc;

	memset((uint8_t*)disklist,0,4096 + 2);

	for(i=0;i<blocks;i++) {
		
		//read disk header information
		flash_read_disk_header(i,&header);
		
		//empty block
		if((uint8_t)header.name[0] == 0xFF) {
			continue;
		}

		//continuation of disk sides
		if(header.name[0] == 0x00) {
			continue;
		}

		list[0] = (uint8_t)i;
		memcpy(list + 1,header.name,26);
		list[31] = 0;
		printf("block %X: id = %02d, '%s'\r\n",i,header.id,header.name);
		list += 32;
		num++;
	}
	disklistblock[0] = 4;
	disklist[0] = num;

	//correct
	crc = calc_crc((uint8_t*)disklistblock,4096 + 1 + 2);
	disklist[4096] = (uint8_t)(crc >> 0);
	disklist[4097] = (uint8_t)(crc >> 8);
}

static void begin_transfer_loader(void)
{
	int leadin = (DEFAULT_LEAD_IN / 8) - 1;
	int dirty = 0;

	printf("beginning transfer...\r\n");
	fifo_init((fifo_t*)&writefifo,(uint8_t*)writebuf,4096);

	if(disklistpos == -1) {
		disklistpos = find_disklist();
		printf("find_disklist() = %d\n",disklistpos);
		create_disklist();
	}

	sram_write(disklistpos,(uint8_t*)disklist,4096 + 2);

	//initialize variables
	outbit = 0;
	rate = 0;
	bytes = 0;
	count = 0;

	//lead-in byte counter
	leadin = (DEFAULT_LEAD_IN / 8) - 1;
	
	//finish off the 0.15 second delay
	TIMER_Delay(TIMER2, 130 * 1000);

	//enable/disable necessary irq's
    NVIC_DisableIRQ(USBD_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
	NVIC_DisableIRQ(TMR3_IRQn);
    NVIC_EnableIRQ(EINT0_IRQn);
    NVIC_EnableIRQ(TMR1_IRQn);
	
	//start timers
	TIMER_Start(TIMER0);
	TIMER_Start(TIMER1);

	//activate ready signal
	SET_READY();
	
	LED_RED(1);

	data2 = 0;

	//transfer lead-in
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		
		//if irq handler needs more data
		if(needbyte) {
			needbyte = 0;
			bytes++;
		}

		//check if enough leadin data has been sent
		if(bytes >= leadin) {
			break;
		}
	}
	
	//reset byte counter
	bytes = 512;

	//transfer disk data
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {

		//check irq handler requesting another byte
		check_needbyte();

		//check if we have started writing
		if(IS_WRITE()) {
			LED_GREEN(0);

			//set dirty flag to write data back to flash
			dirty = 1;

			//initialize the timer to get the flux transitions
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			TIMER0->TCMPR = 0xFFFFFF;
			TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;

			//while the write line is asserted, just gather data, dont decode any until later
			while(IS_WRITE()) {

				//check irq handler requesting another byte
				check_needbyte();
			}

			//stop the timer for gathering flux transitions
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;

			LED_GREEN(1);
		}

		//check if insane
		if(bytes >= 0xFF00) {
			printf("reached end of data block, something went wrong...\r\n");
			break;
		}
	}

    NVIC_DisableIRQ(EINT0_IRQn);
    NVIC_DisableIRQ(TMR1_IRQn);
	TIMER_Stop(TIMER0);
	TIMER_Stop(TIMER1);
    NVIC_EnableIRQ(USBD_IRQn);

	//clear the ready signal
	CLEAR_READY();
	
	LED_RED(1);
	LED_GREEN(0);

	//if data was written, figure out what diskblock to boot from
	if(dirty) {
		uint8_t *ptr = tempbuffer;
		int in,out,len = 0;
		uint8_t byte,bitval = 0;

		while(fifo_read_byte((fifo_t*)&writefifo,&byte)) {
			decode(tempbuffer,raw_to_raw03_byte(byte),&len,&bitval);
		}

		memset((uint8_t*)writebuf,0,WRITEBUFSIZE);
		bin_to_raw03(tempbuffer,(uint8_t*)writebuf,len / 8,WRITEBUFSIZE);
		in = out = 0;
		memset(tempbuffer,0,1024);
		block_decode(tempbuffer,(uint8_t*)writebuf,&in,&out,4096,1024,2,2);
		
		hexdump("tempbuffer",tempbuffer,256);

		printf("loader exiting, new diskblock = %d\n",ptr[1]);
		fds_insert_disk(ptr[1]);
	}
	else {
		printf("transferred %d bytes\r\n",bytes);
	}

	LED_RED(0);
	LED_GREEN(1);
}

//writes disk from data stored in the sram
/*void fds_writedisk(void)
{

	data2 = 0;
	bytes = 0;
	outbit = 0;
	rate = 0;
	bytes = 0;
	count = 0;

    NVIC_DisableIRQ(USBD_IRQn);
	NVIC_DisableIRQ(TMR1_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
    NVIC_DisableIRQ(EINT0_IRQn);
	NVIC_EnableIRQ(TMR3_IRQn);
	TIMER_Stop(TIMER1);
	TIMER_Start(TIMER3);

	CLEAR_WRITE();
	CLEAR_STOPMOTOR();
	SET_SCANMEDIA();

	printf("checking position of drive head (if already ready)...\n");
	while(IS_READY()) {
		
	}

	printf("waiting on drive to be ready...\n");
	sram_read_start(0);

	while(IS_READY() == 0) {
		
	}

//	printf("drive ready, sending data...\n");

	SET_WRITE();

	needbyte = 0;
	bytes = 0;
	count = 0;
	NVIC_EnableIRQ(TMR3_IRQn);

	while(bytes < 65500 && IS_READY()) {
		if(needbyte) {
			needbyte = 0;
			sram_read_byte(&data2);
			bytes++;
		}
	}
	printf("finished sending data, sent %d bytes...\n",bytes);

	CLEAR_WRITE();
	CLEAR_SCANMEDIA();
	SET_STOPMOTOR();
	
	sram_read_end();

	TIMER_Stop(TIMER3);
	NVIC_DisableIRQ(TMR3_IRQn);
    NVIC_EnableIRQ(USBD_IRQn);
}*/

void fds_init(void)
{
	int usbattached = USBD_IS_ATTACHED();
	
//	usbattached = 0;
	if(usbattached) {
		fds_setup_diskread();
		CLEAR_WRITE();
	}
	else {
		fds_setup_transfer();
		CLEAR_WRITABLE();
		CLEAR_READY();
		CLEAR_MEDIASET();
		CLEAR_MOTORON();
		fds_insert_disk(0);
	}
}

enum {
	MODE_TRANSFER = 0,
	MODE_DISKREAD
};

int mode = MODE_TRANSFER;

//setup for talking to the ram adaptor
void fds_setup_transfer(void)
{
    /* Unlock protected registers */
    SYS_UnlockReg();

	//setup gpio pins for the fds
    GPIO_SetMode(PD, BIT5, GPIO_PMD_INPUT);
    GPIO_SetMode(PD, BIT4, GPIO_PMD_INPUT);
    GPIO_SetMode(PD, BIT3, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PD, BIT2, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PD, BIT1, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PD, BIT0, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PA, BIT12, GPIO_PMD_INPUT);
    GPIO_SetMode(PA, BIT11, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PB, BIT14, GPIO_PMD_INPUT);

	GPIO_DisableInt(PA, 11);
    GPIO_EnableEINT0(PB, 14, GPIO_INT_RISING);

	SYS_LockReg();

	mode = MODE_TRANSFER;
	printf("entering ram adaptor transfer mode\n");
}

//setup for reading/writing disks with the drive
void fds_setup_diskread(void)
{
    /* Unlock protected registers */
    SYS_UnlockReg();

	//setup gpio pins for the fds
	GPIO_SetMode(PD, BIT5, GPIO_PMD_OUTPUT);	//-write
	GPIO_SetMode(PD, BIT4, GPIO_PMD_OUTPUT);	//-scanmedia
	GPIO_SetMode(PD, BIT3, GPIO_PMD_INPUT);		//motoron
	GPIO_SetMode(PD, BIT2, GPIO_PMD_INPUT);		//-writable
	GPIO_SetMode(PD, BIT1, GPIO_PMD_INPUT);		//-mediaset
	GPIO_SetMode(PD, BIT0, GPIO_PMD_INPUT);		//-ready
	GPIO_SetMode(PA, BIT12, GPIO_PMD_OUTPUT);	//-stopmotor
	GPIO_SetMode(PA, BIT11, GPIO_PMD_INPUT);	//read data
	GPIO_SetMode(PB, BIT14, GPIO_PMD_OUTPUT);	//write data

	GPIO_DisableEINT0(PB, 14);
	GPIO_EnableInt(PA, 11, GPIO_INT_RISING);

	SYS_LockReg();

	mode = MODE_DISKREAD;
	printf("entering disk read mode\n");
}

int find_first_disk_side(int block)
{
	flash_header_t header;

	//ensure this isnt the first block, if it is this must be the first side of a disk
	if(block == 0) {
		return(block);
	}

	//keep going in reverse to find the first disk side
	while(block > 0) {

		flash_read_disk_header(block,&header);
		
		//part of a multiside game
		if(header.name[0] != 0) {
			break;
		}

		block--;
	}

	return(block);
}

int mediaset = 0;
int ready = 0;

void fds_tick(void)
{
	if(mode == MODE_DISKREAD) {
		if(IS_MEDIASET() && mediaset == 0) {
			mediaset = 1;
			printf("disk inserted\n");
			if(IS_WRITABLE()) {
				printf("...and it is writable\n");
			}
		}
		if(IS_MEDIASET() == 0) {
			if(mediaset) {
				printf("disk ejected\n");
			}
			mediaset = 0;
		}
		if(IS_READY() && ready == 0) {
			ready = 1;
			printf("ready activated\n");
		}
		if(IS_READY() == 0) {
			if(ready) {
				printf("ready deactivated\n");
				CLEAR_SCANMEDIA();
				SET_STOPMOTOR();
			}
			ready = 0;
		}
		return;
	}
	
	//if the button has been pressed to flip disk sides
	if(PA10 != 0) {
		flash_header_t header;
		
		flash_read_disk_header(diskblock + 1,&header);
		
		//filename is 0's...must be more sides to this disk
		if(header.name[0] == 0) {
			diskblock++;
		}
		
		//try to find the first disk side
		else {
			diskblock = find_first_disk_side(diskblock);
		}

		printf("new disk side slot = %d\n",diskblock);
		CLEAR_MEDIASET();
		TIMER_Delay(TIMER2,1000 * 1000);
		fds_insert_disk(diskblock);
		
		//wait for button to be released before we insert disk
		while(PA10 != 0);

		SET_MEDIASET();			
	}
	
	//check if ram adaptor wants to stop the motor
	if(IS_STOPMOTOR()) {
		CLEAR_MOTORON();
	}

	//if ram adaptor wants to scan the media and run the motor
	if(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {

		SET_MOTORON();

		TIMER_Delay(TIMER2, 20 * 1000);

		if(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
			if(diskblock == 0) {
				begin_transfer_loader();
			}
			else {
				begin_transfer();
			}
		}
	}
}

void fds_insert_disk(int block)
{
	int i;

	diskblock = block;
	fds_setup_transfer();
	printf("copying image to sram...\r\n");
	for(i=0;i<64;i++) {
		flash_read_data((diskblock * 0x10000) + (i * 1024),tempbuffer,1024);
		sram_write(i * 1024,tempbuffer,1024);
	}
	printf("inserting disk at block %d\r\n",block);
	SET_MEDIASET();
	SET_WRITABLE();
}

void fds_remove_disk(void)
{
	CLEAR_MEDIASET();
	CLEAR_READY();
	printf("removing disk\r\n");
}

int needfinish;

void fds_start_diskread(void)
{
	//clear decodebuf
	memset(tempbuffer,0,4096);
	bufpos = 0;
	sentbufpos = 0;
	needfinish = 0;

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

	if(IS_READY() == 0) {
		printf("waiting drive to be ready\n");
		while(IS_READY() == 0);
	}
	
	while(get_buf_size() < len) {
//		printf("waiting for data\n");
	}

	t = sentbufpos + len;

	//if this read will loop around to the beginning of the buffer, handle it
	if(t >= 4096) {
		v = 4096 - sentbufpos;
		w = len - v;
		memcpy(bufbuf,tempbuffer + sentbufpos,v);
		memcpy(bufbuf + v,tempbuffer,w);
		sentbufpos = w;
	}
	
	//this read will be one unbroken chunk of the buffer
	else {
		memcpy(bufbuf,tempbuffer + sentbufpos,len);
		sentbufpos += len;
	}
}

#define OUTBUFSIZE	(255 * 1)

volatile uint8_t outbuf[2][OUTBUFSIZE];
volatile uint8_t *outptr;
volatile int outpage;
volatile int outpos;
volatile int needchunk;

//for sending data out to disk drive
void TMR3_IRQHandler(void)
{
	TIMER_ClearIntFlag(TIMER3);

	//if drive is not ready to be written to, do nothing
	if(IS_READY() == 0) {
		CLEAR_WRITE();
		return;
	}
	else {
		SET_WRITE();
	}

	//output current bit
	PB14 = data & 1;

	//shift the data byte over to the next next bit
	data >>= 1;

	//increment bit counter
	count++;
		
	//if we have sent all eight bits of this byte, get next byte
	if(count == 8) {
		count = 0;

		//read next byte from the page
		data = outptr[outpos++];
			
		//signal we need another chunk, and switch chunks
		if(outpos == OUTBUFSIZE) {
			needchunk = 0x10 | outpage;
			outpage ^= 1;
			outptr = outbuf[outpage];
			outpos = 0;
		}
	}
}

volatile uint8_t inputbuf[OUTBUFSIZE], *inputptr;
volatile int inputlen = 0;

void fds_start_diskwrite(void)
{
	//clear buffers
	memset(outbuf[0],0xAA,OUTBUFSIZE);
	memset(outbuf[1],0xAA,OUTBUFSIZE);
	data = 0xAA;
	outptr = outbuf[0];
	outpage = 0;
	outpos = 0;
	needchunk = 0;
	inputptr = inputbuf;
	inputlen = 0;

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

int fds_diskwrite_fillbuf(uint8_t *buf, int len)
{
	//copy this portion to the input buffer
	USBD_MemCopy(inputptr,buf,len);
	inputptr += len;
	inputlen += len;

	//if there is more data, wait for it to come before we do anything
	if(inputlen < OUTBUFSIZE) {
		return(1);
	}

	inputptr = inputbuf;
	inputlen = 0;

	//return code for buffer was filled completely
	return(0);
}

int fds_diskwrite(void)
{
	if(needchunk) {
		printf("write buffer underrun, outpos = %d, needchunk = %X\n",outpos,needchunk);
	}
	while(needchunk == 0);	
	memcpy((uint8_t*)outbuf[needchunk & 1],(uint8_t*)inputbuf,OUTBUFSIZE);
	needchunk = 0;
	return(0);
}
