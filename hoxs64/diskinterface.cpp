#include <windows.h>
#include <tchar.h>
#include "dx_version.h"
#include <d3d9.h>
#include <d3dx9core.h>
#include <dinput.h>
#include <dsound.h>
#include <stdio.h>
#include <assert.h>
#include "defines.h"
#include "CDPI.h"
#include "bits.h"
#include "util.h"
#include "utils.h"
#include "mlist.h"
#include "carray.h"
#include "register.h"
#include "errormsg.h"
#include "hconfig.h"
#include "appstatus.h"
#include "dxstuff9.h"
#include "register.h"
#include "c6502.h"
#include "ram64.h"
#include "cpu6510.h"
#include "cia6526.h"
#include "cia1.h"
#include "cia2.h"
#include "vic6569.h"
#include "tap.h"
#include "filter.h"
#include "sid.h"
#include "sidfile.h"
#include "d64.h"
#include "d1541.h"
#include "via6522.h"
#include "via1.h"
#include "via2.h"
#include "t64.h"
#include "tap.h"
#include "diskinterface.h"
#include "C64.h"

#undef DEBUG_DISKPULSES

DiskInterface::DiskInterface()
{
int i;
	for (i=0 ; i < G64_MAX_TRACKS ; i++)
	{
		m_rawTrackData[i] = 0;
	}

	m_currentHeadIndex=0;
	m_currentTrackNumber = 0;
	m_lastHeadStepDir = 0;
	m_lastHeadStepPosition = 0;
	m_shifterWriter = 0;
	m_shifterReader = 0;
	m_frameCounter = 0;
	m_debugFrameCounter = 0;
	m_clockDivider1 = 0;
	m_clockDivider2 = 0;
	m_bDiskMotorOn = false;
	m_bDriveWriteWasOn = false;
	m_bDriveLedOn = false;
	m_driveWriteChangeClock = 0;
	m_writeStream = 0;
	m_diskLoaded = 0;
	m_extraClock = 0;
	m_lastOne = 0;
	m_lastGap = 0;

	m_d64_soe_enable = 1;
	m_d64_write_enable = 0;
	m_d64_serialbus = 0;
	m_d64_dipswitch = 0;
	m_d64_sync = 0;
	m_d64_forcesync = 0;
	m_d64_diskwritebyte = 0;
	mi_d64_diskchange = 0;
	m_diskChangeCounter = 0;

	m_c64_serialbus_diskview = 0;
	m_c64_serialbus_diskview_next = 0;
	m_changing_c64_serialbus_diskview_diskclock = 0;

	m_pD1541_ram = NULL;
	m_pD1541_rom = NULL;
	m_pIndexedD1541_rom = NULL;
	CurrentPALClock = 0;
	CurrentClock = 0;

	m_d64TrackCount = 0;

	m_motorOffClock = 0;
	m_headStepClock = 0;

	mThreadId = 0;
	mhThread = 0;
	mbDiskThreadCommandQuit = 0;
	mevtDiskClocksDone = 0;
	mevtDiskCommand = 0;
	mbDiskThreadCommandQuit = false;
	mbDiskThreadHasQuit = false;
}

DiskInterface::~DiskInterface()
{
	Cleanup();
}

