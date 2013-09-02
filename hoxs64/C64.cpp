#include <windows.h>
#include <commctrl.h>
#include "dx_version.h"
#include <d3d9.h>
#include <d3dx9core.h>
#include <dinput.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <tchar.h>
#include <assert.h>
#include "boost2005.h"
#include "CDPI.h"
#include "filestream.h"
#include "carray.h"
#include "mlist.h"
#include "bits.h"
#include "Huff.h"
#include "C64.h"
#include "savestate.h"

C64::C64()
{
	pIC64Event = 0;
	bPendingSystemCommand = false;
	m_SystemCommand = C64CMD_NONE;
}

C64::~C64()
{
}

HRESULT C64::Init(CConfig *cfg, CAppStatus *appStatus, IC64Event *pIC64Event, CDX9 *dx, TCHAR *szAppDirectory)
{
	ClearError();

	this->cfg = cfg;
	this->appStatus = appStatus;
	this->pIC64Event = pIC64Event;
	this->dx = dx;
	if (szAppDirectory==NULL)
		m_szAppDirectory[0] = 0;
	else
		_tcscpy_s(m_szAppDirectory, _countof(m_szAppDirectory), szAppDirectory);

	tape64.TapeEvent = (ITapeEvent *)this;

	if (ram.Init(m_szAppDirectory, &cart)!=S_OK) return SetError(ram);

	if (cpu.Init(pIC64Event, CPUID_MAIN, &cia1, &cia2, &vic, &sid, &cart, &ram, static_cast<ITape *>(&tape64), &mon)!=S_OK) return SetError(cpu);
	cart.Init(&cpu, ram.mMemory);
	if (cia1.Init(cfg, appStatus, static_cast<IC64 *>(this), &cpu, &vic, &tape64, dx, static_cast<IAutoLoad *>(this))!=S_OK) return SetError(cia1);
	if (cia2.Init(cfg, appStatus, &cpu, &vic, &diskdrive)!=S_OK) return SetError(cia2);

	if (vic.Init(cfg, appStatus, dx, &ram, &cpu, &mon)!=S_OK) return SetError(vic);

	if (sid.Init(cfg, appStatus, dx, cfg->m_fps)!=S_OK) return SetError(sid);

	if (diskdrive.Init(cfg, appStatus, pIC64Event, &mon, szAppDirectory)!=S_OK) return SetError(diskdrive);

	if (mon.Init(pIC64Event, &cpu, &diskdrive.cpu, &vic, &diskdrive)!=S_OK) return SetError(E_FAIL, TEXT("C64 monitor initialisation failed"));

	return S_OK;
}

void C64::InitReset(ICLK sysclock)
{
	m_iClockOverflowCheckCounter = 0;
	tape64.CurrentClock = sysclock;
	vic.CurrentClock = sysclock;
	cia1.CurrentClock = sysclock;
	cia2.CurrentClock = sysclock;
	sid.CurrentClock = sysclock;
	cpu.CurrentClock = sysclock;
	diskdrive.CurrentPALClock = sysclock;
	diskdrive.CurrentClock = sysclock;
	diskdrive.cpu.CurrentClock = sysclock;
	diskdrive.via1.CurrentClock = sysclock;
	diskdrive.via2.CurrentClock = sysclock;

	tape64.nextTapeTickClock = sysclock;
	cia1.nextKeyboardScanClock = sysclock;
	cia1.ClockNextWakeUpClock = sysclock;
	cia2.ClockNextWakeUpClock = sysclock;
	m_bLastPostedDriveWriteLed = false;

	ram.InitReset();
	vic.InitReset(sysclock);
	cia1.InitReset(sysclock);
	cia2.InitReset(sysclock);
	sid.InitReset(sysclock);
	//cart.InitReset(sysclock);
	cpu.InitReset(sysclock);
	diskdrive.InitReset(sysclock);
}

void C64::Reset(ICLK sysclock)
{
	diskdrive.WaitThreadReady();

	InitReset(sysclock);

	tape64.PressStop();
	ram.Reset();
	vic.Reset(sysclock);
	cia1.Reset(sysclock);
	cia2.Reset(sysclock);
	sid.Reset(sysclock);
	//The cpu reset must be called before the cart reset to allow the cart to assert interrupts if any.
	cpu.Reset(sysclock);
	cart.Reset(sysclock);
	diskdrive.Reset(sysclock);
	cpu.SetCassetteSense(1);

	pIC64Event->DiskMotorLed(diskdrive.m_bDiskMotorOn);
	pIC64Event->DiskDriveLed(diskdrive.m_bDriveLedOn);
	pIC64Event->DiskWriteLed(diskdrive.m_bDriveWriteWasOn);
	m_bLastPostedDriveWriteLed = diskdrive.m_bDriveWriteWasOn;
}

void C64::PreventClockOverflow()
{
	if (++m_iClockOverflowCheckCounter > 0x100)
	{
		m_iClockOverflowCheckCounter=0;
		cpu.PreventClockOverflow();
		cia1.PreventClockOverflow();
		cia2.PreventClockOverflow();
		vic.PreventClockOverflow();
		diskdrive.PreventClockOverflow();
		cart.PreventClockOverflow();
	}
}

void C64::EnterDebugRun(bool bWithSound)
{
	diskdrive.WaitThreadReady();
	assert(vic.CurrentClock == cpu.CurrentClock);
	assert(vic.CurrentClock == cia1.CurrentClock);
	assert(vic.CurrentClock == cia2.CurrentClock);
	assert(vic.CurrentClock == diskdrive.CurrentPALClock || cfg->m_bD1541_Emulation_Enable==0);

	if (cfg->m_bD1541_Emulation_Enable==0)
		diskdrive.CurrentPALClock = vic.CurrentClock;

	if (bWithSound && appStatus->m_bSoundOK && appStatus->m_bFilterOK)
		sid.LockSoundBuffer();

	cpu.StartDebug();
	diskdrive.cpu.StartDebug();
}

void C64::FinishDebugRun()
{
	sid.UnLockSoundBuffer();
	cpu.StopDebug();
	diskdrive.cpu.StopDebug();

	PreventClockOverflow();
	CheckDriveLedNofication();
}


void C64::SynchroniseDevicesWithVIC()
{
ICLK sysclock;
	sysclock = vic.CurrentClock;
	diskdrive.WaitThreadReady();
	cpu.StartDebug();
	diskdrive.cpu.StartDebug();
	if (diskdrive.CurrentPALClock != sysclock)
	{
		sysclock = sysclock;
	}
	cpu.ExecuteCycle(sysclock);
	//cart.ExecuteCycle(sysclock);
	cia1.ExecuteCycle(sysclock);
	cia2.ExecuteCycle(sysclock);
	if (cfg->m_bSID_Emulation_Enable)
	{
		sid.ExecuteCycle(sysclock);
	}
	if (cfg->m_bD1541_Emulation_Enable)
	{
		diskdrive.ExecutePALClock(sysclock);
	}
	cpu.StopDebug();
	diskdrive.cpu.StopDebug();
}

void C64::ExecuteDiskClock()
{
ICLK sysclock;

	if (!cfg->m_bD1541_Emulation_Enable)
		return;

	EnterDebugRun(false);
	sysclock = diskdrive.CurrentPALClock;
	
	do
	{
		if (diskdrive.m_pendingclocks == 0)
			sysclock++;

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock-1);
			if (diskdrive.m_pendingclocks>1)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
				break;
			}
		}
		
		vic.ExecuteCycle(sysclock);
		cia1.ExecuteCycle(sysclock);
		cia2.ExecuteCycle(sysclock);
		//cart.ExecuteCycle(sysclock);
		cpu.ExecuteCycle(sysclock); 

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock);
			if (diskdrive.m_pendingclocks>0)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
			}
		}
		if (cfg->m_bSID_Emulation_Enable)
		{
			sid.ExecuteCycle(sysclock);
		}
	} while (false);
	FinishDebugRun();
}

void C64::ExecuteDiskInstruction()
{
ICLK sysclock;
bool bBreak;

	if (!cfg->m_bD1541_Emulation_Enable)
		return;

	EnterDebugRun(false);
	sysclock = diskdrive.CurrentPALClock;
	
	bBreak = false;
	while(!bBreak)
	{
		if (diskdrive.m_pendingclocks == 0)
			sysclock++;

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock-1);
			while (diskdrive.m_pendingclocks>1)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
				if (diskdrive.cpu.IsOpcodeFetch())
				{
					bBreak = true;
					break;
				}
			}
			if (bBreak)
				break;
		}

		vic.ExecuteCycle(sysclock);
		cia1.ExecuteCycle(sysclock);
		cia2.ExecuteCycle(sysclock);
		//cart.ExecuteCycle(sysclock);
		cpu.ExecuteCycle(sysclock); 

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock);
			while (diskdrive.m_pendingclocks>0)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
				if (diskdrive.cpu.IsOpcodeFetch())
				{
					bBreak = true;
					break;
				}
			}
		}
		if (cfg->m_bSID_Emulation_Enable)
		{
			sid.ExecuteCycle(sysclock);
		}
		if (bBreak || diskdrive.cpu.m_cpu_sequence==HLT_IMPLIED)
			break;

	}
	FinishDebugRun();
}

