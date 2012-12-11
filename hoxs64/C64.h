#ifndef __C64_H__
#define __C64_H__

#include <vector>
#include <list>

#include "boost2005.h"
#include "user_message.h"
#include "defines.h"
#include "mlist.h"
#include "carray.h"
#include "cevent.h"

#include "errormsg.h"
#include "bits.h"
#include "util.h"
#include "register.h"
#include "hconfig.h"
#include "appstatus.h"
#include "dxstuff9.h"
#include "cart.h"
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
#include "diskinterface.h"
#include "t64.h"
#include "cart.h"
#include "c64file.h"
#include "assembler.h"
#include "runcommand.h"
#include "commandresult.h"
#include "monitor.h"

class SIDLoader;

class C64 : public IC64, private ITapeEvent, public IAutoLoad, public ErrorMsg
{
public:
	C64();
	~C64();
	RAM64 ram;
	CPU6510 cpu;
	VIC6569 vic;
	CIA1 cia1;
	CIA2 cia2;
	SID64 sid;
	Tape64 tape64;
	DiskInterface diskdrive;

	CAppStatus *appStatus;
	CConfig *cfg;
	IC64Event *pIC64Event;
	CDX9 *dx;
	Monitor mon;
	HRESULT Init(CConfig *, CAppStatus *, IC64Event *, CDX9 *, TCHAR *szAppDirectory);
	void Reset(ICLK sysclock);
	void ExecuteDiskInstruction();
	void ExecuteC64Instruction();
	void ExecuteC64Clock();
	void ExecuteDiskClock();
	void ExecuteDebugFrame();
	void ExecuteFrame();

	void EnterDebugRun(bool bWithSound);
	void FinishDebugRun();

	void SetBasicProgramEndAddress(bit16 last_byte);

	HRESULT AutoLoad(TCHAR *s, int directoryIndex, bool bIndexOnlyPrgFiles, const bit8 c64filename[C64DISKFILENAMELENGTH], bool bQuickLoad, bool bAlignD64Tracks);

	HRESULT LoadCrtFile(TCHAR *filename);
	HRESULT LoadImageFile(TCHAR *filename, bit16* pStartAddress, bit16* pSize);
	HRESULT LoadT64ImageFile(TCHAR *filename, int t64Index, bit16* pStartAddress, bit16* pSize);
	HRESULT LoadTAPFile(TCHAR *filename);
	HRESULT InsertDiskImageFile(TCHAR *filename, bool bAlignD64Tracks);
	HRESULT LoadD64FromFile(TCHAR *filename, bool bAlignD64Tracks);
	HRESULT LoadG64FromFile(TCHAR *filename);
	HRESULT LoadFDIFromFile(TCHAR *filename);

	HRESULT SaveD64ToFile(TCHAR *filename, int numberOfTracks);
	HRESULT SaveFDIToFile(TCHAR *filename);


	//IAutoLoad
	void AutoLoadHandler(ICLK sysclock);

	//ITapeEvent
	void Pulse(ICLK sysclock);
	void EndOfTape(ICLK sysclock);
	
	//IC64
	virtual void HardReset(bool bCancelAutoload);
	virtual void SoftReset(bool bCancelAutoload);
	virtual void PostHardReset(bool bCancelAutoload);
	virtual void PostSoftReset(bool bCancelAutoload);
	virtual void ResetKeyboard();
	virtual void TapePressPlay();
	virtual void TapePressStop();
	virtual void TapePressRewind();
	virtual void TapePressEject();
	virtual HRESULT InsertNewDiskImage(TCHAR *diskname, bit8 id1, bit8 id2, bool bAlignD64Tracks, int numberOfTracks);
	virtual void RemoveDisk();
	virtual void Set_DiskProtect(bool bOn);
	virtual bool Get_DiskProtect();
	virtual void DiskReset();
	virtual IMonitor *GetMon();
	virtual void SetupColorTables(unsigned int d3dFormat);
	virtual HRESULT UpdateBackBuffer();
	virtual void SynchroniseDevicesWithVIC();

	IMonitorCpu *GetCpu(int cpuid);

	enum AutoLoadType
	{
		AUTOLOAD_NONE,
		AUTOLOAD_TAP_FILE,
		AUTOLOAD_PRG_FILE,
		AUTOLOAD_T64_FILE,
		AUTOLOAD_DISK_FILE,
		AUTOLOAD_SID_FILE,
		AUTOLOAD_CRT_FILE
	};

	enum AutoLoadSequence
	{
		AUTOSEQ_RESET=0,
		AUTOSEQ_LOAD=1,
		AUTOSEQ_RUN=2
	};

	void PreventClockOverflow();
	void CheckDriveLedNofication();
	static const bit16 BASICSTARTADDRESS = 0x0801;
	static const int SCREENWRITELOCATION = 1024 + 40*6;
	void WriteSysCallToScreen(bit16 startaddress);
protected:
	TCHAR m_szAppDirectory[MAX_PATH+1];
	struct AutoLoadCommand
	{
		enum AutoLoadType type;
		enum AutoLoadSequence sequence;
		bit16 startaddress;
		bit16 imageSize;
		TCHAR filename[MAX_PATH+1];
		bit8 c64filename[C64DISKFILENAMELENGTH];
		int directoryIndex;
		bool bIndexOnlyPrgFiles;
		bool bQuickLoad;
		bool bAlignD64Tracks;
		bit8 *pImageData;
		class SIDLoader *pSidFile;


		AutoLoadCommand()
		{
			pImageData = 0;
			pSidFile = 0;
			CleanUp();
		}
		~AutoLoadCommand()
		{
			CleanUp();
		}
		void CleanUp()
		{
			if (pImageData)
			{
				GlobalFree(pImageData);
				pImageData = 0;

			}
			if (pSidFile)
			{
				delete(pSidFile);
				pSidFile = 0;
			}
			imageSize = 0;
			type = AUTOLOAD_NONE;
			sequence = AUTOSEQ_RESET;
			startaddress = 0;
			directoryIndex = 0;
			bQuickLoad = false;
			memset(c64filename, 0xA0, sizeof(c64filename));
		}
	};
	AutoLoadCommand autoLoadCommand;


private:
	void ProcessReset();
	bool bPendingReset;
	bool bHardResetSystem;
	bool bSoftResetSystem;
	bool m_bLastPostedDriveWriteLed;
	ICLK m_iClockOverflowCheckCounter;
};

#endif