void DiskInterface::Reset(ICLK sysclock)
{
	WaitThreadReady();

	CurrentClock = sysclock;
	CurrentPALClock = sysclock;
	m_pendingclocks = 0;
	//CurrentPALClock must be set before calling ThreadSignalCommandResetClock
	ThreadSignalCommandResetClock();
	WaitThreadReady();

	m_diskd64clk_xf = -Disk64clk_dy2 / 2;

	m_currentHeadIndex = 0;
	m_shifterWriter = 0;
	m_shifterReader = 0;
	m_frameCounter = 0;
	m_debugFrameCounter = 0;
	m_clockDivider1 = 0;
	m_clockDivider2 = 0;
	m_bDiskMotorOn = false;
	m_bDriveWriteWasOn = false;
	m_bDriveLedOn = false;
	m_driveWriteChangeClock = sysclock;
	m_writeStream = 0;
	m_extraClock = 0;
	m_lastOne = 0;
	m_lastGap = 0;

	m_d64_soe_enable = 1;
	m_d64_write_enable = 0;
	m_d64_serialbus = 0;
	m_d64_dipswitch = 0;
	m_d64_sync = 0;
	m_d64_forcesync = 0;
	m_d64_diskwritebyte = 0;
	mi_d64_diskchange = 0;
	m_diskChangeCounter = 0;
	m_changing_c64_serialbus_diskview_diskclock = sysclock;

	m_motorOffClock = 0;
	m_headStepClock = 0;

	SetRamPattern();

	m_currentTrackNumber = 0x2;
	m_lastHeadStepDir = 0;
	m_lastHeadStepPosition = 2;

	via1.Reset(sysclock);
	via2.Reset(sysclock);
	cpu.Reset(sysclock);

}

void DiskInterface::SetRamPattern()
{
int i;
	for (i=0 ; i<=0x07ff ; i++)
	{
		if ((i & 64)==0)
			m_pD1541_ram[i] = 0;
		else
			m_pD1541_ram[i] = 255;
	}
}

HRESULT DiskInterface::Init(CConfig *cfg, CAppStatus *appStatus, IC64Event *pIC64Event,TCHAR *szAppDirectory)
{
int i;
HANDLE hfile=0;
BOOL r;
DWORD bytes_read;
TCHAR szRomPath[MAX_PATH+1];
HRESULT hr;

	ClearError();
	Cleanup();

	hr = InitDiskThread();
	if (FAILED(hr))
	{
		Cleanup();
		return SetError(hr, TEXT("InitDiskThread failed"));
	}

	if (szAppDirectory==NULL)
		m_szAppDirectory[0] = 0;
	else
		_tcscpy_s(m_szAppDirectory, _countof(m_szAppDirectory), szAppDirectory);

	this->cfg = cfg;
	this->appStatus = appStatus;
	this->pIC64Event = pIC64Event;

	m_pD1541_ram=(bit8 *) GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT,0x0800);
	if (m_pD1541_ram==NULL)
	{
		Cleanup();
		return SetError(E_OUTOFMEMORY, TEXT("Memory allocation failed"));
	}
	m_pD1541_rom=(bit8 *) GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT,0x4000);
	if (m_pD1541_ram==NULL)
	{
		Cleanup();
		return SetError(E_OUTOFMEMORY, TEXT("Memory allocation failed"));
	}

	hfile=INVALID_HANDLE_VALUE;
	szRomPath[0]=0;
	if (_tmakepath_s(szRomPath, _countof(szRomPath), 0, m_szAppDirectory, TEXT("C1541.rom"), 0) == 0)
		hfile=CreateFile(szRomPath,GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,NULL); 
	if (hfile==INVALID_HANDLE_VALUE)
		hfile=CreateFile(TEXT("C1541.rom"),GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,NULL); 
	if (hfile==INVALID_HANDLE_VALUE)
	{
		Cleanup();
		return SetError(E_FAIL, TEXT("Could not open C1541.rom"));
	}
	r=ReadFile(hfile,m_pD1541_rom,0x4000,&bytes_read,NULL);
	CloseHandle(hfile);
	if (r==0)
	{
		Cleanup();
		return SetError(E_FAIL, TEXT("Could not read from C1541.rom"));
	}
	if (bytes_read!=0x4000)
	{
		Cleanup();
		return SetError(E_FAIL, TEXT("Could not read 0x4000 bytes from C1541.rom"));
	}

	m_pIndexedD1541_rom = m_pD1541_rom-0xC000;

	for (i=0 ; i < G64_MAX_TRACKS ; i++)
	{
		m_rawTrackData[i] = (bit8 *) GlobalAlloc(GMEM_FIXED | GMEM_ZEROINIT, DISK_RAW_TRACK_SIZE);
		if (m_rawTrackData[i] == 0)
		{
			Cleanup();
			return SetError(E_OUTOFMEMORY, TEXT("Could not read 0x4000 bytes from C1541.rom"));
		}
	}
	
	via1.Init(cfg, appStatus, &cpu, this);
	via2.Init(cfg, appStatus, &cpu, this);
	cpu.Init(pIC64Event, CPUID_DISK, &via1, &via2, this, m_pD1541_ram, m_pIndexedD1541_rom);

	m_d64_protectOff=1;//1=no protect;0=protected;
	return S_OK;
}

