#include <stdio.h>
#include <string.h>
#include "NUC123.h"
#include "fds.h"
#include "fdsutil.h"
#include "flash.h"

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


#define DECODEBUFSIZE	(1024 * 12)

uint8_t sectorbuf[4096];
uint8_t decodebuf[DECODEBUFSIZE];

#define PAGESIZE	512

uint8_t pagebuf[2][PAGESIZE];

struct write_s {
	int diskpos;			//position on disk where write was started
	int decstart;			//position in decodebuf the data begins
	int decend;				//position in decodebuf the data ends
} writes[8];

int write_num;				//number of writes

volatile int rate = 0;
volatile int diskblock = 0;
volatile int count = 0;

volatile uint8_t data, data2;
volatile int outbit = 0;
volatile int needbyte;
volatile int bytes;

uint8_t writelen;
int havewrite;

void TMR1_IRQHandler(void)
{
	TIMER_ClearIntFlag(TIMER1);

	rate ^= 1;
	if(rate) {
		count++;
		if(count == 8) {
			count = 0;
			data = data2;
			needbyte++;
		}
		outbit = data & 1;
		data >>= 1;
	}
	PA11 = (outbit ^ rate) & 1;
}

//for writes coming out of the ram adaptor
void EINT0_IRQHandler(void)
{
    GPIO_CLR_INT_FLAG(PB, BIT14);

//	printf("EINT0_IRQHandler");
	if(IS_WRITE()) {
		int ra = TIMER_GetCounter(TIMER0);

		TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
		TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;
		writelen = (uint8_t)ra;
		havewrite++;
	}
}

volatile int bufpos,sentbufpos;

volatile diskread_t diskread;

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
		decodebuf[bufpos++] = (uint8_t)ra;
		if(bufpos >= DECODEBUFSIZE) {
			bufpos = 0;
		}
    }
}

__inline uint8_t raw_to_raw03_byte(uint8_t raw)
{
/*
  59 59 5a 5b 5b 5b 5a 5b
  89 b8 89 5a 5b 5a 5b 89
*/
	if(raw < 0x50)
		return(3);
	else if(raw < 0x78)
		return(0);
	else if(raw < 0xA0)
		return(1);
	else if(raw < 0xD0)
		return(2);
	return(3);
}

__inline void decode(uint8_t *dst, uint8_t src, int dstSize, int *outptr) {
	static char bitval;
	int out = *outptr;

	if(dst == 0) {
		bitval = 0;
		return;
	}
	switch(src | (bitval << 4)) {
		case 0x11:
			out++;
		case 0x00:
			out++;
			bitval=0;
			break;
		case 0x12:
			out++;
		case 0x01:
		case 0x10:
			dst[out/8] |= 1 << (out & 7);
			out++;
			bitval=1;
			break;
		default: //Unexpected value.  Keep going, we'll probably get a CRC warning
//				printf("glitch(%d) @ %X(%X.%d)\n", src[in], in, out/8, out%8);
			out++;
			bitval=0;
			break;
	}
	*outptr = out;
}

void hexdump(char *desc, void *addr, int len);

