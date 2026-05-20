#pragma once

#include "Core/Debugger/DebugInterface.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/Debugger/Breakpoints.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/Debugger/Watch.h"
#include "Core/MIPS/MIPSStackWalk.h"
#include "Windows/W32Util/Misc.h"

class CtrlThreadList: public GenericListControl
{
public:
	CtrlThreadList(HWND hwnd);
	void reloadThreads();
	void showMenu(int itemIndex, const POINT &pt);
	const char* getCurrentThreadName();
protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return (int) threads.size(); }
	void OnDoubleClick(int itemIndex, int column) override;
	void OnRightClick(int itemIndex, int column, const POINT &point) override;
private:
	std::vector<DebugThreadInfo> threads;
};

class CtrlDisAsmView;

class CtrlBreakpointList: public GenericListControl
{
public:
	CtrlBreakpointList(HWND hwnd, MIPSDebugInterface* cpu, CtrlDisAsmView* disasm);
	void reloadBreakpoints();
protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return getTotalBreakpointCount(); }
	void OnDoubleClick(int itemIndex, int column) override;
	void OnRightClick(int itemIndex, int column, const POINT &point) override;
	void OnToggle(int item, bool newValue) override;
private:
	std::vector<BreakPoint> displayedBreakPoints_;
	std::vector<MemCheck> displayedMemChecks_;
	std::wstring breakpointText;
	MIPSDebugInterface* cpu;
	CtrlDisAsmView* disasm;

	void editBreakpoint(int itemIndex);
	void gotoBreakpointAddress(int itemIndex);
	void removeBreakpoint(int itemIndex);
	int getTotalBreakpointCount();
	int getBreakpointIndex(int itemIndex, bool& isMemory);
	void showBreakpointMenu(int itemIndex, const POINT &pt);
	void toggleEnabled(int itemIndex);

	void ImportBreakpoints();
	void ExportBreakpoints();
	void ClearAllBreakpoints();
};

class CtrlStackTraceView: public GenericListControl
{
public:
	CtrlStackTraceView(HWND hwnd, DebugInterface* cpu, CtrlDisAsmView* disasm);
	void loadStackTrace();
protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return (int)frames.size(); }
	void OnDoubleClick(int itemIndex, int column) override;
private:
	std::vector<MIPSStackWalk::StackFrame> frames;
	DebugInterface* cpu;
	CtrlDisAsmView* disasm;
};

class CtrlModuleList: public GenericListControl
{
public:
	CtrlModuleList(HWND hwnd, DebugInterface* cpu);
	void loadModules();
protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return (int)modules.size(); }
	void OnDoubleClick(int itemIndex, int column) override;
private:
	std::vector<LoadedModuleInfo> modules;
	DebugInterface* cpu;
};

class CtrlWatchList : public GenericListControl {
public:
	CtrlWatchList(HWND hwnd, DebugInterface *cpu);
	void RefreshValues();

protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return (int)watches_.size(); }
	void OnRightClick(int itemIndex, int column, const POINT &point) override;
	bool ListenRowPrePaint() override { return true; }
	bool OnRowPrePaint(int row, LPNMLVCUSTOMDRAW msg) override;

private:
	void AddWatch();
	void EditWatch(int pos);
	void DeleteWatch(int pos);
	bool HasWatchChanged(int pos);

	std::vector<WatchInfo> watches_;
	DebugInterface *cpu_;
};

// Shows only labels marked as user-defined (via SymbolMap::MarkLabelAsUser).
// Sister tab to the disasm-view right-click "Add Symbol Here..." flow —
// gives a single pane to review, jump to, rename, and delete custom labels
// you've added during a session. Import/export and Clear-All also live here.
class CtrlUserSymbolList : public GenericListControl {
public:
	CtrlUserSymbolList(HWND hwnd, DebugInterface *cpu);
	void Refresh();
	void OnDoubleClick(int itemIndex, int column) override;
	void OnRightClick(int itemIndex, int column, const POINT &point) override;

protected:
	bool WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) override;
	void GetColumnText(wchar_t *dest, size_t destSize, int row, int col) override;
	int GetRowCount() override { return (int)symbols_.size(); }

private:
	void JumpTo(int pos);
	void Edit(int pos);
	void Delete(int pos);
	void CopyAddress(int pos);
	void AddNew();
	void Import();
	void Export();
	void ClearAll();

	std::vector<SymbolEntry> symbols_;
	DebugInterface *cpu_;
};