typedef BOOL (WINAPI *LPInitializeCriticalSectionAndSpinCount)(LPCRITICAL_SECTION riticalSection, DWORD dwSpinCount );

HRESULT DiskInterface::InitDiskThread()
{

LPInitializeCriticalSectionAndSpinCount pInitializeCriticalSectionAndSpinCount = NULL;
	CloseDiskThread();

	mbDiskThreadCommandQuit = false;
	mbDiskThreadHasQuit = false;

	mevtDiskClocksDone = CreateEvent(0, TRUE, FALSE, NULL);
	if (mevtDiskClocksDone == 0)
	{
		return E_FAIL;
	}
	mevtDiskCommand = CreateEvent(0, TRUE, FALSE, NULL);
	if (mevtDiskCommand == 0)
	{
		return E_FAIL;
	}
	else
	{
		if (G::IsWinVerSupportInitializeCriticalSectionAndSpinCount())
		{
			HMODULE hKernel32 = GetModuleHandle(TEXT("KERNEL32"));
			if (hKernel32) 
			{
				pInitializeCriticalSectionAndSpinCount = (LPInitializeCriticalSectionAndSpinCount) (GetProcAddress(hKernel32, "InitializeCriticalSectionAndSpinCount") );
			}
		}

		if (pInitializeCriticalSectionAndSpinCount!=0)
		{
			pInitializeCriticalSectionAndSpinCount(&mcrtDisk, 0x4000);
		}
		else
		{
			InitializeCriticalSection(&mcrtDisk);
		}
	}

	mhThread = CreateThread(NULL, 0, DiskInterface::DiskThreadProc, this, 0, &mThreadId);
	if (mhThread == 0)
	{
		return E_FAIL;
	}

	WaitThreadReady();
	
	return S_OK;
}

void DiskInterface::WaitThreadReady()
{
	while (1)
	{
		DWORD r;
		r = WaitForSingleObject(mevtDiskClocksDone, 0);
		if (r == WAIT_OBJECT_0)
			return ;
		else if (r == WAIT_TIMEOUT)
			continue;
		else
			return ;
	}
}

void DiskInterface::ThreadSignalCommandResetClock()
{
	EnterCriticalSection(&mcrtDisk);
	if (!mbDiskThreadHasQuit)
	{
		mbDiskThreadCommandResetClock = TRUE;
		ResetEvent(mevtDiskClocksDone);
		SetEvent(mevtDiskCommand);
	}
	LeaveCriticalSection(&mcrtDisk);	
}

void DiskInterface::ThreadSignalCommandExecuteClock(ICLK PalClock)
{
	EnterCriticalSection(&mcrtDisk);
	if (!mbDiskThreadHasQuit)
	{
		m_DiskThreadCommandedPALClock = PalClock;
		ResetEvent(mevtDiskClocksDone);
		SetEvent(mevtDiskCommand);
	}
	LeaveCriticalSection(&mcrtDisk);	
}

void DiskInterface::ThreadSignalCommandClose()
{
	EnterCriticalSection(&mcrtDisk);
	if (!mbDiskThreadHasQuit)
	{
		mbDiskThreadCommandQuit = true;
		ResetEvent(mevtDiskClocksDone);
		SetEvent(mevtDiskCommand);
	}
	LeaveCriticalSection(&mcrtDisk);
}

void DiskInterface::CloseDiskThread()
{
	if (mhThread)
	{
		WaitThreadReady();
		ThreadSignalCommandClose();

		WaitForMultipleObjects(1, &mhThread, TRUE, INFINITE);
		CloseHandle(mhThread);
		
		mhThread = 0;
	}

	if (mevtDiskClocksDone)
	{
		CloseHandle(mevtDiskClocksDone);
		mevtDiskClocksDone = 0;
	}
	
	if (mevtDiskCommand)
	{
		CloseHandle(mevtDiskCommand);
		DeleteCriticalSection(&mcrtDisk);
		mevtDiskCommand = 0;
	}
}