void C64::ExecuteC64Clock()
{
ICLK sysclock;

	EnterDebugRun(false);
	sysclock = vic.CurrentClock;
	
	do
	{
		sysclock++;

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock-1);
			while (diskdrive.m_pendingclocks>1)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
			}
		}

		vic.ExecuteCycle(sysclock);
		cia1.ExecuteCycle(sysclock);
		cia2.ExecuteCycle(sysclock);
		//cart.ExecuteCycle(sysclock);
		cpu.ExecuteCycle(sysclock); 

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock);
			while (diskdrive.m_pendingclocks>0)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
			}
		}
		if (cfg->m_bSID_Emulation_Enable)
		{
			sid.ExecuteCycle(sysclock);
		}
	} while (false);
	FinishDebugRun();
}

void C64::ExecuteC64Instruction()
{
ICLK sysclock;
bool bBreak;

	EnterDebugRun(false);
	sysclock = vic.CurrentClock;
	
	bBreak = false;
	while(!bBreak)
	{
		sysclock++;

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock-1);
			while (diskdrive.m_pendingclocks>1)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
			}
		}

		bool bWasC64CpuOpCodeFetch = cpu.IsOpcodeFetch();

		vic.ExecuteCycle(sysclock);
		cia1.ExecuteCycle(sysclock);
		cia2.ExecuteCycle(sysclock);
		//cart.ExecuteCycle(sysclock);
		cpu.ExecuteCycle(sysclock); 

		if (cpu.IsOpcodeFetch() && !bWasC64CpuOpCodeFetch)
		{
			bBreak = true;
		}

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock);
			while (diskdrive.m_pendingclocks>0)
			{
				diskdrive.ExecuteOnePendingDiskCpuClock();
			}
		}
		if (cfg->m_bSID_Emulation_Enable)
		{
			sid.ExecuteCycle(sysclock);
		}
		if (bBreak || cpu.m_cpu_sequence==HLT_IMPLIED)
			break;

	}
	FinishDebugRun();
}

void C64::ExecuteDebugFrame()
{
ICLK cycles,sysclock;
bool bBreakC64, bBreakDisk, bBreakVic;

	if (bPendingSystemCommand)
	{
		ProcessReset();
	}
	if (vic.vic_raster_line==PAL_MAX_LINE && vic.vic_raster_cycle==63)
		cycles = (PAL_MAX_LINE+1)*63;
	else if (vic.vic_check_irq_in_cycle2)
		cycles = (PAL_MAX_LINE+1)*63 -1;
	else
		cycles = (PAL_MAX_LINE+1-vic.vic_raster_line)*63 - (vic.vic_raster_cycle);

	sysclock = vic.CurrentClock;
	
	EnterDebugRun(true);
	bBreakC64 = false;
	bBreakDisk = false;
	bBreakVic = false;
	while(cycles > 0)
	{
		cycles--;
		sysclock++;

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock-1);
			while (diskdrive.m_pendingclocks > 1)
			{
				bool bWasDiskCpuOnInt = diskdrive.cpu.IsInterruptInstruction();
				diskdrive.ExecuteOnePendingDiskCpuClock();
				if (diskdrive.cpu.IsOpcodeFetch())
				{
					if (diskdrive.cpu.PROCESSOR_INTERRUPT == 0)
					{
						if (diskdrive.cpu.CheckExecute(diskdrive.cpu.mPC.word, true) == 0)
						{
							bBreakDisk = true;
							break;
						}
					}
				}
				if (diskdrive.cpu.GetBreakOnInterruptTaken())
				{
					if (diskdrive.cpu.IsInterruptInstruction() && (diskdrive.cpu.IsOpcodeFetch() || !bWasDiskCpuOnInt))
						bBreakDisk = true;
				}
			}
 			if (bBreakDisk)
				break;
		}

		bool bWasC64CpuOpCodeFetch = cpu.IsOpcodeFetch();
		bool bWasC64CpuOnInt = cpu.IsInterruptInstruction();

		vic.ExecuteCycle(sysclock);
		cia1.ExecuteCycle(sysclock);
		cia2.ExecuteCycle(sysclock);
		//cart.ExecuteCycle(sysclock);
		cpu.ExecuteCycle(sysclock); 

		if (vic.CheckBreakpointRasterCompare(vic.GetNextRasterLine(), vic.GetNextRasterCycle(), true) == 0)
		{
			bBreakVic = true;
		}

		if (cpu.IsOpcodeFetch() && !bWasC64CpuOpCodeFetch)
		{			
			if (cpu.PROCESSOR_INTERRUPT == 0)
			{
				if (cpu.CheckExecute(cpu.mPC.word, true) == 0)
				{
					bBreakC64 = true;
				}
			}
		}
		if (cpu.GetBreakOnInterruptTaken())
		{
			if (cpu.IsInterruptInstruction() && (cpu.IsOpcodeFetch() || !bWasC64CpuOnInt))
				bBreakC64 = true;
		}

		if (cfg->m_bD1541_Emulation_Enable)
		{
			diskdrive.AccumulatePendingDiskCpuClocksToPalClock(sysclock);
			while (diskdrive.m_pendingclocks>0)
			{
				bool bWasDiskCpuOnInt = diskdrive.cpu.IsInterruptInstruction();
				diskdrive.ExecuteOnePendingDiskCpuClock();
				if (diskdrive.cpu.IsOpcodeFetch())
				{
					if (diskdrive.cpu.PROCESSOR_INTERRUPT == 0)
					{
						if (diskdrive.cpu.CheckExecute(diskdrive.cpu.mPC.word, true) == 0)
						{
							bBreakDisk = true;
							break;
						}
					}
				}
				if (diskdrive.cpu.GetBreakOnInterruptTaken())
				{
					if (diskdrive.cpu.IsInterruptInstruction() && (diskdrive.cpu.IsOpcodeFetch() || !bWasDiskCpuOnInt))
						bBreakDisk = true;
				}
			}
		}
		if (cfg->m_bSID_Emulation_Enable)
		{
			sid.ExecuteCycle(sysclock);
		}
 		if (bBreakC64 || bBreakDisk || bBreakVic)
			break;

	}
	FinishDebugRun();
	if (pIC64Event)
	{
		if (bBreakC64)
		{
			pIC64Event->BreakExecuteCpu64();
		}
		if (bBreakDisk)
		{
			pIC64Event->BreakExecuteCpuDisk();
		}
		if (bBreakVic)
		{
			pIC64Event->BreakVicRasterCompare();
		}
	}
}

void C64::ExecuteFrame()
{
ICLK cycles,sysclock;

	if (bPendingSystemCommand)
	{
		ProcessReset();
	}
	if (vic.vic_raster_line==PAL_MAX_LINE && vic.vic_raster_cycle==63)
		cycles = (PAL_MAX_LINE+1)*63;
	else if (vic.vic_check_irq_in_cycle2)
		cycles = (PAL_MAX_LINE+1)*63 -1;
	else
		cycles = (PAL_MAX_LINE+1-vic.vic_raster_line)*63 - (vic.vic_raster_cycle);

	sysclock = vic.CurrentClock + cycles;
	
	if (appStatus->m_bSoundOK && appStatus->m_bFilterOK)
		sid.LockSoundBuffer();

	BOOL bIsDiskEnabled = cfg->m_bD1541_Emulation_Enable;
	BOOL bIsDiskThreadEnabled = cfg->m_bD1541_Thread_Enable;
	if (bIsDiskEnabled && !bIsDiskThreadEnabled)
	{
		diskdrive.ExecuteAllPendingDiskCpuClocks();
	}

	cpu.ExecuteCycle(sysclock);	
	vic.ExecuteCycle(sysclock);
	//cart.ExecuteCycle(sysclock);
	cia1.ExecuteCycle(sysclock);
	cia2.ExecuteCycle(sysclock);	
	if (bIsDiskEnabled)
	{
		if (bIsDiskThreadEnabled && !appStatus->m_bSerialTooBusyForSeparateThread)
		{
			diskdrive.ThreadSignalCommandExecuteClock(sysclock);
		}
		else
		{
			diskdrive.ExecutePALClock(sysclock);
			appStatus->m_bSerialTooBusyForSeparateThread = false;
		}
	}
	if (cfg->m_bSID_Emulation_Enable)
	{
		sid.ExecuteCycle(sysclock);
	}
	sid.UnLockSoundBuffer();
	CheckDriveLedNofication();
}