static void begin_transfer(void)
{
	int i, j;
	int decodelen = 0;
	int leadin = DEFAULT_LEAD_IN;

	printf("beginning transfer...\r\n");

	flash_read_start(diskblock * 0x10000 + 256);
	needbyte = 0;
	count = 7;
	havewrite = 0;
	writelen = 0;
	
	write_num = 0;
	
	for(i=0;i<DECODEBUFSIZE;i++) {
		decodebuf[i] = 0;
	}

    NVIC_DisableIRQ(USBD_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
    NVIC_EnableIRQ(EINT0_IRQn);
    NVIC_EnableIRQ(TMR1_IRQn);	
	TIMER_Start(TIMER0);
	TIMER_Start(TIMER1);

	bytes = 0;
	needbyte = 0;
	count = 7;
	havewrite = 0;
	writelen = 0;

	//transfer lead-in
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		if(needbyte) {
			needbyte = 0;
			data2 = 0;
			leadin -= 8;
			if(leadin <= 0) {
				flash_read((uint8_t*)&data2,1);
				bytes++;
				break;
			}
		}
	}

	//transfer disk data
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		if(IS_WRITE()) {
			int len = 0;

			if(write_num >= 8) {
				printf("too many writes!\n");
				break;
			}
			writes[write_num].diskpos = bytes + 2;
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			TIMER0->TCMPR = 0xFFFFFF;
			TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;
			decode(0,0,0,0);
			while(IS_WRITE()) {
				if(havewrite) {
					havewrite = 0;
					if(len < DECODEBUFSIZE) {
						decode(decodebuf + decodelen,raw_to_raw03_byte(writelen),DECODEBUFSIZE,&len);
					}
					else {
						printf("decodebuf is too small\n");
						break;
					}
				}
				if(needbyte) {
					needbyte = 0;
					flash_read((uint8_t*)&data2,1);
					bytes++;
					if(bytes >= 0xFF00) {
						printf("reached end of data block, something went wrong...\r\n");
						break;
					}
				}
			}
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			len = (len / 8) + 2;
			writes[write_num].decstart = decodelen;
			writes[write_num].decend = decodelen + len;
			printf("finished write %d, start = %d, end = %d (len = %d)\r\n",write_num,decodelen,decodelen + len,len);
			decodelen += len;
			write_num++;
		}
		if(needbyte) {
			needbyte = 0;
			flash_read((uint8_t*)&data2,1);
			bytes++;
			if(bytes >= 0xFF00) {
				printf("reached end of data block, something went wrong...\r\n");
				break;
			}
		}
	}

    NVIC_DisableIRQ(EINT0_IRQn);
    NVIC_DisableIRQ(TMR1_IRQn);
	TIMER_Stop(TIMER0);
	TIMER_Stop(TIMER1);
    NVIC_EnableIRQ(USBD_IRQn);

	flash_read_stop();
	
	//needs to be cleaned up/optimized
	if(write_num) {

		//write the written data to flash
		for(i=0;i<write_num;i++) {
			int pos = writes[i].diskpos + 256;
			int start = writes[i].decstart;
			int end = writes[i].decend;
			int size = (end - start);// + 122 + 40;
			int sector = pos >> 12;
			int sectoraddr = pos & 0xFFF;
			uint8_t *decodeptr = decodebuf + start;

			printf("writing to flash: diskpos %d, size %d, sector = %d, sectoraddr = %X (decstart = %d, decsize = %d)\r\n",pos,size,sector,sectoraddr,start,size);
			flash_read_sector(diskblock,sector,sectorbuf);
			for(j=sectoraddr;j<4096 && size;j++, size--) {
				sectorbuf[j] = *decodeptr++;
			}
			flash_write_sector(diskblock,sector,sectorbuf);
			if(size) {
				printf("write spans two sectors...\n");
				sector++;
				flash_read_sector(diskblock,sector,sectorbuf);
				for(j=0;j<4096;j++, size--) {
					sectorbuf[j] = *decodeptr++;
				}
				flash_write_sector(diskblock,sector,sectorbuf);
				if(size) {
					printf("write spans three sectors!!  D: D: D:\n");
				}
			}
		}
	}
	printf("transferred %d bytes\r\n",bytes);
}

//string to find to start sending the fake disklist
uint8_t diskliststr[17] = {0x80,0x03,0x07,0x10,'D','I','S','K','L','I','S','T',0x00,0x80,0x00,0x10,0x00};