DWORD WINAPI  DiskInterface::DiskThreadProc( LPVOID lpParam )
{
DiskInterface *pDisk;
DWORD r;

	pDisk = (DiskInterface *)lpParam;
	r = pDisk->DiskThreadProc();
	SetEvent(pDisk->mevtDiskClocksDone);
	pDisk->mbDiskThreadHasQuit = true;
	return r;
}

DWORD DiskInterface::DiskThreadProc()
{
DWORD r;
ICLK clk;
bool bQuit;
bool bCommand;

	r = WAIT_OBJECT_0;
	bCommand = false;
	bQuit = false;
	SetEvent(mevtDiskClocksDone);
	while (!bQuit)
	{
		r = WaitForSingleObject(mevtDiskCommand, INFINITE);
		if ( r == WAIT_OBJECT_0)
		{
			while (!bQuit)
			{
				EnterCriticalSection(&mcrtDisk);

				if (mbDiskThreadCommandQuit)
				{
					bQuit = true;
					ResetEvent(mevtDiskCommand);
					SetEvent(mevtDiskClocksDone);
					LeaveCriticalSection(&mcrtDisk);
					break;
				}

				if (mbDiskThreadCommandResetClock)
				{
					mbDiskThreadCommandResetClock = false;
					m_DiskThreadCommandedPALClock = CurrentPALClock;
					m_DiskThreadCurrentPALClock = CurrentPALClock; 
				}


				clk = m_DiskThreadCommandedPALClock;
				m_DiskThreadCurrentPALClock = CurrentPALClock;

				if ((ICLKS)(m_DiskThreadCurrentPALClock - clk) >= 0)
				{
					ResetEvent(mevtDiskCommand);
					SetEvent(mevtDiskClocksDone);
					LeaveCriticalSection(&mcrtDisk);
					break;
				}
				LeaveCriticalSection(&mcrtDisk);

				ExecuteAllPendingDiskCpuClocks();
				ExecutePALClock(clk);
			}
		}
		else if (r == WAIT_ABANDONED)
		{			
			return 0;
		}
		else if (r == WAIT_TIMEOUT)
		{
			continue;
		}
		else //WAIT_FAILED
		{
			return 1;
		}
	}
	return 0;
}

void DiskInterface::Cleanup()
{
int i;

	CloseDiskThread();

	for (i=0 ; i < G64_MAX_TRACKS ; i++)
	{
		if (m_rawTrackData[i])
			GlobalFree(m_rawTrackData[i]);

		m_rawTrackData[i] = 0;
	}

	if (m_pD1541_ram)
	{
		GlobalFree(m_pD1541_ram);
		m_pD1541_ram=NULL;
	}
	if (m_pD1541_rom)
	{
		GlobalFree(m_pD1541_rom);
		m_pD1541_rom=NULL;
	}
	m_pIndexedD1541_rom=NULL;
}

void DiskInterface::D64_DiskProtect(BOOL bOn)
{
	if (bOn)
	{
		m_d64_protectOff = 0;
	}
	else
	{
		m_d64_protectOff = 1;
	}
}

bit8 DiskInterface::GetProtectSensorState()
{
	if (mi_d64_diskchange == 0)
	{
		if (m_diskLoaded)
			return m_d64_protectOff;
		else
			return 1;
	}
	else 
	{
		if (mi_d64_diskchange>DISKCHANGE2)
			return 0;
		else if (mi_d64_diskchange>DISKCHANGE1)
			return 1;
		else
			return 0;
	}
}

void DiskInterface::D64_serial_write(bit8 c64_serialbus)
{
	m_c64_serialbus_diskview = c64_serialbus;
	D64_Attention_Change();
}