void C64::CheckDriveLedNofication()
{
	if (pIC64Event)
	{
		if ((diskdrive.m_d64_write_enable == 0) && (diskdrive.CurrentClock - diskdrive.m_driveWriteChangeClock) > 200000L) 
		{
			diskdrive.m_bDriveWriteWasOn = false;
		}
		if (m_bLastPostedDriveWriteLed != diskdrive.m_bDriveWriteWasOn)
		{
			m_bLastPostedDriveWriteLed = diskdrive.m_bDriveWriteWasOn;
			pIC64Event->DiskWriteLed(diskdrive.m_bDriveWriteWasOn);
		}

		pIC64Event->DiskMotorLed(diskdrive.m_bDiskMotorOn);
		pIC64Event->DiskDriveLed(diskdrive.m_bDriveLedOn);
	}
}

void C64::ResetKeyboard()
{
	cia1.ResetKeyboard();
}

void C64::SetBasicProgramEndAddress(bit16 last_byte)
{
	//last_byte=start+code_size-1;
	ram.mMemory[0x2d]=(bit8)(last_byte+1);
	ram.mMemory[0x2e]=(bit8)((last_byte+1)>>8);
	ram.mMemory[0x2f]=(bit8)(last_byte+1);
	ram.mMemory[0x30]=(bit8)((last_byte+1)>>8);
	ram.mMemory[0x31]=(bit8)(last_byte+1);
	ram.mMemory[0x32]=(bit8)((last_byte+1)>>8);
	ram.mMemory[0xae]=(bit8)(last_byte+1);
	ram.mMemory[0xaf]=(bit8)((last_byte+1)>>8);
}


#define ClocksFromFrame(x) (PALCLOCKSPERSECOND*x)
void C64::WriteSysCallToScreen(bit16 startaddress)
{
char szAddress[7];
const char szSysCall[] = {19,25,19,32,0};
	CopyMemory(&ram.miMemory[SCREENWRITELOCATION], szSysCall, strlen(szSysCall));
	szAddress[0] = 0;
	sprintf_s(szAddress, _countof(szAddress), "%ld", startaddress);
	unsigned int i;
	for (i=0 ; i < strlen(szAddress) ; i++)
		ram.miMemory[SCREENWRITELOCATION + 4 + i] = szAddress[i];
}

void C64::AutoLoadHandler(ICLK sysclock)
{
const int resettime=120;
const char szRun[] = {18,21,14,0};
const char szLoadDisk[] = {12,15,1,4,34,42,34,44,56,44,49,0};
const char szSysCall[] = {19,25,19,32,0};
const int LOADPREFIXLENGTH = 5;
const int LOADPOSTFIXINDEX = 6;
const int LOADPOSTFIXLENGTH = 5;
HRESULT hr;
ICLK period;
bit16 loadSize;
int directoryIndex;
char szAddress[7];
LPCTSTR SZAUTOLOADTITLE = TEXT("C64 auto load");

	if (autoLoadCommand.sequence == AUTOSEQ_RESET)
	{
		appStatus->m_bAutoload = TRUE;
		autoLoadCommand.sequence = C64::AUTOSEQ_LOAD;
	}

	period = sysclock / (PALCLOCKSPERSECOND / 50);
	if (period < resettime)
	{
		return;	
	}
	switch (autoLoadCommand.type)
	{
	case C64::AUTOLOAD_TAP_FILE:
		if (period < (resettime + 3))
		{
		}
		else if (period < (resettime + 6))
		{
			cia1.SetKeyMatrixDown(1, 7);//lshift
		}
		else if (period  < (resettime + 9))
		{
			cia1.SetKeyMatrixDown(1, 7);//lshift
			cia1.SetKeyMatrixDown(7, 7);//stop
		}
		else if (period < (resettime + 12))
		{
			break;
		}
		else 
		{
			TapePressPlay();
			autoLoadCommand.CleanUp();
			appStatus->m_bAutoload = FALSE;
		}
		break;
	case C64::AUTOLOAD_PRG_FILE:		
	case C64::AUTOLOAD_T64_FILE:	
		if (autoLoadCommand.sequence == C64::AUTOSEQ_LOAD)
		{
			autoLoadCommand.sequence = C64::AUTOSEQ_RUN;
			if (autoLoadCommand.type == AUTOLOAD_PRG_FILE)
			{
				hr = LoadImageFile(&autoLoadCommand.filename[0], &autoLoadCommand.startaddress, &autoLoadCommand.imageSize);
			}
			else
			{
				directoryIndex  = (autoLoadCommand.directoryIndex < 0) ? 0 : autoLoadCommand.directoryIndex;
				hr = LoadT64ImageFile(&autoLoadCommand.filename[0], directoryIndex, &autoLoadCommand.startaddress, &autoLoadCommand.imageSize);
			}
			if (SUCCEEDED(hr))
			{
				if (autoLoadCommand.startaddress <= BASICSTARTADDRESS)
				{
					SetBasicProgramEndAddress(autoLoadCommand.startaddress + autoLoadCommand.imageSize - 1);
				}
				else
				{
					CopyMemory(&ram.miMemory[SCREENWRITELOCATION], szSysCall, strlen(szSysCall));
					szAddress[0] = 0;
					sprintf_s(szAddress, _countof(szAddress), "%ld", autoLoadCommand.startaddress);
					unsigned int i;
					for (i=0 ; i < strlen(szAddress) ; i++)
						ram.miMemory[SCREENWRITELOCATION + 4 + i] = szAddress[i];
				}
			}
			else
			{
				if (this->pIC64Event)
				{
					pIC64Event->ShowErrorBox(SZAUTOLOADTITLE, errorText);
				}
				autoLoadCommand.CleanUp();
				appStatus->m_bAutoload = FALSE;
				break;
			}
		}
		if (autoLoadCommand.startaddress <= BASICSTARTADDRESS)
		{
			if (period < resettime + 3)	
			{
				cia1.SetKeyMatrixDown(2, 1);//r
			}
			else if (period < resettime + 6)
			{
			}
			else if (period < resettime + 9)	
			{
				cia1.SetKeyMatrixDown(3, 6);//u
			}
			else if (period < resettime + 12)
			{
			}
			else if (period < resettime + 15)	
			{
				cia1.SetKeyMatrixDown(4, 7);//n
			}
			else if (period < resettime + 18)
			{
			}
			else if (period < resettime + 21)
			{
				cia1.SetKeyMatrixDown(0, 1);//return
			}
			else
			{
				autoLoadCommand.CleanUp();
				appStatus->m_bAutoload = FALSE;
			}
		}
		else
		{
			if (period < resettime + 3)	
			{
				cia1.SetKeyMatrixDown(1, 5);//s
			}
			else if (period < resettime + 6)
			{
			}
			else if (period < resettime + 9)
			{
				cia1.SetKeyMatrixDown(0, 1);//return
			}
			else
			{
				autoLoadCommand.CleanUp();
				appStatus->m_bAutoload = FALSE;
			}
		}
		break;
	case C64::AUTOLOAD_SID_FILE:		
		if (autoLoadCommand.sequence == C64::AUTOSEQ_LOAD)
		{
			autoLoadCommand.sequence = C64::AUTOSEQ_RUN;
			if (autoLoadCommand.pSidFile)
			{
				SIDLoader &sl = *autoLoadCommand.pSidFile;
				if (autoLoadCommand.directoryIndex < 0)
					hr = sl.LoadSID(ram.miMemory, &autoLoadCommand.filename[0], true, 0);
				else
					hr = sl.LoadSID(ram.miMemory, &autoLoadCommand.filename[0], false, autoLoadCommand.directoryIndex + 1);
				if (SUCCEEDED(hr))
				{
					if (autoLoadCommand.pSidFile->RSID_BASIC)
					{
						SetBasicProgramEndAddress((WORD)(sl.startSID+sl.lenSID-1));
						autoLoadCommand.type = C64::AUTOLOAD_PRG_FILE;
						autoLoadCommand.sequence = C64::AUTOSEQ_RUN;
						autoLoadCommand.startaddress = BASICSTARTADDRESS;
						break;
					}
					else
					{
						cpu.mPC.word = sl.driverLoadAddress;
						cpu.m_cpu_sequence = C_FETCH_OPCODE;
					}
				}
				else
				{
					//FIXME Desirable not to show messages in the C64 class
					if (this->pIC64Event)
					{
						pIC64Event->ShowErrorBox(SZAUTOLOADTITLE, sl.errorText);
					}
				}
			}
			autoLoadCommand.CleanUp();
			appStatus->m_bAutoload = FALSE;
		}
		break;
	case C64::AUTOLOAD_DISK_FILE:
		if (autoLoadCommand.sequence == C64::AUTOSEQ_LOAD)
		{
			autoLoadCommand.sequence = C64::AUTOSEQ_RUN;
			if (autoLoadCommand.bQuickLoad)
			{
				autoLoadCommand.type = C64::AUTOLOAD_PRG_FILE;
				if (autoLoadCommand.pImageData!=0 && autoLoadCommand.imageSize > 2)
				{
					autoLoadCommand.startaddress = autoLoadCommand.pImageData[0] + autoLoadCommand.pImageData[1] * 0x100;

					loadSize = autoLoadCommand.imageSize - 2;
					if ((bit32)autoLoadCommand.startaddress + (bit32)loadSize -1 > 0xffffL)
					{
						loadSize = 0xffff - autoLoadCommand.startaddress + 1;
					}
					if (loadSize > 0)
					{
						memcpy_s(&ram.miMemory[autoLoadCommand.startaddress], 0x10000 - autoLoadCommand.startaddress, &autoLoadCommand.pImageData[2], loadSize);					
						if (autoLoadCommand.startaddress <= BASICSTARTADDRESS)
							SetBasicProgramEndAddress(autoLoadCommand.startaddress + loadSize - 1);
						else
						{
							CopyMemory(&ram.miMemory[SCREENWRITELOCATION], szSysCall, strlen(szSysCall));
							szAddress[0] = 0;
							sprintf_s(szAddress, _countof(szAddress), "%ld", autoLoadCommand.startaddress);
							unsigned int i;
							for (i=0 ; i < strlen(szAddress) ; i++)
								ram.miMemory[SCREENWRITELOCATION + 4 + i] = szAddress[i];
						}
					}
				}
				else
				{
					autoLoadCommand.startaddress = BASICSTARTADDRESS;
				}
			}
			else
			{
				int filenamelen = C64File::GetC64FileNameLength(autoLoadCommand.c64filename, sizeof(autoLoadCommand.c64filename));
				if (filenamelen == 0)
				{
					memcpy_s(&ram.miMemory[SCREENWRITELOCATION], 40, szLoadDisk, strlen(szLoadDisk));
				}
				else
				{
					bit8 screencodebuffer[sizeof(autoLoadCommand.c64filename)];
					memset(screencodebuffer, 0, sizeof(screencodebuffer));
					for (int i=0 ; i < sizeof(screencodebuffer) ; i++)
					{
						screencodebuffer[i]  = C64File::ConvertPetAsciiToScreenCode(autoLoadCommand.c64filename[i]);
					}
					//ConvertPetAsciiToScreenCode
					memcpy_s(&ram.miMemory[SCREENWRITELOCATION], 40, szLoadDisk, LOADPREFIXLENGTH);
					memcpy_s(&ram.miMemory[SCREENWRITELOCATION + LOADPREFIXLENGTH], C64Directory::D64FILENAMELENGTH, screencodebuffer, filenamelen); 
					memcpy_s(&ram.miMemory[SCREENWRITELOCATION + LOADPREFIXLENGTH + filenamelen], 40, &szLoadDisk[LOADPOSTFIXINDEX], LOADPOSTFIXLENGTH);
				}
			}
		}
		if (period < resettime + 3)
		{
		}
		else if (period < resettime + 6)
		{
			cia1.SetKeyMatrixDown(1, 7);//lshift
		}
		else if (period < resettime + 9)
		{
			cia1.SetKeyMatrixDown(1, 7);//lshift
			cia1.SetKeyMatrixDown(7, 7);//stop
		}
		else
		{
			autoLoadCommand.CleanUp();
			appStatus->m_bAutoload = FALSE;
		}
		break;
	default:
		autoLoadCommand.CleanUp();
		appStatus->m_bAutoload = FALSE;

	}
}

