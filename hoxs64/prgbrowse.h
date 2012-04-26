#ifndef __CPRGBrowse_H__
#define	__CPRGBrowse_H__

typedef class CArrayElement<struct C64Filename> CDirectoryElement;
typedef class CArray<struct C64Filename> CDirectoryArray;


class CPRGBrowse
{
public:
	enum filetype {ALL=0,FDI=1,G64=2,D64=4,TAP=8,T64=16,PRG=32,P00=64,SID=128};
	CPRGBrowse();
	~CPRGBrowse();
	HRESULT Init(bit8 *charGen);
	BOOL Open(HINSTANCE hInstance, OPENFILENAME *pOF, enum filetype filetypes);
	LRESULT ChildDialogProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
	void PopulateList(HWND hDlg);
	HRESULT CreateControls(HWND hDlg);
	CDirectoryArray dataList;

	void DrawC64String(HDC hdc, int x, int y, BYTE str[], int length, bool bShifted, int scalex, int scaley);
	void DrawC64String(HDC hdc, int x, int y, BYTE str[], int length, bool bShifted, int fontheight);

	HINSTANCE m_hInstance;
	int SelectedListItem;
	int SelectedDirectoryIndex;
	bool SelectedQuickLoadDiskFile;
	bool SelectedAlignD64Tracks;
	bit8 SelectedC64FileName[C64DISKFILENAMELENGTH];
	int SelectedC64FileNameLength;

	enum filetype mAllowTypes;
	static const int HEADERLINES = 1;
	bool DisableQuickLoad;

private:
	CDPI m_dpi;
	static const int MAXLISTITEMCOUNT = 1000;
	int miLoadedListItemCount;
	enum FIS
	{
		COMPLETED = 0,
		WORKING = 1
	};
	void CancelFileInspector();
	HRESULT BeginFileInspector(HWND hWndDlg, TCHAR *fileName);
	static DWORD WINAPI StartFileInspectorThread(LPVOID lpParam);
	DWORD StartFileInspector();
	HWND mhWndInspector;
	TCHAR mptsFileName[MAX_PATH+1];
	HANDLE mhEvtComplete;
	CRITICAL_SECTION mCrtStatus;
	bool mbSectionOK;
	HANDLE mhEvtQuit;
	FIS mFileInspectorStatus;
	HRESULT mFileInspectorResult;
	void InspectorCompleteFail();
	void InspectorCompleteOK();
	void InspectorStart();

	HBRUSH m_hbrush;
	void ReSize(HWND hDlg, LONG w, LONG h);
	void CleanUp();
	bit8 *m_pCharGen;
	C64File m_c64file;
	HWND m_hParent;
	HWND m_hBrowse;
	HWND m_hListBox;
	HWND m_hCheckQuickLoad;
	HWND m_hCheckAlignD64Tracks;
	int m_width_custom;
	int mgapTop;
	int mgapBottom;
	bool mbGapsDone;
	void OnMeasureListViewItem(LPMEASUREITEMSTRUCT lpdis);
	void OnDrawListViewItem(LPDRAWITEMSTRUCT lpdis);
};



#endif