void DiskInterface::D64_Attention_Change()
{
bit8 t,autoATN,portOut;
bit8 ATN;

	portOut = via1.PortBOutput();

	ATN = (~m_c64_serialbus_diskview <<2) & portOut & 0x80;


	autoATN = portOut & 0x10;
	t=~portOut;
	if (autoATN)//auto attention
	{
		m_d64_serialbus = ((t << 6) & (~m_c64_serialbus_diskview << 2) ) & 0x80  //DATA OUT
			|((t & 0x8)<< 3);//CLOCK OUT
	}
	else
	{
		m_d64_serialbus = ((t << 6) & (m_c64_serialbus_diskview << 2)) & 0x80  //DATA OUT
			|((t & 0x8)<< 3);//CLOCK OUT
	}

	via1.SetCA1Input(ATN!=0);
}


void DiskInterface::MoveHead(bit8 trackNo)
{
	m_currentTrackNumber = trackNo;
}

bool DiskInterface::StepHeadIn()
{
  	if (m_currentTrackNumber < (G64_MAX_TRACKS-1))
	{
		m_currentTrackNumber++;
		m_lastHeadStepDir = 1;
		m_lastHeadStepPosition = (m_lastHeadStepPosition + 1) & 3;
		m_headStepClock = 5000;
		return true;
	}
	else
	{
		m_lastHeadStepDir = -1;
		return false;
	}
}

bool DiskInterface::StepHeadOut()
{
	if (m_currentTrackNumber > 0)
	{
		m_currentTrackNumber--;
		m_lastHeadStepDir = -1;
		m_lastHeadStepPosition = (m_lastHeadStepPosition - 1) & 3;
		m_headStepClock = 5000;
		return true;
	}
	else
	{
		m_lastHeadStepDir = 1;
		return false;
	}
}

void DiskInterface::StepHeadAuto()
{
	if (m_lastHeadStepDir == 1)
	{
		if (m_lastHeadStepPosition == 1 || m_lastHeadStepPosition == 3)
		{
			if (StepHeadOut())
				StepHeadOut();
		}
	}
	else if (m_lastHeadStepDir == -1)
	{
		if (m_lastHeadStepPosition == 0 || m_lastHeadStepPosition == 2)
		{
			if (StepHeadIn())
				StepHeadIn();
		}
	}
}