void C64::TapePressPlay()
{
	cpu.SetCassetteSense(0);
	tape64.PressPlay();
	cia1.SetWakeUpClock();
}

void C64::TapePressStop()
{
	cia1.flag_change = !cia1.f_flag_in;
	cia1.f_flag_in=1;
	cpu.SetCassetteSense(1);
	tape64.PressStop();
	cia1.SetWakeUpClock();
}

void C64::TapePressEject()
{
	cia1.flag_change = !cia1.f_flag_in;
	cia1.f_flag_in=1;
	cpu.SetCassetteSense(1);
	tape64.Eject();
	cia1.SetWakeUpClock();
}

void C64::TapePressRewind()
{
	TapePressStop();
	tape64.Rewind();
	cia1.SetWakeUpClock();
}

void C64::Set_DiskProtect(bool bOn)
{
	diskdrive.D64_DiskProtect(bOn);
}

bool C64::Get_DiskProtect()
{
	return diskdrive.m_d64_protectOff == 0;
}

void C64::DiskReset()
{
	diskdrive.Reset(cpu.CurrentClock);
}

void C64::DetachCart()
{
	if (IsCartAttached())
	{
		cart.DetachCart();
		HardReset(true);
	}
}

bool C64::IsCartAttached()
{
	return cart.IsCartAttached();
}

void C64::SetupColorTables(unsigned int d3dFormat)
{
	this->vic.setup_color_tables((D3DFORMAT)d3dFormat);
}

HRESULT C64::UpdateBackBuffer()
{
	return this->vic.UpdateBackBuffer();
}

IMonitor *C64::GetMon()
{
	return &mon;
}

//ITapeEvent
void C64::Pulse(ICLK sysclock)
{
	cia1.Pulse(sysclock);
}
//ITapeEvent
void C64::EndOfTape(ICLK sysclock)
{
	TapePressStop();
}

HRESULT C64::AutoLoad(TCHAR *filename, int directoryIndex, bool bIndexOnlyPrgFiles, const bit8 c64FileName[C64DISKFILENAMELENGTH], bool bQuickLoad, bool bAlignD64Tracks)
{
HRESULT hr = S_OK;
TCHAR *p;
C64File c64file;

	ClearError();
	autoLoadCommand.CleanUp();

	hr = c64file.Init();
	if (FAILED(hr))
		return SetError(hr, TEXT("Could initialise autoload."));

	autoLoadCommand.type = C64::AUTOLOAD_NONE;
	autoLoadCommand.sequence = C64::AUTOSEQ_RESET;
	autoLoadCommand.directoryIndex = directoryIndex;
	autoLoadCommand.bQuickLoad = bQuickLoad;
	autoLoadCommand.bAlignD64Tracks =  bAlignD64Tracks;
	autoLoadCommand.bIndexOnlyPrgFiles =bIndexOnlyPrgFiles;
	if (c64FileName == NULL)
	{
		memset(autoLoadCommand.c64filename, 0xa0, sizeof(autoLoadCommand.c64filename));
		if (directoryIndex >= 0)
		{
			int numItems;
			hr = c64file.LoadDirectory(filename, C64Directory::D64MAXDIRECTORYITEMCOUNT, numItems, bIndexOnlyPrgFiles, NULL);
			if (SUCCEEDED(hr))
				c64file.GetDirectoryItemName(directoryIndex, autoLoadCommand.c64filename, sizeof(autoLoadCommand.c64filename));
		}
	}
	else
		memcpy_s(autoLoadCommand.c64filename, sizeof(autoLoadCommand.c64filename), c64FileName, C64DISKFILENAMELENGTH);
	appStatus->m_bAutoload = FALSE;

	hr = _tcscpy_s(&autoLoadCommand.filename[0], _countof(autoLoadCommand.filename), filename);
	if (FAILED(hr))
		return SetError(hr, TEXT("%s too long."), filename);

	if (lstrlen(filename) >= 4)
	{
		p= &(filename[lstrlen(filename) - 4]);
		if (lstrcmpi(p, TEXT(".crt"))==0)
		{
			hr = LoadCrtFile(filename);
			if (SUCCEEDED(hr))
			{
				cart.AttachCart();
				Reset(0);
				autoLoadCommand.type = C64::AUTOLOAD_NONE;
				appStatus->m_bAutoload = FALSE;
				
			}
			return hr;
		}
		else if (lstrcmpi(p, TEXT(".tap"))==0)
		{
			hr = LoadTAPFile(filename);
			if (SUCCEEDED(hr))
			{
				autoLoadCommand.type = C64::AUTOLOAD_TAP_FILE;
				appStatus->m_bAutoload = TRUE;
			}
		}
		else if (lstrcmpi(p, TEXT(".prg"))==0 || lstrcmpi(p, TEXT(".p00"))==0)
		{
			autoLoadCommand.type = C64::AUTOLOAD_PRG_FILE;
			appStatus->m_bAutoload = TRUE;
		}		
		else if (lstrcmpi(p, TEXT(".t64"))==0)
		{
			autoLoadCommand.type = C64::AUTOLOAD_T64_FILE;
			appStatus->m_bAutoload = TRUE;
		}		
		else if (lstrcmpi(p, TEXT(".d64"))==0 || lstrcmpi(p, TEXT(".g64"))==0 || lstrcmpi(p, TEXT(".fdi"))==0)
		{
			if (!cfg->m_bD1541_Emulation_Enable)
			{
				diskdrive.CurrentPALClock = cpu.CurrentClock;
				cfg->m_bD1541_Emulation_Enable = TRUE;
			}
			
			pIC64Event->SetBusy(true);
			hr = InsertDiskImageFile(filename, bAlignD64Tracks);
			if (SUCCEEDED(hr))
			{
				if (bQuickLoad)
				{
					if (autoLoadCommand.pImageData)
					{
						GlobalFree(autoLoadCommand.pImageData);
						autoLoadCommand.pImageData = 0;
					}
					if (autoLoadCommand.directoryIndex<0)
						hr = c64file.LoadFileImage(filename, NULL, &autoLoadCommand.pImageData, &autoLoadCommand.imageSize);
					else
						hr = c64file.LoadFileImage(filename, autoLoadCommand.c64filename, &autoLoadCommand.pImageData, &autoLoadCommand.imageSize);
					if (FAILED(hr))
						SetError(hr, TEXT("Unable to quick load."));
				}
			}
			pIC64Event->SetBusy(false);
			if (SUCCEEDED(hr))
			{
				autoLoadCommand.type = C64::AUTOLOAD_DISK_FILE;
				appStatus->m_bAutoload = TRUE;
				cart.DetachCart();
				Reset(0);
				return hr;
			}
			else
			{
				autoLoadCommand.CleanUp();
				appStatus->m_bAutoload = FALSE;
				return hr;
			}
		}
		else if (lstrcmpi(p, TEXT(".sid"))==0)
		{
			autoLoadCommand.pSidFile = new SIDLoader();
			if (autoLoadCommand.pSidFile == 0)
				return SetError(E_FAIL, TEXT("Out of memory."));
			hr = autoLoadCommand.pSidFile->LoadSIDFile(filename);
			if (FAILED(hr))
				return SetError(*(autoLoadCommand.pSidFile));
			autoLoadCommand.type = C64::AUTOLOAD_SID_FILE;
			appStatus->m_bAutoload = TRUE;
		}
		else
		{
			return SetError(E_FAIL, TEXT("Unknown file type."));
		}
	}
	else
	{
		return SetError(E_FAIL,TEXT("Unknown file type."));
	}
	cart.DetachCart();
	Reset(0);
	return S_OK;
}

