#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <tchar.h>
#include <assert.h>
#include <vector>
#include <list>
#include <algorithm>

#include "boost2005.h"
#include "user_message.h"
#include "defines.h"
#include "mlist.h"
#include "carray.h"
#include "cevent.h"
#include "errormsg.h"
#include "CDPI.h"
#include "bits.h"
#include "util.h"
#include "utils.h"
#include "register.h"
#include "cart.h"


CartKcsPower::CartKcsPower(const CrtHeader &crtHeader, IC6510 *pCpu, bit8 *pC64RamMemory)
	: CartCommon(crtHeader, pCpu, pC64RamMemory)
{
}

void CartKcsPower::LatchShift()
{
	reg1 = ((reg1 & ~5) | ((reg1 & 0xA) >> 1)) & 0xff;//Move old EXROM in bit 3 to bit 2. Move old GAME in bit 1 to bit 0
}

bit8 CartKcsPower::ReadRegister(bit16 address, ICLK sysclock)
{
bit16 addr;
bit8 v;
	if (address >= 0xDE00 && address < 0xDF00)
	{
		bit8 old1 = reg1;
		LatchShift();
		addr = address - 0xDE00 + 0x9E00;
		v = this->m_ipROML_8000[addr];
		if (addr & 1)
		{
			reg1 |= 8;
		}
		else
		{
			reg1 &= ((~8) & 0xff);
		}
		reg1 |= 2;//Reading with (IO1==0). Set GAME
		if (old1 != reg1)
			ConfigureMemoryMap();
		return v;
	}
	else if (address >= 0xDF00 && address < 0xE000)
	{
		addr = (address - 0xDF00) & 0x7f;
		if (addr & 0x80)
		{
			////TEST {
			//m_pCpu->Clear_CRT_NMI();
			//LatchShift();
			//
			////Ultimax
			//reg1 &= ((~2) & 0xff);//Clear GAME
			//reg1 |= 8;//Set EXROM 

			//ConfigureMemoryMap();
			////TEST }
		}
		//if (addr & 0x10)
		//{
		//	return ((reg1 & 4) << 5) || ((reg1 & 1) << 6);
		//}
		//else
		{
			return m_pCartData[addr];
		}	
	}
	return 0;
}

void CartKcsPower::WriteRegister(bit16 address, ICLK sysclock, bit8 data)
{
bit16 addr;
	bit8 old1 = reg1;
	if (address >= 0xDE00 && address < 0xDF00)
	{
		addr = address - 0xDE00 + 0x9E00;
		LatchShift();
		if (addr & 1)
		{
			reg1 |= 8;//Writing with (A1==1 && IO1==0). Set EXROM
		}
		else
		{
			reg1 &= ~8;//Writing with (A1==0 && IO1==0). Clear EXROM
		}
		reg1 &= ((~2) & 0xff);//Writing with (IO1==0). Clear GAME
	}
	else if (address >= 0xDF00 && address < 0xE000)
	{
		addr = (address - 0xDF00) & 0x7f;
		////TEST {
		//LatchShift();
		//if (addr & 1)
		//{
		//	reg1 |= 8;//Writing with (A1==1 && IO1==0). Set EXROM
		//}
		//else
		//{
		//	reg1 &= ~8;//Writing with (A1==0 && IO1==0). Clear EXROM
		//}
		//reg1 &= ((~2) & 0xff);//Writing with (IO1==0). Clear GAME
		//TEST }
		//if (addr & 0x10)
		//{
		//}
		//else
		{
			m_pCartData[addr] = data;
		}
	}
	if (old1 != reg1)
		ConfigureMemoryMap();
}

void CartKcsPower::CartFreeze()
{
	if (m_bIsCartAttached)
	{
		//m_pCpu->Set_CRT_IRQ(m_pCpu->Get6510CurrentClock());
		m_pCpu->Set_CRT_NMI(m_pCpu->Get6510CurrentClock());
		m_bFreezePending = true;
		m_bFreezeDone = false;
	}
}

void CartKcsPower::CheckForCartFreeze()
{
	if (m_bFreezePending)
	{
		m_bFreezePending = false;
		m_bFreezeDone = false;
		LatchShift();
		reg1 = 8;//Set EXROM in bit 2.
		m_pCpu->Clear_CRT_IRQ();
		m_pCpu->Clear_CRT_NMI();
		ConfigureMemoryMap();
	}
}

void CartKcsPower::UpdateIO()
{
	m_iSelectedBank = 0;
	m_bEnableRAM = false;
	m_iRamBankOffsetIO = 0;
	m_iRamBankOffsetRomL = 0;
	m_bIsCartIOActive = true;
	GAME = (reg1 & 2) != 0;//bit 1 sets GAME. bit 0 reads GAME on latching.
	EXROM = (reg1 & 8) != 0;//bit 3 sets EXROM. bit 2 reads EXROM on latching.
	BankRom();
	//if (m_bFreezePending)
	//{
	//}
	//else if (m_bFreezeDone)	
	//{
	//}
	//else
	//{
	//}			
}