void DiskInterface::ClockDividerAdd(bit8 clocks, bit8 speed)
{
bit8 prevClockDivider1;
bit8 prevClockDivider2;
bit8 byteReady;
bit8 writeClock;
//Works for hostage
static const bit16 WEAKBITDIVIDER = 781;
static const bit32 WEAKBITLIMIT = 1280;
bool bQB_rising;

	if (clocks==0)
		return;
	prevClockDivider1 = m_clockDivider1;
	m_lastOne +=clocks;
	m_extraClock+=clocks;

	//Works for hostage
	if (m_extraClock > WEAKBITDIVIDER)
	{
		m_extraClock = m_extraClock - WEAKBITDIVIDER;
		if (m_lastOne > WEAKBITLIMIT && m_d64_write_enable == 0)
		{
			m_clockDivider1 = speed;
			m_clockDivider2 = 0;			
		}
	}

	m_writeStream = 0;
	m_clockDivider1 += clocks;

	if (m_clockDivider1 >= 16)
	{
		//About writeClock
		//The write position is not aligned with the read position. 
		//This is a quick fix that gives a consistent 16Mhz positioned write clock with the assumption that QB can only
		//ever rise once during this function call. The function during write mode is always called at the start of a 
		//16Mhz disk band arc.
		//The function argument "clocks" will always have the value 16 during write mode.
		writeClock = 31 - m_clockDivider1;
loop:
		m_clockDivider1 = m_clockDivider1 + speed  - 16;


		prevClockDivider2 = m_clockDivider2;

		m_clockDivider2 =  (m_clockDivider2 + 1) & 0xf;
		
		bQB_rising =  ((m_clockDivider2 & 2) != 0 && (prevClockDivider2 & 2) == 0);

		if (bQB_rising)
		{
			//rising QB

			if ((signed char)m_shifterWriter < 0)
			{//writing a 1
				m_writeStream = writeClock + 1;
			}
			m_shifterWriter <<= 1;

			m_shifterReader <<= 1;
			if ((m_clockDivider2 & 0xc) == 0)
			{//reading a 1
				if (m_headStepClock==0)
				{
					m_shifterReader |= 1;
				}
			}
			if (!m_d64_sync)
			{
				m_frameCounter =  (m_frameCounter + 1) & 0xf;
			}

			m_debugFrameCounter = (m_debugFrameCounter + 1) & 0xf;
			bit8 oldSync = m_d64_sync;


			m_d64_sync = ((m_d64_write_enable==0) && ((m_shifterReader & 0x3ff) == 0x3ff)) || (m_d64_forcesync!=0);

			if (m_d64_sync)
				m_frameCounter = 0;

#ifdef DEBUG_DISKPULSES
			if (!m_d64_sync && oldSync!=0)
			{
				m_debugFrameCounter = 0;
			}
#endif
		}
	}

	byteReady=1;
	if ((m_frameCounter & 7) == 7)
	{
		if ((m_clockDivider2 & 3)==3)
		{
			m_shifterWriter = m_d64_diskwritebyte;
		}
		if ((m_d64_soe_enable!=0) && (m_clockDivider2 & 2)==0)
		{
			byteReady = 0;
			if (via2.ca1_in != 0)
			{
				cpu.SOTrigger = true;
				cpu.SOTriggerClock = CurrentClock;
			}
		}
	}

#ifdef DEBUG_DISKPULSES
	static bool allowCapture = true;
	static bool printDebug = false;
	if ((m_debugFrameCounter & 7) == 7 )
	{
		if ((m_clockDivider2 & 2)==0 && allowCapture)
		{
			allowCapture = false;
			TCHAR sDebug[50];
			static int linePrintCount = 0;
			int dataByte = (int)(m_shifterReader & 0xff);
			_stprintf_s(sDebug, _countof(sDebug), TEXT("%2X "), dataByte);		
			if (printDebug)
			{
				OutputDebugString(sDebug);
				linePrintCount ++;
				if (linePrintCount>=16)
				{
					linePrintCount = 0;
					OutputDebugString(TEXT("\n"));
				}
			}
		}
	}
	else
		allowCapture = true;
#endif

	if (via2.ca1_in!=byteReady)
	{
		via2.ExecuteCycle(CurrentClock - 1);
		via2.SetCA1Input(byteReady);
	}
	if (m_clockDivider1 >= 16)
	{
		prevClockDivider1 = speed;
		goto loop;
	}
}

void DiskInterface::SpinDisk(ICLK sysclock)
{
bit8 bitTime;
ICLKS clocks;
bit8 speed;
bit8 bMotorRun = m_bDiskMotorOn;

	speed = m_clockDividerReload;
	bitTime = 0;
	clocks = (ICLKS)(sysclock - CurrentClock);
	while (clocks-- > 0)
	{
		CurrentClock++;

		if (m_motorOffClock)
		{
			m_motorOffClock--;
			bMotorRun = true;
		}
		if (m_headStepClock)
		{
			m_headStepClock--;
		}

		if (bMotorRun && mi_d64_diskchange==0)
		{
			if (m_d64_write_enable==0)
			{
				if (m_diskLoaded)
				{
					bitTime = GetDisk16(m_currentTrackNumber, m_currentHeadIndex);					
				}
				if (bitTime !=0)
				{
					bitTime--;
					ClockDividerAdd(bitTime, speed);

					//Reset the clockdividers because a disk pulse has occurred.
					m_lastGap = m_lastOne;
					m_lastOne = 0;
					m_clockDivider1 = speed;
					m_clockDivider2 = 0;

					ClockDividerAdd(16-bitTime, speed);
				}
				else
				{
					ClockDividerAdd(16, speed);
				}
			}
			else
			{
				ClockDividerAdd(16, speed);
				if (m_d64_protectOff!=0 && m_diskLoaded)
					PutDisk16(m_currentTrackNumber, m_currentHeadIndex, m_writeStream);
			}
			MotorDisk16(m_currentTrackNumber, &m_currentHeadIndex);
		}
		else
		{
			ClockDividerAdd(16, speed);
		}
	}
}