HRESULT C64::LoadImageFile(TCHAR *filename, bit16* pStartAddress, bit16* pSize)
{
HANDLE hfile=0;
BOOL r;
DWORD bytes_read,file_size;
bit32 start,code_size,s;
TCHAR *p;

	ClearError();
	hfile = CreateFile(filename,GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,NULL); 
	if (hfile==INVALID_HANDLE_VALUE)
		return SetError(E_FAIL, TEXT("Could not open %s."), filename);
	file_size = GetFileSize(hfile, 0);
	if (INVALID_FILE_SIZE == file_size)
	{
		CloseHandle(hfile);
		return SetError(E_FAIL,TEXT("Could not open %s."), filename);
	}
	if (file_size > sizeof(ram.tmp_data))
	{
		CloseHandle(hfile);
		return SetError(E_FAIL,TEXT("%s is too large to be a C64 image."), filename);
	}

	r=ReadFile(hfile,&ram.tmp_data[0],file_size,&bytes_read,NULL);
	CloseHandle(hfile);
	if (r==0)
		return SetError(E_FAIL,TEXT("Could not read from %s."),filename);
	if (bytes_read!=file_size)
		return SetError(E_FAIL,TEXT("Could not read from %s."),filename);
	

	if (lstrlen(filename)>=4)
	{
		p= &(filename[lstrlen(filename)-4]);
		if (lstrcmpi(p,TEXT(".p00"))==0)
		{
			start=* ((bit16 *)(&ram.tmp_data[0x1a]));
			code_size=(bit16)file_size - 0x1c;
			s=0x1c;
		}
		else if (lstrcmpi(p,TEXT(".prg"))==0)
		{
			start=* ((bit16 *)(&ram.tmp_data[0x00]));
			code_size=(bit16)file_size - 0x2;
			s=0x2;
		}		
		else
		{
			start=* ((bit16 *)(&ram.tmp_data[0x00]));
			code_size=(bit16)file_size - 0x2;
			s=0x2;
		}
	}
	else
	{
		start=* ((bit16 *)(&ram.tmp_data[0x00]));
		code_size=(bit16)file_size - 0x2;
		s=0x2;
	}

	start &= 0xffff;
	code_size &= 0xffff;
	if ((code_size+start-1)>0xffff)
		code_size = 0x10000 - start;
	memcpy(&ram.mMemory[start],&ram.tmp_data[s], code_size);

	*pStartAddress = (bit16)start;
	*pSize = (bit16)code_size;
	return S_OK;

}

HRESULT C64::LoadT64ImageFile(TCHAR *filename, int t64Index, bit16* pStartAddress, bit16* pSize)
{
HANDLE hfile=0;
bit16 start,code_size;
T64 t64;
HRESULT hr;
	
	ClearError();
	if (t64Index<0)
		return SetError(E_FAIL,TEXT("Could not open the selected directory item for %s."), filename);
	hr = t64.LoadT64Directory(filename, MAXT64LIST);
	if (FAILED(hr))
		return SetError(t64);
	if (t64.t64Header.maxEntries <= t64Index)
		return SetError(E_FAIL,TEXT("Could not open the selected directory item for %s."), filename);

	if (t64.t64Item[t64Index].mySize > 0xffff || t64.t64Item[t64Index].mySize <= 2)
		return SetError(E_FAIL,TEXT("Could not open the selected directory item for %s."), filename);

	start = t64.t64Item[t64Index].startAddress;
	code_size = (bit16)t64.t64Item[t64Index].mySize;
	
	hr = t64.LoadT64File(filename, t64.t64Item[t64Index].offset, code_size);
	if (FAILED(hr))
		return SetError(t64);

	if (start==0)
		start=* ((bit16 *)(&t64.data[0]));

	if (((bit32)code_size + (bit32)start - 1) > 0xffffL)
		code_size = 0xffff - start + 1;
	memcpy(&ram.mMemory[start],&t64.data[0], code_size);

	*pStartAddress = (bit16)start;
	*pSize = (bit16)code_size;
	return S_OK;
}

HRESULT C64::LoadCrtFile(TCHAR *filename)
{
HRESULT hr = E_FAIL;
	ClearError();
	hr = cart.LoadCrtFile(filename);
	if (SUCCEEDED(hr))
	{
		cart.AttachCart();
		this->HardReset(true);
	}
	this->SetError(cart);
	return hr;
}

HRESULT C64::LoadTAPFile(TCHAR *filename)
{
HRESULT hr;

	ClearError();
	hr = tape64.InsertTAPFile(filename);
	if (FAILED(hr))
	{
		return SetError(hr,tape64.errorText);
	}
	return S_OK;
}


void C64::RemoveDisk()
{
	diskdrive.RemoveDisk();
}

HRESULT C64::InsertDiskImageFile(TCHAR *filename, bool bAlignD64Tracks)
{
TCHAR *p;

	ClearError();
	if (lstrlen(filename) < 4)
		return E_FAIL;

	p = &(filename[lstrlen(filename)-4]);
	
	if (lstrcmpi(p, TEXT(".d64"))==0)
		return LoadD64FromFile(filename, bAlignD64Tracks);
	else if (lstrcmpi(p, TEXT(".g64"))==0)
		return LoadG64FromFile(filename);
	else if (lstrcmpi(p, TEXT(".fdi"))==0)
		return LoadFDIFromFile(filename);
	else
		return E_FAIL;
	return S_OK;
}


HRESULT C64::LoadD64FromFile(TCHAR *filename, bool bAlignD64Tracks)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	hr = dsk.LoadD64FromFile(filename, true, bAlignD64Tracks);

	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	diskdrive.WaitThreadReady();
	diskdrive.LoadImageBits(&dsk);

	diskdrive.SetDiskLoaded();
	return hr;
}

HRESULT C64::LoadG64FromFile(TCHAR *filename)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	hr = dsk.LoadG64FromFile(filename, true);

	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	diskdrive.WaitThreadReady();
	diskdrive.LoadImageBits(&dsk);

	diskdrive.SetDiskLoaded();
	return hr;
}