int find_disklist()
{
	int pos = 0;
	int count = 0;
	uint8_t byte;

	flash_read_start(0 + 256);
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

uint8_t *disklistblock = decodebuf + 4096;
uint8_t *disklist = decodebuf + 4096 + 1;

void create_disklist(void)
{
	uint8_t *list = disklist + 32;
	flash_header_t header;
	int blocks = flash_get_total_blocks();
	int i,num = 0;
	uint32_t crc;

	memset(disklist,0,4096 + 2);

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
	crc = calc_crc(disklistblock,4096 + 1 + 2);
	disklist[4096] = (uint8_t)(crc >> 0);
	disklist[4097] = (uint8_t)(crc >> 8);
}

static void begin_transfer_loader(void)
{
	int i, j;
	int decodelen = 0;
	int leadin = DEFAULT_LEAD_IN;
	static int disklistpos = -1;

	printf("beginning loader transfer...\r\n");

	if(disklistpos == -1) {
		disklistpos = find_disklist();
		printf("find_disklist() = %d\n",disklistpos);
		create_disklist();
	}

	flash_read_start(diskblock * 0x10000);
	needbyte = 0;
	count = 7;
	havewrite = 0;
	writelen = 0;
	
	write_num = 0;
	
	for(i=0;i<1024;i++) {
		decodebuf[i] = 0;
	}

    NVIC_DisableIRQ(USBD_IRQn);
    NVIC_DisableIRQ(GPAB_IRQn);
    NVIC_EnableIRQ(EINT0_IRQn);
    NVIC_EnableIRQ(TMR1_IRQn);	
	TIMER_Start(TIMER0);
	TIMER_Start(TIMER1);

	bytes = 0;
	needbyte = 0;
	count = 7;
	havewrite = 0;
	writelen = 0;

	//transfer lead-in
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		if(needbyte) {
			needbyte = 0;
			data2 = 0;
			leadin -= 8;
			if(leadin <= 0) {
				flash_read((uint8_t*)&data2,1);
				bytes++;
				break;
			}
		}
	}

	//transfer disk data
	while(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
		if(IS_WRITE()) {
			int len = 0;

			writes[write_num].diskpos = bytes + 2;
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			TIMER0->TCMPR = 0xFFFFFF;
			TIMER0->TCSR = TIMER_CONTINUOUS_MODE | 7 | TIMER_TCSR_TDR_EN_Msk | TIMER_TCSR_CEN_Msk;
			decode(0,0,0,0);
			while(IS_WRITE()) {
				if(havewrite) {
					havewrite = 0;
					decode(decodebuf + decodelen,raw_to_raw03_byte(writelen),DECODEBUFSIZE,&len);
				}
				if(needbyte) {
					needbyte = 0;
					flash_read((uint8_t*)&data2,1);
					bytes++;
					if(bytes >= 0xFF00) {
						printf("reached end of data block, something went wrong...\r\n");
						break;
					}
				}
			}
			TIMER0->TCSR = TIMER_TCSR_CRST_Msk;
			len = (len / 8) + 2;
			writes[write_num].decstart = decodelen;
			writes[write_num].decend = decodelen + len;
			printf("finished write %d, start = %d, end = %d (len = %d)\r\n",write_num,decodelen,decodelen + len,len);
			decodelen += len;
			write_num++;
		}
		if(needbyte) {
			needbyte = 0;
			flash_read((uint8_t*)&data2,1);
			if(bytes >= disklistpos) {
				int n = bytes - disklistpos;
				if(n < (4096 + 2)) {
					data2 = disklist[n];
				}
				else {
					data2 = 0;
				}
			}
			bytes++;
			if(bytes >= 0xFF00) {
				printf("reached end of data block, something went wrong...\r\n");
				break;
			}
		}
	}
    NVIC_DisableIRQ(EINT0_IRQn);
    NVIC_DisableIRQ(TMR1_IRQn);
	TIMER_Stop(TIMER0);
	TIMER_Stop(TIMER1);
    NVIC_EnableIRQ(USBD_IRQn);

	flash_read_stop();
	
	//loader
	if(write_num) {
		uint8_t *ptr = &decodebuf[1024];
		int in,out;
		
		//ptr should look like this: $80 $02 $dd
		//where $dd is the new diskblock
//		hexdump("decodebuf",decodebuf,256);
		bin_to_raw03(decodebuf,sectorbuf,writes[0].decend,4096);
		in = 0;
		out = 0;
		block_decode(&decodebuf[1024],sectorbuf,&in,&out,4096,1024,2,2);
		
//		hexdump("&decodebuf[1024]",&decodebuf[1024],256);

		printf("loader exiting, new diskblock = %d\n",ptr[1]);
		fds_insert_disk(ptr[1]);
		return;
	}

	printf("transferred %d bytes\r\n",bytes);
}

int needfinish;

void fds_start_diskread(void)
{
	//clear decodebuf
	memset(decodebuf,0,DECODEBUFSIZE);
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
		ret = DECODEBUFSIZE - sentbufpos;
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
	if(t >= DECODEBUFSIZE) {
		v = DECODEBUFSIZE - sentbufpos;
		w = len - v;
		memcpy(bufbuf,decodebuf + sentbufpos,v);
		memcpy(bufbuf + v,decodebuf,w);
		sentbufpos = w;
	}
	
	//this read will be one unbroken chunk of the buffer
	else {
		memcpy(bufbuf,decodebuf + sentbufpos,len);
		sentbufpos += len;
	}
}

void fds_init(void)
{
	int usbattached = USBD_IS_ATTACHED();
	
	usbattached = 0;
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
		
		//wait for button to be released before we insert disk
		while(PA10 != 0);

		SET_MEDIASET();			
	}
	
	//check if ram adaptor wants to stop the motor
	if(IS_STOPMOTOR()) {
		CLEAR_MOTORON();
	}

	//if ram adaptor wants to scan the media
	if(IS_SCANMEDIA()) {

		//if ram adaptor doesnt want to stop the motor
		if(IS_DONT_STOPMOTOR()) {

			SET_MOTORON();

			TIMER_Delay(TIMER2, 20 * 1000);

			if(IS_SCANMEDIA() && IS_DONT_STOPMOTOR()) {
				TIMER_Delay(TIMER2, 130 * 1000);
				SET_READY();
				if(diskblock == 0) {
					begin_transfer_loader();
				}
				else {
					begin_transfer();
				}
				CLEAR_READY();
			}
		}
	}
}

void fds_insert_disk(int block)
{
	diskblock = block;
	fds_setup_transfer();
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