bit8 DiskInterface::GetDisk16(bit8 trackNumber, bit32 headIndex)
{
	return m_rawTrackData[trackNumber][headIndex];
}

void DiskInterface::PutDisk16(bit8 trackNumber, bit32 headIndex, bit8 data)
{
	m_rawTrackData[trackNumber][headIndex] = data;
}

void DiskInterface::MotorDisk16(bit8 trackNumber, bit32 *headIndex)
{
	if (++*headIndex >= (DISK_RAW_TRACK_SIZE))
		*headIndex = 0;
}

void DiskInterface::C64SerialBusChange(ICLK palclock, bit8 c64_serialbus)
{
ICLK palcycles, cycles;

	palcycles = palclock - CurrentPALClock;
	if ((ICLKS) palcycles <= 0)
	{
		this->D64_serial_write(m_c64_serialbus_diskview_next);
	}
	else
	{
		__int64 div = ((Disk64clk_dy2  * (__int64)palcycles) + m_diskd64clk_xf) / Disk64clk_dy1;
		cycles = (ICLK)div;
		m_changing_c64_serialbus_diskview_diskclock = this->CurrentClock + cycles;
		m_c64_serialbus_diskview_next = c64_serialbus;
	}

}

bit8 DiskInterface::GetC64SerialBusDiskView(ICLK diskclock)
{
	if ((ICLKS)(this->CurrentClock - m_changing_c64_serialbus_diskview_diskclock) >=0)
	{
		return m_c64_serialbus_diskview;
	}
	else
	{
		return m_c64_serialbus_diskview_next;
	}
}

void DiskInterface::AccumulatePendingDiskCpuClocksToPalClock(ICLK palclock)
{
ICLK palcycles, cycles;

	palcycles = palclock - CurrentPALClock;
	if ((ICLKS) palcycles <= 0)
		return;
	__int64 div = ((Disk64clk_dy2  * (__int64)palcycles) + m_diskd64clk_xf) / Disk64clk_dy1;
	m_diskd64clk_xf = ((Disk64clk_dy2 * (__int64)palcycles) + m_diskd64clk_xf) % Disk64clk_dy1;
	cycles = (ICLK)div;
	m_pendingclocks += cycles;
	CurrentPALClock = palclock;
}

void DiskInterface::ExecuteOnePendingDiskCpuClock()
{
	if (m_pendingclocks > 0)
	{
		ExecuteCycle(cpu.CurrentClock + 1);
		m_pendingclocks--;
	}
}

void DiskInterface::ExecuteAllPendingDiskCpuClocks()
{
	if (m_pendingclocks > 0)
	{
		ExecuteCycle(cpu.CurrentClock + m_pendingclocks);
		m_pendingclocks = 0;
	}
}

//985248  = 2 x 2 x 2 x 2 x 2 x 3 x 3 x 11 x 311
//1000000 = 2 x 2 x 2 x 2 x 2 x 2 x 5 x  5 x   5 x 5 x 5 x 5
//985248 PAL
//1022727 NTSC
void DiskInterface::ExecutePALClock(ICLK palclock)
{
ICLK palcycles,cycles,sysclock;

	palcycles = palclock - CurrentPALClock;
	if ((ICLKS) palcycles <= 0)
		return;

	__int64 div = ((Disk64clk_dy2  * (__int64)palcycles) + m_diskd64clk_xf) / Disk64clk_dy1;
	m_diskd64clk_xf = ((Disk64clk_dy2 * (__int64)palcycles) + m_diskd64clk_xf) % Disk64clk_dy1;

	cycles = (ICLK)div;
	m_pendingclocks = 0;
	sysclock = cpu.CurrentClock + cycles;
	ExecuteCycle(sysclock);
	CurrentPALClock = palclock;
}