HRESULT C64::LoadFDIFromFile(TCHAR *filename)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	hr = dsk.LoadFDIFromFile(filename);

	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}
	if (hr == APPERR_BAD_CRC)
	{
		SetError(dsk);
	}

	diskdrive.WaitThreadReady();
	diskdrive.LoadImageBits(&dsk);

	diskdrive.SetDiskLoaded();

	return hr;
}

HRESULT C64::InsertNewDiskImage(TCHAR *diskname, bit8 id1, bit8 id2, bool bAlignD64Tracks, int numberOfTracks)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}

	dsk.InsertNewDiskImage(diskname, id1, id2, bAlignD64Tracks, numberOfTracks);

	diskdrive.WaitThreadReady();
	diskdrive.LoadImageBits(&dsk);
	diskdrive.SetDiskLoaded();
	return S_OK;
}

HRESULT C64::SaveD64ToFile(TCHAR *filename, int numberOfTracks)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}
	diskdrive.WaitThreadReady();
	diskdrive.SaveImageBits(&dsk);	
	hr = dsk.SaveD64ToFile(filename, numberOfTracks);
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}
	return hr;	
}

HRESULT C64::SaveFDIToFile(TCHAR *filename)
{
GCRDISK dsk;
HRESULT hr;

	ClearError();
	hr = dsk.Init();
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}
	diskdrive.WaitThreadReady();
	diskdrive.SaveImageBits(&dsk);	
	hr = dsk.SaveFDIToFile(filename);
	if (FAILED(hr))
	{
		SetError(dsk);
		return hr;
	}
	return S_OK;	
}

HRESULT C64::SaveTrackState(bit32 *pTrackBuffer, bit8 *pTrack, int track_size, int *p_gap_count)
{
int i;
int delay = 0;
int gapCount = 0;

	if (p_gap_count)
		*p_gap_count = 0;

	for (i=0; i<DISK_RAW_TRACK_SIZE; i++)
	{
		bit8 k = pTrack[i];
		if (k > 0 && k <= 16)
		{
			k = (k - 1) & 0xf;
			//The first delay value represents the gap from the start of the track to the first pulse.
			delay += k;
			
			pTrackBuffer[gapCount] = delay;
			gapCount++;
		
			delay = (16 - k);
		}
		else
			delay += 16;
	}

	if (gapCount > 0)
	{
		pTrackBuffer[gapCount++] = delay;
		//The last delay value represents the gap between the last pulse and the end of the track.
	}

	if (p_gap_count)
		*p_gap_count = gapCount;

	return S_OK;
}

HRESULT C64::LoadTrackState(const bit32 *pTrackBuffer, bit8 *pTrack, int gap_count)
{
const int MAXTIME = DISK_RAW_TRACK_SIZE * 16;
	if (gap_count < 0)
		return S_OK;
	int i;
	bit32 delay = 0;
	ZeroMemory(pTrack, DISK_RAW_TRACK_SIZE);
	//The last value at pTrackBuffer[gap_count - 1] represents the gap between the last pulse and the end of the track.
	//The number of pulses is equal to gap_count - 1;
	for (i=0; i < gap_count - 1; i++)
	{
		delay += pTrackBuffer[i];
		if (delay > MAXTIME)
			break;
		int q = delay / 16;
		pTrack[q] = (delay % 16) + 1;
	}
	return S_OK;
}

HRESULT C64::SaveC64StateToFile(TCHAR *filename)
{
HRESULT hr;
ULONG bytesWritten;
SsSectionHeader sh;
bit32 *pTrackBuffer = NULL;

	SynchroniseDevicesWithVIC();

	IStream *pfs = NULL;
	do
	{
		hr = FileStream::CreateObject(filename, &pfs, true);
		if (FAILED(hr))
			break;

		SsHeader hdr;
		ZeroMemory(&hdr, sizeof(hdr));
		strcpy(hdr.Signature, SaveState::SIGNATURE);
		strcpy(hdr.EmulatorName, SaveState::NAME);
		hdr.Version = 0;
		hdr.HeaderSize = sizeof(hdr);
		hr = pfs->Write(&hdr, sizeof(hdr), &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::C64Ram;
		sh.size = sizeof(sh) + SaveState::SIZE64K;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->ram.mMemory, SaveState::SIZE64K, &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::C64ColourRam;
		sh.size = sizeof(sh) + SaveState::SIZECOLOURAM;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->ram.mColorRAM, SaveState::SIZECOLOURAM, &bytesWritten);
		if (FAILED(hr))
			break;

		SsCpuMain sbCpuMain;
		this->cpu.GetState(sbCpuMain);
		hr = SaveState::SaveSection(pfs, sbCpuMain, SsLib::SectionType::C64Cpu);
		if (FAILED(hr))
			break;

		SsCia1 sbCia1;
		this->cia1.GetState(sbCia1);
		hr = SaveState::SaveSection(pfs, sbCia1, SsLib::SectionType::C64Cia1);
		if (FAILED(hr))
			break;
		
		SsCia2 sbCia2;
		this->cia2.GetState(sbCia2);
		hr = SaveState::SaveSection(pfs, sbCia2, SsLib::SectionType::C64Cia2);
		if (FAILED(hr))
			break;

		SsVic6569 sbVic6569;
		this->vic.GetState(sbVic6569);
		hr = SaveState::SaveSection(pfs, sbVic6569, SsLib::SectionType::C64Vic);
		if (FAILED(hr))
			break;

		SsSid sbSid;
		this->sid.GetState(sbSid);
		hr = SaveState::SaveSection(pfs, sbSid, SsLib::SectionType::C64Sid);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::C64KernelRom;
		sh.size = sizeof(sh) + SaveState::SIZEC64KERNEL;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->ram.mKernal, SaveState::SIZEC64KERNEL, &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::C64BasicRom;
		sh.size = sizeof(sh) + SaveState::SIZEC64BASIC;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->ram.mBasic, SaveState::SIZEC64BASIC, &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::C64CharRom;
		sh.size = sizeof(sh) + SaveState::SIZEC64CHARGEN;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->ram.mCharGen, SaveState::SIZEC64CHARGEN, &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::DriveRam;
		sh.size = sizeof(sh) + SaveState::SIZEDRIVERAM;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->diskdrive.m_pD1541_ram, SaveState::SIZEDRIVERAM, &bytesWritten);
		if (FAILED(hr))
			break;

		sh.id = SsLib::SectionType::DriveRom;
		sh.size = sizeof(sh) + SaveState::SIZEDRIVEROM;
		sh.version = 0;
		hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
		if (FAILED(hr))
			break;
		hr = pfs->Write(this->diskdrive.m_pD1541_rom, SaveState::SIZEDRIVEROM, &bytesWritten);
		if (FAILED(hr))
			break;

		SsDiskInterface sbDiskInterface;
		this->diskdrive.GetState(sbDiskInterface);
		hr = SaveState::SaveSection(pfs, sbDiskInterface, SsLib::SectionType::DriveController);
		if (FAILED(hr))
			break;

		SsVia1 sbVia1;
		this->diskdrive.via1.GetState(sbVia1);
		hr = SaveState::SaveSection(pfs, sbVia1, SsLib::SectionType::DriveVia1);
		if (FAILED(hr))
			break;

		SsVia2 sbVia2;
		this->diskdrive.via2.GetState(sbVia2);
		hr = SaveState::SaveSection(pfs, sbVia2, SsLib::SectionType::DriveVia2);
		if (FAILED(hr))
			break;

		if (this->diskdrive.m_diskLoaded)
		{
			HuffCompression hw;
			hr = hw.Init();
			if (FAILED(hr))
				break;
			pTrackBuffer = (bit32 *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, (DISK_RAW_TRACK_SIZE+1)*sizeof(bit32));
			if (!pTrackBuffer)
			{
				hr = E_OUTOFMEMORY;
				break;
			}
			LARGE_INTEGER spos_zero;
			LARGE_INTEGER spos_next;
			ULARGE_INTEGER pos_current_section_header;
			ULARGE_INTEGER pos_next_section_header;
			ULARGE_INTEGER pos_current_track_header;
			ULARGE_INTEGER pos_next_track_header;
			ULARGE_INTEGER pos_dummy;
			spos_zero.QuadPart = 0;
			hr = pfs->Seek(spos_zero, STREAM_SEEK_CUR, &pos_current_section_header);
			if (FAILED(hr))
				break;
			sh.id = SsLib::SectionType::DriveDiskImage;
			sh.size = sizeof(sh);
			sh.version = 0;
			hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
			if (FAILED(hr))
				break;
			hr = pfs->Seek(spos_zero, STREAM_SEEK_CUR, &pos_current_track_header);
			if (FAILED(hr))
				break;
			pos_next_section_header = pos_current_track_header;

			for(int i=0; i<G64_MAX_TRACKS; i++)
			{
				SsTrackHeader th;
				th.number = i;
				th.size = sizeof(th);
				th.version = 0;
				th.gap_count = 0;
				hr = pfs->Write(&th, sizeof(th), &bytesWritten);
				if (FAILED(hr))
					break;
				int gap_count = 0;
				bit32 compressed_size = 0;
				bit8 *pTrack = diskdrive.m_rawTrackData[i];
				SaveTrackState(pTrackBuffer, pTrack, DISK_RAW_TRACK_SIZE, &gap_count);
				hr = hw.SetFile(pfs);
				if (FAILED(hr))
					break;
				if (gap_count > 0)
				{
					hr = hw.Compress(pTrackBuffer, gap_count, &compressed_size);
					if (FAILED(hr))
						break;
				}
				hr = pfs->Seek(spos_zero, STREAM_SEEK_CUR, &pos_next_track_header);
				if (FAILED(hr))
					break;

				spos_next.QuadPart = pos_current_track_header.QuadPart;
				hr = pfs->Seek(spos_next, STREAM_SEEK_SET, &pos_dummy);
				if (FAILED(hr))
					break;
				bit32 sectionSize = (bit32)(pos_next_track_header.QuadPart - pos_current_track_header.QuadPart);
				assert(sectionSize == compressed_size+sizeof(SsTrackHeader));
				th.size = sectionSize;
				th.gap_count = gap_count;
				hr = pfs->Write(&th, sizeof(th), &bytesWritten);
				if (FAILED(hr))
					break;
				spos_next.QuadPart = pos_next_track_header.QuadPart;
				hr = pfs->Seek(spos_next, STREAM_SEEK_SET, &pos_dummy);
				if (FAILED(hr))
					break;

				pos_current_track_header = pos_next_track_header;
				pos_next_section_header = pos_next_track_header;
			}
			if (FAILED(hr))
				break;

			spos_next.QuadPart = pos_current_section_header.QuadPart;
			hr = pfs->Seek(spos_next, STREAM_SEEK_SET, &pos_dummy);
			if (FAILED(hr))
				break;
			sh.size = (bit32)(pos_next_section_header.QuadPart - pos_current_section_header.QuadPart);
			hr = pfs->Write(&sh, sizeof(sh), &bytesWritten);
			if (FAILED(hr))
				break;
			spos_next.QuadPart = pos_next_section_header.QuadPart;
			hr = pfs->Seek(spos_next, STREAM_SEEK_SET, &pos_dummy);
			if (FAILED(hr))
				break;
		}

	} while (false);
	if (pfs)
	{
		pfs->Release();
		pfs = NULL;
	}
	if (pTrackBuffer)
	{
		GlobalFree(pTrackBuffer);
		pTrackBuffer = NULL;
	}
	return hr;
}

HRESULT C64::LoadC64StateFromFile(TCHAR *filename)
{
HRESULT hr;
ULONG bytesWritten;
SsSectionHeader sh;
STATSTG stat;
LARGE_INTEGER pos_next;
ULARGE_INTEGER pos_out;
SsCpuMain sbCpuMain;
SsCia1 sbCia1;
SsCia2 sbCia2;
SsVic6569 sbVic6569;
SsSid sbSid;
SsDiskInterface sbDriveController;
SsVia1 sbDriveVia1;
SsVia2 sbDriveVia2;
bit8 *pC64Ram = NULL;
bit8 *pC64ColourRam = NULL;
bit8 *pC64KernelRom = NULL;
bit8 *pC64BasicRom = NULL;
bit8 *pC64CharRom = NULL;
bit8 *pDriveRam = NULL;
bit8 *pDriveRom = NULL;
bool hasC64 = false;
bool done = false;
bool eof = false;
HuffDecompression hw;
bit32 *pTrackBuffer = NULL;
bit8 *pTrack = NULL;
bool bC64Cpu = false;
bool bC64Ram = false;
bool bC64ColourRam = false;
bool bC64Cia1 = false;
bool bC64Cia2 = false;
bool bC64Vic6569 = false;
bool bC64Sid = false;
bool bC64KernelRom = false;
bool bC64BasicRom = false;
bool bC64CharRom = false;
bool bTapePlayer = false;
bool bTapeData = false;
bool bDriveController = false;
bool bDriveVia1 = false;
bool bDriveVia2 = false;
bool bDriveData = false;
bool bDriveRam = false;
bool bDriveRom = false;
bool bDriveDiskData = false;
const ICLK MAXDIFF = PAL_CLOCKS_PER_FRAME;

	diskdrive.WaitThreadReady();

	IStream *pfs = NULL;
	do
	{
		hr = FileStream::CreateObject(filename, &pfs, false);
		if (FAILED(hr))
			break;

		ZeroMemory(&stat, sizeof(stat));

		pfs->Stat(&stat, STATFLAG_NONAME);
		if (FAILED(hr))
			break;

		SsHeader hdr;
		ZeroMemory(&hdr, sizeof(hdr));
		hr = pfs->Read(&hdr, sizeof(hdr), &bytesWritten);
		if (FAILED(hr))
			break;

		if (strcmp(hdr.Signature, SaveState::SIGNATURE) != 0)
		{
			hr = E_FAIL;
			break;
		}

		pos_next.QuadPart = 0;
		hr = pfs->Seek(pos_next, STREAM_SEEK_CUR, &pos_out);
		if (FAILED(hr))
			break;
		pos_next.QuadPart = pos_out.QuadPart;
		
		SsTrackHeader trackHeader;
		while (!eof && !done)
		{
			if ((ULONGLONG)pos_next.QuadPart + sizeof(sh) >= stat.cbSize.QuadPart)
			{
				eof = true;
				break;
			}
			hr = pfs->Seek(pos_next, STREAM_SEEK_SET, &pos_out);
			if (FAILED(hr))
				break;
			hr = pfs->Read(&sh, sizeof(sh), &bytesWritten);
			if (FAILED(hr))
				break;
			if (sh.size == 0)
			{
				eof = true;
				break;
			}
			pos_next.QuadPart = pos_next.QuadPart + sh.size;
			switch(sh.id)
			{
			case SsLib::SectionType::C64Cpu:
				hr = pfs->Read(&sbCpuMain, sizeof(sbCpuMain), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bC64Cpu = true;
				}
				break;
			case SsLib::SectionType::C64Ram:
				pC64Ram = (bit8 *)malloc(SaveState::SIZE64K);
				if (pC64Ram)
				{
					hr = pfs->Read(pC64Ram, SaveState::SIZE64K, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bC64Ram = true;
				break;
			case SsLib::SectionType::C64ColourRam:
				pC64ColourRam = (bit8 *)malloc(SaveState::SIZECOLOURAM);
				if (pC64ColourRam)
				{
					hr = pfs->Read(pC64ColourRam, SaveState::SIZECOLOURAM, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bC64ColourRam = true;
				break;
			case SsLib::SectionType::C64Cia1:
				hr = pfs->Read(&sbCia1, sizeof(sbCia1), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bC64Cia1 = true;
				}
				break;
			case SsLib::SectionType::C64Cia2:
				hr = pfs->Read(&sbCia2, sizeof(sbCia2), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bC64Cia2 = true;
				}
				break;
			case SsLib::SectionType::C64Vic:
				hr = pfs->Read(&sbVic6569, sizeof(sbVic6569), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bC64Vic6569 = true;
				}
				break;
			case SsLib::SectionType::C64Sid:
				hr = pfs->Read(&sbSid, sizeof(sbSid), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bC64Sid = true;
				}
				break;
			case SsLib::SectionType::C64KernelRom:
				pC64KernelRom = (bit8 *)malloc(SaveState::SIZEC64KERNEL);
				if (pC64KernelRom)
				{
					hr = pfs->Read(pC64KernelRom, SaveState::SIZEC64KERNEL, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bC64KernelRom = true;
				break;
			case SsLib::SectionType::C64BasicRom:
				pC64BasicRom = (bit8 *)malloc(SaveState::SIZEC64BASIC);
				if (pC64BasicRom)
				{
					hr = pfs->Read(pC64BasicRom, SaveState::SIZEC64BASIC, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bC64BasicRom = true;
				break;
			case SsLib::SectionType::C64CharRom:
				pC64CharRom = (bit8 *)malloc(SaveState::SIZEC64CHARGEN);
				if (pC64CharRom)
				{
					hr = pfs->Read(pC64CharRom, SaveState::SIZEC64CHARGEN, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bC64CharRom = true;
				break;
			case SsLib::SectionType::DriveRam:
				pDriveRam = (bit8 *)malloc(SaveState::SIZEDRIVERAM);
				if (pDriveRam)
				{
					hr = pfs->Read(pDriveRam, SaveState::SIZEDRIVERAM, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bDriveRam = true;
				break;
			case SsLib::SectionType::DriveRom:
				pDriveRom = (bit8 *)malloc(SaveState::SIZEDRIVEROM);
				if (pDriveRom)
				{
					hr = pfs->Read(pDriveRom, SaveState::SIZEDRIVEROM, &bytesWritten);
				}
				else
				{
					hr = E_OUTOFMEMORY;
				}
				bDriveRom = true;
				break;
			case SsLib::SectionType::DriveController:
				hr = pfs->Read(&sbDriveController, sizeof(sbDriveController), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bDriveController = true;
				}
				break;
			case SsLib::SectionType::DriveVia1:
				hr = pfs->Read(&sbDriveVia1, sizeof(sbDriveVia1), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bDriveVia1 = true;
				}
				break;
			case SsLib::SectionType::DriveVia2:
				hr = pfs->Read(&sbDriveVia2, sizeof(sbDriveVia2), &bytesWritten);
				if (SUCCEEDED(hr))
				{
					bDriveVia2 = true;
				}
				break;
			case SsLib::SectionType::DriveDiskImage:
				if (!pTrackBuffer)
				{
					pTrackBuffer = (bit32 *)GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, (DISK_RAW_TRACK_SIZE+1)*sizeof(bit32));
					if (!pTrackBuffer)
					{
						hr = E_OUTOFMEMORY;
						break;
					}
				}
				hr = pfs->Read(&trackHeader, sizeof(trackHeader), &bytesWritten);
				if (FAILED(hr))
					break;
				hr = hw.SetFile(pfs);
				if (FAILED(hr))
					break;
				if (trackHeader.gap_count > DISK_RAW_TRACK_SIZE)
				{
					hr = E_FAIL;
					break;
				}
				if (trackHeader.gap_count > 0 && trackHeader.number >=0 && trackHeader.number < G64_MAX_TRACKS)
				{
					pTrack = diskdrive.m_rawTrackData[trackHeader.number];
					if (pTrack)
					{
						hr = hw.Decompress(trackHeader.gap_count, &pTrackBuffer);
						if (FAILED(hr))
							break;
						this->LoadTrackState(pTrackBuffer, pTrack, trackHeader.gap_count);
					}
				}
				break;
			}
			if (FAILED(hr))
				break;

			if (bC64Cpu && bC64Ram && bC64ColourRam && bC64Cia1 && bC64Cia2 && bC64Vic6569 && bC64Sid)
			{
				hasC64 = true;
				hr = S_OK;
			}
		}
		if (FAILED(hr))
			break;
	} while (false);
	if (pfs)
	{
		pfs->Release();
		pfs = NULL;
	}
	if (hasC64)
	{
		cpu.SetState(sbCpuMain);
		if (ram.mMemory && pC64Ram)
			memcpy(ram.mMemory, pC64Ram, SaveState::SIZE64K);
		if (ram.mColorRAM && pC64ColourRam)
			memcpy(ram.mColorRAM, pC64ColourRam, SaveState::SIZECOLOURAM);
		cia1.SetState(sbCia1);
		cia2.SetState(sbCia2);
		vic.SetState(sbVic6569);
		sid.SetState(sbSid);

		if (bDriveController)
		{
			diskdrive.SetState(sbDriveController);
		}
		if (bDriveVia1)
		{
			diskdrive.via1.SetState(sbDriveVia1);
		}
		if (bDriveVia2)
		{
			diskdrive.via2.SetState(sbDriveVia2);
		}

		if (pC64KernelRom && ram.mKernal)
		{
			memcpy(ram.mKernal, pC64KernelRom, SaveState::SIZEC64KERNEL);
		}
		if (pC64BasicRom && ram.mBasic)
		{
			memcpy(ram.mBasic, pC64BasicRom, SaveState::SIZEC64BASIC);
		}
		if (pC64CharRom && ram.mCharGen)
		{
			memcpy(ram.mCharGen, pC64CharRom, SaveState::SIZEC64CHARGEN);
		}

		if (pDriveRam && diskdrive.m_pD1541_ram)
		{
			memcpy(diskdrive.m_pD1541_ram, pDriveRam, SaveState::SIZEDRIVERAM);
		}
		if (pDriveRom && diskdrive.m_pD1541_rom)
		{
			memcpy(diskdrive.m_pD1541_rom, pDriveRom, SaveState::SIZEDRIVEROM);
		}
		ICLK c = sbCpuMain.common.CurrentClock;
		cia1.SetCurrentClock(c);
		cia2.SetCurrentClock(c);
		vic.SetCurrentClock(c);
		sid.SetCurrentClock(c);
		cart.SetCurrentClock(c);
		diskdrive.SetCurrentClock(c);
		
		cpu.ConfigureMemoryMap();

		hr = S_OK;
	}
	else
	{
		if (SUCCEEDED(hr))
			hr = E_FAIL;
	}
	if (pTrackBuffer)
	{
		GlobalFree(pTrackBuffer);
		pTrackBuffer = NULL;
	}
	if (pC64Ram)
	{
		free(pC64Ram);
		pC64Ram = NULL;
	}
	if (pC64ColourRam)
	{
		free(pC64ColourRam);
		pC64ColourRam = NULL;
	}
	if (pC64KernelRom)
	{
		free(pC64KernelRom);
		pC64KernelRom = NULL;
	}
	if (pC64BasicRom)
	{
		free(pC64BasicRom);
		pC64BasicRom = NULL;
	}
	if (pC64CharRom)
	{
		free(pC64CharRom);
		pC64CharRom = NULL;
	}
	if (pDriveRam)
	{
		free(pDriveRam);
		pDriveRam = NULL;
	}
	if (pDriveRom)
	{
		free(pDriveRom);
		pDriveRom = NULL;
	}
	return hr;
}

void C64::SoftReset(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	if (!appStatus->m_bDebug)
	{
		ExecuteRandomClocks();
	}
	ICLK sysclock = cpu.CurrentClock;
	cpu.InitReset(sysclock);

	//The cpu reset must be called before the cart reset to allow the cart to assert interrupts if any.
	cpu.Reset(sysclock);
	cart.Reset(sysclock);
}

void C64::HardReset(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	Reset(0);
}

void C64::CartFreeze(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	if (!appStatus->m_bDebug)
	{
		ExecuteRandomClocks();
	}

	cart.CartFreeze();
}

void C64::CartReset(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	if (!appStatus->m_bDebug)
	{
		ExecuteRandomClocks();
	}
	cart.CartReset();
}

void C64::PostSoftReset(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	bPendingSystemCommand = true;
	m_SystemCommand = C64CMD_SOFTRESET;
}

void C64::PostHardReset(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	bPendingSystemCommand = true;
	m_SystemCommand = C64CMD_HARDRESET;
}

void C64::PostCartFreeze(bool bCancelAutoload)
{
	if (bCancelAutoload)
		appStatus->m_bAutoload = 0;
	bPendingSystemCommand = true;
	m_SystemCommand = C64CMD_CARTFREEZE;
}

void C64::ProcessReset()
{
	bPendingSystemCommand = false;
	switch(m_SystemCommand)
	{
	case C64CMD_HARDRESET:
		HardReset(false);
		break;
	case C64CMD_SOFTRESET:
		SoftReset(false);
		break;
	case C64CMD_CARTFREEZE:
		CartFreeze(false);
		break;
	case C64CMD_CARTRESET:
		CartReset(false);
		break;
	}
}

void C64::ExecuteRandomClocks()
{
	SynchroniseDevicesWithVIC();
	int randomclocks = (int)floor((double)rand() / (double)RAND_MAX * ((double)PAL50CLOCKSPERSECOND / (double)PAL50FRAMESPERSECOND));
	while (randomclocks-- > 0)
		this->ExecuteC64Clock();
}

IMonitorCpu *C64::GetCpu(int cpuid)
{
	if (cpuid == CPUID_MAIN)
		return &cpu;
	else if (cpuid == CPUID_DISK)
		return &diskdrive.cpu;
	else
		return NULL;
}

DefaultCpu::DefaultCpu(int cpuid, IC64 *c64)
{
	this->cpuid = cpuid;
	this->c64 = c64;
}

int DefaultCpu::GetCpuId()
{
	return cpuid;
}

IMonitorCpu *DefaultCpu::GetCpu()
{
	if (cpuid == CPUID_MAIN)
		return c64->GetMon()->GetMainCpu();
	else if (cpuid == CPUID_DISK)
		return c64->GetMon()->GetDiskCpu();
	else
		return NULL;
}