void DiskInterface::ExecuteCycle(ICLK sysclock)
{
ICLKS cycles;

	cycles = (ICLKS)(sysclock - cpu.CurrentClock);
	if (cycles>0)
	{
		m_diskChangeCounter -= cycles;
		if (m_diskChangeCounter < 0)
		{
			m_diskChangeCounter += DISKCHANGECLOCKRELOAD;
			if (mi_d64_diskchange)
				mi_d64_diskchange--;
		}
		ICLK clock_change =  m_changing_c64_serialbus_diskview_diskclock;
		ICLKS cycles_to_serialbuschange = (ICLKS)(clock_change - cpu.CurrentClock);
		if (cycles_to_serialbuschange < 0 || cycles < cycles_to_serialbuschange)
		{
			cpu.ExecuteCycle(sysclock);
			SpinDisk(sysclock);
			via1.ExecuteCycle(sysclock);
			via2.ExecuteCycle(sysclock);
		}
		else
		{
			if (cycles_to_serialbuschange > 0)
			{
				cpu.ExecuteCycle(clock_change);
				SpinDisk(clock_change);
				via1.ExecuteCycle(clock_change);
				via2.ExecuteCycle(clock_change);
				cycles -= cycles_to_serialbuschange;
			}

			this->D64_serial_write(m_c64_serialbus_diskview_next);

			if (cycles > 0)
			{
				cpu.ExecuteCycle(sysclock);
				SpinDisk(sysclock);
				via1.ExecuteCycle(sysclock);
				via2.ExecuteCycle(sysclock);
			}
		}
	}
}

void DiskInterface::PreventClockOverflow()
{
	const ICLKS CLOCKSYNCBAND_NEAR = 0x4000;
	const ICLKS CLOCKSYNCBAND_FAR = 0x40000000;
	ICLK ClockBehindNear = CurrentClock - CLOCKSYNCBAND_NEAR;

	if ((ICLKS)(CurrentClock - m_changing_c64_serialbus_diskview_diskclock) >= CLOCKSYNCBAND_FAR)
		m_changing_c64_serialbus_diskview_diskclock = ClockBehindNear;

	if ((ICLKS)(CurrentClock - m_driveWriteChangeClock) >= CLOCKSYNCBAND_FAR)
		m_driveWriteChangeClock = ClockBehindNear;
	cpu.PreventClockOverflow();
}

void DiskInterface::LoadImageBits(GCRDISK *dsk)
{
int i;
	WaitThreadReady();
	m_d64TrackCount = dsk->m_d64TrackCount;
	for (i=0 ; i < G64_MAX_TRACKS ; i++)
	{
		memcpy_s(m_rawTrackData[i], DISK_RAW_TRACK_SIZE, dsk->m_rawTrackData[i], DISK_RAW_TRACK_SIZE);
	}
}

void DiskInterface::SaveImageBits(GCRDISK *dsk)
{
int i;

	WaitThreadReady();
	dsk->m_d64TrackCount = m_d64TrackCount;

	for (i=0 ; i < G64_MAX_TRACKS ; i++)
	{
		memcpy_s(dsk->m_rawTrackData[i], DISK_RAW_TRACK_SIZE, m_rawTrackData[i], DISK_RAW_TRACK_SIZE);
	}
}

void DiskInterface::SetDiskLoaded()
{
	WaitThreadReady();
	if (m_diskLoaded)
		mi_d64_diskchange=DISKCHANGE3;
	else
		mi_d64_diskchange=DISKCHANGE1;
	m_diskLoaded = 1;
}

void DiskInterface::RemoveDisk()
{
	WaitThreadReady();
	if (m_diskLoaded)
	{
		m_diskLoaded = 0;
		mi_d64_diskchange = DISKCHANGE1;
	}
}

bit8 DiskInterface::ReadRegister(bit16 address, ICLK sysclock)
{
	return 0;
}

void DiskInterface::WriteRegister(bit16 address, ICLK sysclock, bit8 data)
{
}

bit8 DiskInterface::ReadRegister_no_affect(bit16 address, ICLK sysclock)
{
	return 0;
}

bit8 DiskInterface::GetHalfTrackIndex()
{
	return m_currentTrackNumber;
}
