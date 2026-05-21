#include "Windows/Debugger/Debugger_Lists.h"
#include "Common/CommonWindows.h"
#include <windowsx.h>
#include <commctrl.h>
#include "Windows/Debugger/BreakpointWindow.h"
#include "Windows/Debugger/CtrlDisAsmView.h"
#include "Windows/Debugger/DebuggerShared.h"
#include "Windows/Debugger/WatchItemWindow.h"
#include "Windows/W32Util/ContextMenu.h"
#include "Windows/MainWindow.h"
#include "Windows/InputBox.h"
#include "Windows/W32Util/ShellUtil.h"
#include "Windows/resource.h"
#include "Windows/main.h"
#include "Common/Data/Encoding/Utf8.h"
#include "Common/File/FileUtil.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/HLE/sceKernelThread.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

enum { TL_NAME, TL_PROGRAMCOUNTER, TL_ENTRYPOINT, TL_PRIORITY, TL_STATE, TL_WAITTYPE, TL_COLUMNCOUNT };
enum { BPL_ENABLED, BPL_TYPE, BPL_OFFSET, BPL_SIZELABEL, BPL_OPCODE, BPL_CONDITION, BPL_HITS, BPL_COLUMNCOUNT };
enum { SF_ENTRY, SF_ENTRYNAME, SF_CURPC, SF_CUROPCODE, SF_CURSP, SF_FRAMESIZE, SF_COLUMNCOUNT };
enum { ML_NAME, ML_ADDRESS, ML_SIZE, ML_ACTIVE, ML_COLUMNCOUNT };
enum { WL_NAME, WL_EXPRESSION, WL_VALUE, WL_COLUMNCOUNT };
enum { USL_ADDRESS, USL_NAME, USL_COLUMNCOUNT };

GenericListViewColumn threadColumns[TL_COLUMNCOUNT] = {
	{ L"Name",			0.20f },
	{ L"PC",			0.15f },
	{ L"Entry Point",	0.15f },
	{ L"Priority",		0.15f },
	{ L"State",			0.15f },
	{ L"Wait type",		0.20f }
};

GenericListViewDef threadListDef = {
	threadColumns,	ARRAY_SIZE(threadColumns),	NULL,	false
};

GenericListViewColumn breakpointColumns[BPL_COLUMNCOUNT] = {
	{ L"",				0.03f },	// enabled
	{ L"Type",			0.15f },
	{ L"Offset",		0.12f },
	{ L"Size/Label",	0.20f },
	{ L"Opcode",		0.28f },
	{ L"Condition",		0.17f },
	{ L"Hits",			0.05f },
};

GenericListViewDef breakpointListDef = {
	breakpointColumns,	ARRAY_SIZE(breakpointColumns),	NULL,	true
};

GenericListViewColumn stackTraceColumns[SF_COLUMNCOUNT] = {
	{ L"Entry",			0.12f },
	{ L"Name",			0.24f },
	{ L"PC",			0.12f },
	{ L"Opcode",		0.28f },
	{ L"SP",			0.12f },
	{ L"Frame Size",	0.12f }
};

GenericListViewDef stackTraceListDef = {
	stackTraceColumns,	ARRAY_SIZE(stackTraceColumns),	NULL,	false
};

GenericListViewColumn moduleListColumns[ML_COLUMNCOUNT] = {
	{ L"Name",			0.25f },
	{ L"Address",		0.25f },
	{ L"Size",			0.25f },
	{ L"Active",		0.25f },
};

GenericListViewDef moduleListDef = {
	moduleListColumns,	ARRAY_SIZE(moduleListColumns),	NULL,	false
};

GenericListViewColumn watchListColumns[WL_COLUMNCOUNT] = {
	{ L"Name",          0.25f },
	{ L"Expression",    0.5f },
	{ L"Value",         0.25f },
};

GenericListViewDef watchListDef = {
	watchListColumns, ARRAY_SIZE(watchListColumns), nullptr, false,
};

GenericListViewColumn userSymListColumns[USL_COLUMNCOUNT] = {
	{ L"Address",       0.30f },
	{ L"Name",          0.70f },
};

GenericListViewDef userSymListDef = {
	userSymListColumns, ARRAY_SIZE(userSymListColumns), nullptr, false,
};

//
// CtrlThreadList
//

CtrlThreadList::CtrlThreadList(HWND hwnd): GenericListControl(hwnd,threadListDef)
{
	Update();
}

bool CtrlThreadList::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& returnValue)
{
	switch (msg)
	{
	case WM_KEYDOWN:
		if (wParam == VK_TAB)
		{
			SendMessage(GetParent(GetHandle()),WM_DEB_TABPRESSED,0,0);
			returnValue = 0;
			return true;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN)
		{
			if (wParam == VK_TAB)
			{
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	}

	return false;
}

void CtrlThreadList::showMenu(int itemIndex, const POINT &pt)
{
	auto threadInfo = threads[itemIndex];

	// Can't do it, sorry.  Needs to not be running.
	if (Core_IsActive())
		return;

	HMENU subMenu = GetContextMenu(ContextMenuID::THREADLIST);
	switch (threadInfo.status) {
	case THREADSTATUS_DEAD:
	case THREADSTATUS_DORMANT:
	case THREADSTATUS_RUNNING:
		EnableMenuItem(subMenu, ID_DISASM_THREAD_FORCERUN, MF_BYCOMMAND | MF_DISABLED);
		EnableMenuItem(subMenu, ID_DISASM_THREAD_KILL, MF_BYCOMMAND | MF_DISABLED);
		break;
	case THREADSTATUS_READY:
		EnableMenuItem(subMenu, ID_DISASM_THREAD_FORCERUN, MF_BYCOMMAND | MF_DISABLED);
		EnableMenuItem(subMenu, ID_DISASM_THREAD_KILL, MF_BYCOMMAND | MF_ENABLED);
		break;
	case THREADSTATUS_SUSPEND:
	case THREADSTATUS_WAIT:
	case THREADSTATUS_WAITSUSPEND:
	default:
		EnableMenuItem(subMenu, ID_DISASM_THREAD_FORCERUN, MF_BYCOMMAND | MF_ENABLED);
		EnableMenuItem(subMenu, ID_DISASM_THREAD_KILL, MF_BYCOMMAND | MF_ENABLED);
		break;
	}

	switch (TriggerContextMenu(ContextMenuID::THREADLIST, GetHandle(), ContextPoint::FromClient(pt)))
	{
	case ID_DISASM_THREAD_FORCERUN:
		__KernelResumeThreadFromWait(threadInfo.id, 0);
		reloadThreads();
		break;
	case ID_DISASM_THREAD_KILL:
		sceKernelTerminateThread(threadInfo.id);
		reloadThreads();
		break;
	}
}

void CtrlThreadList::GetColumnText(wchar_t* dest, size_t destSize, int row, int col)
{
	if (row < 0 || row >= (int)threads.size()) {
		return;
	}

	switch (col)
	{
	case TL_NAME:
		wcscpy(dest, ConvertUTF8ToWString(threads[row].name).c_str());
		break;
	case TL_PROGRAMCOUNTER:
		switch (threads[row].status)
		{
		case THREADSTATUS_DORMANT:
		case THREADSTATUS_DEAD:
			wcscpy(dest, L"N/A");
			break;
		default:
			wsprintf(dest, L"0x%08X",threads[row].curPC);
			break;
		};
		break;
	case TL_ENTRYPOINT:
		wsprintf(dest,L"0x%08X",threads[row].entrypoint);
		break;
	case TL_PRIORITY:
		wsprintf(dest,L"%d",threads[row].priority);
		break;
	case TL_STATE:
		switch (threads[row].status)
		{
		case THREADSTATUS_RUNNING:
			wcscpy(dest,L"Running");
			break;
		case THREADSTATUS_READY:
			wcscpy(dest,L"Ready");
			break;
		case THREADSTATUS_WAIT:
			wcscpy(dest,L"Waiting");
			break;
		case THREADSTATUS_SUSPEND:
			wcscpy(dest,L"Suspended");
			break;
		case THREADSTATUS_DORMANT:
			wcscpy(dest,L"Dormant");
			break;
		case THREADSTATUS_DEAD:
			wcscpy(dest,L"Dead");
			break;
		case THREADSTATUS_WAITSUSPEND:
			wcscpy(dest,L"Waiting/Suspended");
			break;
		default:
			wcscpy(dest,L"Invalid");
			break;
		}
		break;
	case TL_WAITTYPE:
		wcscpy(dest, ConvertUTF8ToWString(WaitTypeToString(threads[row].waitType)).c_str());
		break;
	}
}

void CtrlThreadList::OnDoubleClick(int itemIndex, int column)
{
	u32 address;
	switch (threads[itemIndex].status)
	{
	case THREADSTATUS_DORMANT:
	case THREADSTATUS_DEAD:
		address = threads[itemIndex].entrypoint;
		break;
	default:
		address = threads[itemIndex].curPC;
		break;
	}

	SendMessage(GetParent(GetHandle()),WM_DEB_GOTOWPARAM,address,0);
}

void CtrlThreadList::OnRightClick(int itemIndex, int column, const POINT& point)
{
	showMenu(itemIndex,point);
}

void CtrlThreadList::reloadThreads()
{
	threads = GetThreadsInfo();
	Update();
}

const char* CtrlThreadList::getCurrentThreadName()
{
	for (size_t i = 0; i < threads.size(); i++)
	{
		if (threads[i].isCurrent) return threads[i].name;
	}

	return "N/A";
}


//
// CtrlBreakpointList
//

CtrlBreakpointList::CtrlBreakpointList(HWND hwnd, MIPSDebugInterface* cpu, CtrlDisAsmView* disasm)
	: GenericListControl(hwnd,breakpointListDef),cpu(cpu),disasm(disasm)
{
	SetSendInvalidRows(true);
	Update();
}

bool CtrlBreakpointList::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& returnValue)
{
	switch(msg)
	{
	case WM_KEYDOWN:
		returnValue = 0;
		if(wParam == VK_RETURN)
		{
			int index = GetSelectedIndex();
			editBreakpoint(index);
			return true;
		} else if (wParam == VK_DELETE)
		{
			int index = GetSelectedIndex();
			removeBreakpoint(index);
			return true;
		} else if (wParam == VK_TAB)
		{
			SendMessage(GetParent(GetHandle()),WM_DEB_TABPRESSED,0,0);
			return true;
		} else if (wParam == VK_SPACE)
		{
			int index = GetSelectedIndex();
			toggleEnabled(index);
			return true;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN)
		{
			if (wParam == VK_TAB || wParam == VK_RETURN)
			{
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	}

	return false;
}

void CtrlBreakpointList::reloadBreakpoints()
{
	// Update the items we're displaying from the debugger.
	displayedBreakPoints_ = g_breakpoints.GetBreakpoints();
	displayedMemChecks_= g_breakpoints.GetMemChecks();

	for (int i = 0; i < GetRowCount(); i++)
	{
		bool isMemory;
		int index = getBreakpointIndex(i, isMemory);
		if (index < 0)
			continue;

		if (isMemory)
			SetCheckState(i, displayedMemChecks_[index].IsEnabled());
		else
			SetCheckState(i, displayedBreakPoints_[index].IsEnabled());
	}

	Update();
}

void CtrlBreakpointList::editBreakpoint(int itemIndex)
{
	bool isMemory;
	int index = getBreakpointIndex(itemIndex, isMemory);
	if (index == -1) return;

	BreakpointWindow win(GetHandle(),cpu);
	if (isMemory)
	{
		auto mem = displayedMemChecks_[index];
		win.loadFromMemcheck(mem);
		if (win.exec())
		{
			g_breakpoints.RemoveMemCheck(mem.start,mem.end);
			win.addBreakpoint();
		}
	} else {
		auto bp = displayedBreakPoints_[index];
		win.loadFromBreakpoint(bp);
		if (win.exec())
		{
			g_breakpoints.RemoveBreakPoint(bp.addr);
			win.addBreakpoint();
		}
	}
}

void CtrlBreakpointList::toggleEnabled(int itemIndex)
{
	bool isMemory;
	int index = getBreakpointIndex(itemIndex, isMemory);
	if (index == -1) return;

	if (isMemory) {
		MemCheck mcPrev = displayedMemChecks_[index];
		g_breakpoints.ChangeMemCheck(mcPrev.start, mcPrev.end, mcPrev.cond, BreakAction(mcPrev.result ^ BREAK_ACTION_PAUSE));
	} else {
		BreakPoint bpPrev = displayedBreakPoints_[index];
		g_breakpoints.ChangeBreakPoint(bpPrev.addr, BreakAction(bpPrev.result ^ BREAK_ACTION_PAUSE));
	}
}

void CtrlBreakpointList::gotoBreakpointAddress(int itemIndex)
{
	bool isMemory;
	int index = getBreakpointIndex(itemIndex, isMemory);
	if (index == -1)
		return;

	if (isMemory) {
		u32 address = displayedMemChecks_[index].start;
		MainWindow::CreateMemoryWindow();
		if (memoryWindow)
			memoryWindow->Goto(address);
	} else {
		u32 address = displayedBreakPoints_[index].addr;
		MainWindow::CreateDisasmWindow();
		if (disasmWindow)
			disasmWindow->Goto(address);
	}
}

void CtrlBreakpointList::removeBreakpoint(int itemIndex)
{
	bool isMemory;
	int index = getBreakpointIndex(itemIndex,isMemory);
	if (index == -1) return;

	if (isMemory) {
		auto mc = displayedMemChecks_[index];
		g_breakpoints.RemoveMemCheck(mc.start, mc.end);
	} else {
		u32 address = displayedBreakPoints_[index].addr;
		g_breakpoints.RemoveBreakPoint(address);
	}
}

int CtrlBreakpointList::getTotalBreakpointCount() {
	int count = (int)displayedMemChecks_.size();
	for (auto bp : displayedBreakPoints_) {
		if (!bp.temporary)
			++count;
	}

	return count;
}

int CtrlBreakpointList::getBreakpointIndex(int itemIndex, bool& isMemory)
{
	// memory breakpoints first
	if (itemIndex < (int)displayedMemChecks_.size())
	{
		isMemory = true;
		return itemIndex;
	}

	itemIndex -= (int)displayedMemChecks_.size();

	size_t i = 0;
	while (i < displayedBreakPoints_.size())
	{
		if (displayedBreakPoints_[i].temporary)
		{
			i++;
			continue;
		}

		// the index is 0 when there are no more breakpoints to skip
		if (itemIndex == 0)
		{
			isMemory = false;
			return (int)i;
		}

		i++;
		itemIndex--;
	}

	return -1;
}

void CtrlBreakpointList::GetColumnText(wchar_t* dest, size_t destSize, int row, int col)
{
	if (!PSP_IsInited()) {
		return;
	}
	bool isMemory;
	int index = getBreakpointIndex(row,isMemory);
	if (index == -1) return;
		
	switch (col)
	{
	case BPL_TYPE:
		{
			if (isMemory) {
				switch ((int)displayedMemChecks_[index].cond) {
				case MEMCHECK_READ:
					wcscpy(dest,L"Read");
					break;
				case MEMCHECK_WRITE:
					wcscpy(dest,L"Write");
					break;
				case MEMCHECK_READWRITE:
					wcscpy(dest,L"Read/Write");
					break;
				case MEMCHECK_WRITE | MEMCHECK_WRITE_ONCHANGE:
					wcscpy(dest,L"Write Change");
					break;
				case MEMCHECK_READWRITE | MEMCHECK_WRITE_ONCHANGE:
					wcscpy(dest,L"Read/Write Change");
					break;
				}
			} else {
				wcscpy(dest, L"Execute");
			}
		}
		break;
	case BPL_OFFSET:
		{
			if (isMemory) {
				wsprintf(dest,L"0x%08X",displayedMemChecks_[index].start);
			} else {
				wsprintf(dest,L"0x%08X",displayedBreakPoints_[index].addr);
			}
		}
		break;
	case BPL_SIZELABEL:
		{
			if (isMemory) {
				auto mc = displayedMemChecks_[index];
				if (mc.end == 0)
					wsprintf(dest,L"0x%08X",1);
				else
					wsprintf(dest,L"0x%08X",mc.end-mc.start);
			} else {
				const std::string sym = g_symbolMap->GetLabelString(displayedBreakPoints_[index].addr);
				if (!sym.empty()) {
					ConvertUTF8ToWString(dest, destSize, sym);
				} else {
					wcscpy(dest,L"-");
				}
			}
		}
		break;
	case BPL_OPCODE:
		{
			if (isMemory) {
				wcscpy(dest,L"-");
			} else {
				char temp[256];
				disasm->getOpcodeText(displayedBreakPoints_[index].addr, temp, sizeof(temp));
				ConvertUTF8ToWString(dest, destSize, temp);
			}
		}
		break;
	case BPL_CONDITION:
		{
			if (isMemory || displayedBreakPoints_[index].hasCond == false) {
				wcscpy(dest,L"-");
			} else {
				std::wstring s = ConvertUTF8ToWString(displayedBreakPoints_[index].cond.expressionString);
				wcscpy(dest,s.c_str());
			}
		}
		break;
	case BPL_HITS:
		{
			if (isMemory) {
				wsprintf(dest,L"%d",displayedMemChecks_[index].numHits);
			} else {
				wsprintf(dest,L"-");
			}
		}
		break;
	case BPL_ENABLED:
		{
			wsprintf(dest,L"\xFFFE");
		}
		break;
	}
}

void CtrlBreakpointList::OnDoubleClick(int itemIndex, int column)
{
	gotoBreakpointAddress(itemIndex);
}

void CtrlBreakpointList::OnRightClick(int itemIndex, int column, const POINT& point)
{
	showBreakpointMenu(itemIndex,point);
}

void CtrlBreakpointList::OnToggle(int item, bool newValue)
{
	toggleEnabled(item);
}

void CtrlBreakpointList::showBreakpointMenu(int itemIndex, const POINT &pt)
{
	bool isMemory;
	int index = getBreakpointIndex(itemIndex, isMemory);
	if (index == -1)
	{
		switch (TriggerContextMenu(ContextMenuID::NEWBREAKPOINT, GetHandle(), ContextPoint::FromClient(pt)))
		{
		case ID_DISASM_ADDNEWBREAKPOINT:
			{
				BreakpointWindow bpw(GetHandle(),cpu);
				if (bpw.exec()) bpw.addBreakpoint();
			}
			break;
		case ID_DISASM_BP_IMPORT: ImportBreakpoints(); break;
		case ID_DISASM_BP_EXPORT: ExportBreakpoints(); break;
		case ID_DISASM_BP_CLEAR:  ClearAllBreakpoints(); break;
		}
	} else {
		MemCheck mcPrev;
		BreakPoint bpPrev;
		if (isMemory) {
			mcPrev = displayedMemChecks_[index];
		} else {
			bpPrev = displayedBreakPoints_[index];
		}

		HMENU subMenu = GetContextMenu(ContextMenuID::BREAKPOINTLIST);
		if (isMemory) {
			CheckMenuItem(subMenu, ID_DISASM_DISABLEBREAKPOINT, MF_BYCOMMAND | (mcPrev.IsEnabled() ? MF_CHECKED : MF_UNCHECKED));
		} else {
			CheckMenuItem(subMenu, ID_DISASM_DISABLEBREAKPOINT, MF_BYCOMMAND | (bpPrev.IsEnabled() ? MF_CHECKED : MF_UNCHECKED));
		}

		switch (TriggerContextMenu(ContextMenuID::BREAKPOINTLIST, GetHandle(), ContextPoint::FromClient(pt)))
		{
		case ID_DISASM_DISABLEBREAKPOINT:
			if (isMemory) {
				g_breakpoints.ChangeMemCheck(mcPrev.start, mcPrev.end, mcPrev.cond, BreakAction(mcPrev.result ^ BREAK_ACTION_PAUSE));
			} else {
				g_breakpoints.ChangeBreakPoint(bpPrev.addr, BreakAction(bpPrev.result ^ BREAK_ACTION_PAUSE));
			}
			break;
		case ID_DISASM_EDITBREAKPOINT:
			editBreakpoint(itemIndex);
			break;
		case ID_DISASM_ADDNEWBREAKPOINT:
			{		
				BreakpointWindow bpw(GetHandle(),cpu);
				if (bpw.exec()) bpw.addBreakpoint();
			}
			break;
		case ID_DISASM_DELETEBREAKPOINT:
			removeBreakpoint(itemIndex);
			break;
		case ID_DISASM_BP_IMPORT: ImportBreakpoints(); break;
		case ID_DISASM_BP_EXPORT: ExportBreakpoints(); break;
		case ID_DISASM_BP_CLEAR:  ClearAllBreakpoints(); break;
		}
	}
}

// Import/Export breakpoints. Format is plain text, tab-separated.
//   EX  addr  enabled  log  cond_expr  log_fmt        — exec breakpoint
//   MEM start end  cond  enabled  log  cond_expr  log_fmt — memory check
// `cond` for MEM is one of R, W, RW, RWC. `cond_expr` and `log_fmt` may be
// empty (consecutive tabs). Lines starting with # are comments; blank lines
// ignored. Tabs and newlines in cond_expr/log_fmt are not supported and
// such entries are silently skipped on export — a reasonable trade since
// the BreakpointWindow UI doesn't make them easy to enter.

static std::string EscapeForBPField(const std::string &s) {
	for (char c : s) {
		if (c == '\t' || c == '\n' || c == '\r') return std::string();
	}
	return s;
}

static std::vector<std::string> SplitTabs(const std::string &line, int expected) {
	std::vector<std::string> out;
	size_t prev = 0;
	for (size_t i = 0; i <= line.size(); ++i) {
		if (i == line.size() || line[i] == '\t') {
			out.emplace_back(line.substr(prev, i - prev));
			prev = i + 1;
		}
	}
	while ((int)out.size() < expected) out.emplace_back();
	return out;
}

void CtrlBreakpointList::ImportBreakpoints() {
	std::string path;
	if (!W32Util::BrowseForFileName(true, GetHandle(), L"Import breakpoints",
			nullptr, nullptr,
			L"Breakpoint files (*.bp)\0*.bp\0Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0",
			L"bp", path)) {
		return;
	}
	int mode = MessageBox(GetHandle(),
		L"Replace existing breakpoints?\n\n"
		L"Yes = clear current breakpoints (exec + memory), then load.\n"
		L"No  = merge — keep existing, add new from file.\n"
		L"Cancel = abort.",
		L"Import breakpoints", MB_YESNOCANCEL | MB_ICONQUESTION);
	if (mode == IDCANCEL) return;

	std::ifstream in(path);
	if (!in) {
		MessageBox(GetHandle(), L"Failed to open file.", L"Import breakpoints",
			MB_OK | MB_ICONERROR);
		return;
	}

	if (mode == IDYES) {
		g_breakpoints.ClearAllBreakPoints();
		g_breakpoints.ClearAllMemChecks();
	}

	int loadedEx = 0, loadedMem = 0, rejected = 0;
	std::string line;
	while (std::getline(in, line)) {
		// Trim trailing \r (Windows line endings on Linux read).
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (line.empty() || line[0] == '#') continue;

		// First field is type.
		size_t tab = line.find('\t');
		if (tab == std::string::npos) { rejected++; continue; }
		std::string type = line.substr(0, tab);

		if (type == "EX") {
			auto f = SplitTabs(line, 6);
			// f[0]=EX, f[1]=addr, f[2]=enabled, f[3]=log, f[4]=cond, f[5]=fmt
			u32 addr = 0;
			try { addr = (u32)std::stoul(f[1], nullptr, 0); }
			catch (...) { rejected++; continue; }
			bool enabled = (!f[2].empty() && f[2] != "0");
			bool log     = (!f[3].empty() && f[3] != "0");
			g_breakpoints.AddBreakPoint(addr, false);
			BreakAction action = BREAK_ACTION_IGNORE;
			if (enabled) action |= BREAK_ACTION_PAUSE;
			if (log)     action |= BREAK_ACTION_LOG;
			g_breakpoints.ChangeBreakPoint(addr, action);
			if (!f[4].empty()) {
				BreakPointCond cond;
				cond.debug = cpu;
				cond.expressionString = f[4];
				if (initExpression(cpu, f[4].c_str(), cond.expression)) {
					g_breakpoints.ChangeBreakPointAddCond(addr, cond);
				}
			}
			if (!f[5].empty()) g_breakpoints.ChangeBreakPointLogFormat(addr, f[5]);
			loadedEx++;
		} else if (type == "MEM") {
			auto f = SplitTabs(line, 8);
			// f[0]=MEM, f[1]=start, f[2]=end, f[3]=cond(R/W/RW/RWC),
			// f[4]=enabled, f[5]=log, f[6]=condexpr, f[7]=fmt
			u32 start = 0, end = 0;
			try {
				start = (u32)std::stoul(f[1], nullptr, 0);
				end   = (u32)std::stoul(f[2], nullptr, 0);
			} catch (...) { rejected++; continue; }
			MemCheckCondition cond = MEMCHECK_READ;
			if (f[3] == "R")        cond = MEMCHECK_READ;
			else if (f[3] == "W")   cond = MEMCHECK_WRITE;
			else if (f[3] == "RW")  cond = MEMCHECK_READWRITE;
			else if (f[3] == "RWC") cond = (MemCheckCondition)(MEMCHECK_READWRITE | MEMCHECK_WRITE_ONCHANGE);
			else { rejected++; continue; }
			bool enabled = (!f[4].empty() && f[4] != "0");
			bool log     = (!f[5].empty() && f[5] != "0");
			BreakAction action = BREAK_ACTION_IGNORE;
			if (enabled) action |= BREAK_ACTION_PAUSE;
			if (log)     action |= BREAK_ACTION_LOG;
			g_breakpoints.AddMemCheck(start, end, cond, action);
			if (!f[6].empty()) {
				BreakPointCond bpc;
				bpc.debug = cpu;
				bpc.expressionString = f[6];
				if (initExpression(cpu, f[6].c_str(), bpc.expression)) {
					g_breakpoints.ChangeMemCheckAddCond(start, end, bpc);
				}
			}
			if (!f[7].empty()) g_breakpoints.ChangeMemCheckLogFormat(start, end, f[7]);
			loadedMem++;
		} else {
			rejected++;
		}
	}

	reloadBreakpoints();
	wchar_t msg[256];
	swprintf_s(msg, L"Loaded %d exec + %d memory breakpoint(s).%s",
		loadedEx, loadedMem,
		rejected ? (std::wstring(L"\nRejected ") + std::to_wstring(rejected) + L" invalid line(s).").c_str() : L"");
	MessageBox(GetHandle(), msg, L"Import breakpoints", MB_OK | MB_ICONINFORMATION);
}

void CtrlBreakpointList::ExportBreakpoints() {
	auto bps  = g_breakpoints.GetBreakpoints();
	auto mcs  = g_breakpoints.GetMemChecks();
	if (bps.empty() && mcs.empty()) {
		MessageBox(GetHandle(), L"No breakpoints to export.", L"Export breakpoints",
			MB_OK | MB_ICONINFORMATION);
		return;
	}
	std::string path;
	if (!W32Util::BrowseForFileName(false, GetHandle(), L"Export breakpoints",
			nullptr, L"breakpoints.bp",
			L"Breakpoint files (*.bp)\0*.bp\0All files (*.*)\0*.*\0\0",
			L"bp", path)) {
		return;
	}
	std::ofstream out(path, std::ios::trunc);
	if (!out) {
		MessageBox(GetHandle(), L"Failed to open file for write.", L"Export breakpoints",
			MB_OK | MB_ICONERROR);
		return;
	}
	out << "# PPSSPP breakpoints. Tab-separated. # = comment.\n";
	out << "# EX  <addr>  <enabled:0|1>  <log:0|1>  <cond_expr>  <log_fmt>\n";
	out << "# MEM <start> <end>  <cond:R|W|RW|RWC>  <enabled:0|1>  <log:0|1>  <cond_expr>  <log_fmt>\n";
	for (const auto &bp : bps) {
		if (bp.temporary) continue;
		char addrBuf[16];
		snprintf(addrBuf, sizeof(addrBuf), "0x%08X", bp.addr);
		bool enabled = (bp.result & BREAK_ACTION_PAUSE) != 0;
		bool log     = (bp.result & BREAK_ACTION_LOG) != 0;
		std::string cond = bp.hasCond ? EscapeForBPField(bp.cond.expressionString) : std::string();
		std::string fmt  = EscapeForBPField(bp.logFormat);
		out << "EX\t" << addrBuf << '\t' << (enabled ? 1 : 0) << '\t'
		    << (log ? 1 : 0) << '\t' << cond << '\t' << fmt << '\n';
	}
	for (const auto &mc : mcs) {
		char startBuf[16], endBuf[16];
		snprintf(startBuf, sizeof(startBuf), "0x%08X", mc.start);
		snprintf(endBuf,   sizeof(endBuf),   "0x%08X", mc.end);
		bool enabled = (mc.result & BREAK_ACTION_PAUSE) != 0;
		bool log     = (mc.result & BREAK_ACTION_LOG) != 0;
		const char *condStr =
			(mc.cond & MEMCHECK_WRITE_ONCHANGE) ? "RWC" :
			(mc.cond == MEMCHECK_READWRITE) ? "RW" :
			(mc.cond == MEMCHECK_WRITE) ? "W" : "R";
		std::string cond = mc.hasCondition ? EscapeForBPField(mc.condition.expressionString) : std::string();
		std::string fmt  = EscapeForBPField(mc.logFormat);
		out << "MEM\t" << startBuf << '\t' << endBuf << '\t' << condStr << '\t'
		    << (enabled ? 1 : 0) << '\t' << (log ? 1 : 0) << '\t'
		    << cond << '\t' << fmt << '\n';
	}
}

void CtrlBreakpointList::ClearAllBreakpoints() {
	if (MessageBox(GetHandle(),
			L"Remove all exec + memory breakpoints?", L"Clear All",
			MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
		return;
	}
	g_breakpoints.ClearAllBreakPoints();
	g_breakpoints.ClearAllMemChecks();
	reloadBreakpoints();
}

//
// CtrlStackTraceView
//

CtrlStackTraceView::CtrlStackTraceView(HWND hwnd, DebugInterface* cpu, CtrlDisAsmView* disasm)
	: GenericListControl(hwnd,stackTraceListDef),cpu(cpu),disasm(disasm)
{
	Update();
}

bool CtrlStackTraceView::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& returnValue)
{
	switch(msg)
	{
	case WM_KEYDOWN:
		if (wParam == VK_TAB)
		{
			returnValue = 0;
			SendMessage(GetParent(GetHandle()),WM_DEB_TABPRESSED,0,0);
			return true;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN)
		{
			if (wParam == VK_TAB || wParam == VK_RETURN)
			{
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	}

	return false;
}

void CtrlStackTraceView::GetColumnText(wchar_t* dest, size_t destSize, int row, int col)
{
	// We should have emptied the list if g_symbolMap is nullptr, but apparently we don't,
	// so let's have a sanity check here.
	if (row < 0 || row >= (int)frames.size() || !g_symbolMap) {
		return;
	}

	switch (col)
	{
	case SF_ENTRY:
		wsprintf(dest,L"%08X",frames[row].entry);
		break;
	case SF_ENTRYNAME:
		{
			const std::string sym = g_symbolMap->GetLabelString(frames[row].entry);
			if (!sym.empty()) {
				wcscpy(dest, ConvertUTF8ToWString(sym).c_str());
			} else {
				wcscpy(dest,L"-");
			}
		}
		break;
	case SF_CURPC:
		wsprintf(dest,L"%08X",frames[row].pc);
		break;
	case SF_CUROPCODE:
		{
			char temp[512];
			disasm->getOpcodeText(frames[row].pc, temp, sizeof(temp));
			wcscpy(dest, ConvertUTF8ToWString(temp).c_str());
		}
		break;
	case SF_CURSP:
		wsprintf(dest,L"%08X",frames[row].sp);
		break;
	case SF_FRAMESIZE:
		wsprintf(dest,L"%08X",frames[row].stackSize);
		break;
	}
}

void CtrlStackTraceView::OnDoubleClick(int itemIndex, int column)
{
	SendMessage(GetParent(GetHandle()),WM_DEB_GOTOWPARAM,frames[itemIndex].pc,0);
}

void CtrlStackTraceView::loadStackTrace() {
	auto memLock = Memory::Lock();
	if (!PSP_IsInited())
		return;

	auto threads = GetThreadsInfo();

	u32 entry = 0, stackTop = 0;
	for (size_t i = 0; i < threads.size(); i++)
	{
		if (threads[i].isCurrent)
		{
			entry = threads[i].entrypoint;
			stackTop = threads[i].initialStack;
			break;
		}
	}

	if (entry != 0) {
		frames = MIPSStackWalk::Walk(cpu->GetPC(),cpu->GetRegValue(0,31),cpu->GetRegValue(0,29),entry,stackTop);
	} else {
		frames.clear();
	}
	Update();
}

//
// CtrlModuleList
//

CtrlModuleList::CtrlModuleList(HWND hwnd, DebugInterface* cpu)
	: GenericListControl(hwnd,moduleListDef),cpu(cpu)
{
	Update();
}

bool CtrlModuleList::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& returnValue)
{
	switch(msg)
	{
	case WM_KEYDOWN:
		if (wParam == VK_TAB)
		{
			returnValue = 0;
			SendMessage(GetParent(GetHandle()),WM_DEB_TABPRESSED,0,0);
			return true;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG*)lParam)->message == WM_KEYDOWN)
		{
			if (wParam == VK_TAB || wParam == VK_RETURN)
			{
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	}

	return false;
}

void CtrlModuleList::GetColumnText(wchar_t* dest, size_t destSize, int row, int col)
{
	if (row < 0 || row >= (int)modules.size()) {
		return;
	}

	switch (col) {
	case ML_NAME:
		ConvertUTF8ToWString(dest, destSize, modules[row].name);
		break;
	case ML_ADDRESS:
		wsprintf(dest,L"%08X",modules[row].address);
		break;
	case ML_SIZE:
		wsprintf(dest,L"%08X",modules[row].size);
		break;
	case ML_ACTIVE:
		wcscpy(dest,modules[row].active ? L"true" : L"false");
		break;
	}
}

void CtrlModuleList::OnDoubleClick(int itemIndex, int column)
{
	SendMessage(GetParent(GetHandle()),WM_DEB_GOTOWPARAM,modules[itemIndex].address,0);
}

void CtrlModuleList::loadModules()
{
	if (g_symbolMap) {
		modules = g_symbolMap->getAllModules();
	} else {
		modules.clear();
	}
	Update();
}

// In case you modify things in the memory view.
static constexpr UINT_PTR IDT_CHECK_REFRESH = 0xC0DE0044;

CtrlWatchList::CtrlWatchList(HWND hwnd, DebugInterface *cpu)
	: GenericListControl(hwnd, watchListDef), cpu_(cpu) {
	SetSendInvalidRows(true);
	Update();

	SetTimer(GetHandle(), IDT_CHECK_REFRESH, 1000U, nullptr);
}

void CtrlWatchList::RefreshValues() {
	int steppingCounter = Core_GetSteppingCounter();
	int changes = false;
	for (auto &watch : watches_) {
		if (watch.steppingCounter != steppingCounter) {
			watch.lastValue = watch.currentValue;
			watch.steppingCounter = steppingCounter;
			changes = true;
		}

		uint32_t prevValue = watch.currentValue;
		watch.evaluateFailed = !parseExpression(cpu_, watch.expression, watch.currentValue);
		if (prevValue != watch.currentValue)
			changes = true;
	}

	if (changes)
		Update();
}

bool CtrlWatchList::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) {
	switch (msg) {
	case WM_KEYDOWN:
		switch (wParam) {
		case VK_TAB:
			returnValue = 0;
			SendMessage(GetParent(GetHandle()), WM_DEB_TABPRESSED, 0, 0);
			return true;
		case VK_RETURN:
			returnValue = 0;
			EditWatch(GetSelectedIndex());
			return true;
		case VK_DELETE:
			returnValue = 0;
			DeleteWatch(GetSelectedIndex());
			return true;
		default:
			break;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG *)lParam)->message == WM_KEYDOWN) {
			if (wParam == VK_TAB || wParam == VK_RETURN || wParam == VK_DELETE) {
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	case WM_TIMER:
		if (wParam == IDT_CHECK_REFRESH) {
			RefreshValues();
			return true;
		}
		break;
	}

	return false;
}

void CtrlWatchList::GetColumnText(wchar_t *dest, size_t destSize, int row, int col) {
	const auto &watch = watches_[row];
	switch (col) {
	case WL_NAME:
		wcsncpy(dest, ConvertUTF8ToWString(watch.name).c_str(), 255);
		dest[255] = 0;
		break;
	case WL_EXPRESSION:
		wcsncpy(dest, ConvertUTF8ToWString(watch.originalExpression).c_str(), 255);
		dest[255] = 0;
		break;
	case WL_VALUE:
		if (watch.evaluateFailed) {
			wcscpy(dest, L"(failed to evaluate)");
		} else {
			const uint32_t &value = watch.currentValue;
			float valuef = 0.0f;
			switch (watch.format) {
			case WatchFormat::HEX:
				wsprintf(dest, L"0x%08X", value);
				break;
			case WatchFormat::INT:
				wsprintf(dest, L"%d", (int32_t)value);
				break;
			case WatchFormat::FLOAT:
				memcpy(&valuef, &value, sizeof(valuef));
				swprintf_s(dest, destSize, L"%f", valuef);
				break;
			case WatchFormat::STR:
				if (Memory::IsValidAddress(value)) {
					uint32_t len = Memory::ClampValidSizeAt(value, 255);
					swprintf_s(dest, destSize, L"%.*S", len, Memory::GetCharPointer(value));
				} else {
					wsprintf(dest, L"(0x%08X)", value);
				}
				break;
			}
		}
		break;
	}
}

void CtrlWatchList::OnRightClick(int itemIndex, int column, const POINT &pt) {
	if (itemIndex == -1) {
		switch (TriggerContextMenu(ContextMenuID::CPUADDWATCH, GetHandle(), ContextPoint::FromClient(pt))) {
		case ID_DISASM_ADDNEWBREAKPOINT:
			AddWatch();
			break;
		}
	} else {
		switch (TriggerContextMenu(ContextMenuID::CPUWATCHLIST, GetHandle(), ContextPoint::FromClient(pt))) {
		case ID_DISASM_EDITBREAKPOINT:
			EditWatch(itemIndex);
			break;
		case ID_DISASM_DELETEBREAKPOINT:
			DeleteWatch(itemIndex);
			break;
		case ID_DISASM_ADDNEWBREAKPOINT:
			AddWatch();
			break;
		}
	}
}

bool CtrlWatchList::OnRowPrePaint(int row, LPNMLVCUSTOMDRAW msg) {
	if (row >= 0 && HasWatchChanged(row)) {
		msg->clrText = RGB(255, 0, 0);
		return true;
	}
	return false;
}

void CtrlWatchList::AddWatch() {
	WatchItemWindow win(nullptr, GetHandle(), cpu_);
	if (win.Exec()) {
		WatchInfo info;
		if (initExpression(cpu_, win.GetExpression().c_str(), info.expression)) {
			info.name = win.GetName();
			info.originalExpression = win.GetExpression();
			info.format = win.GetFormat();
			watches_.push_back(info);
			RefreshValues();
		} else {
			char errorMessage[512];
			snprintf(errorMessage, sizeof(errorMessage), "Invalid expression \"%s\": %s", win.GetExpression().c_str(), getExpressionError());
			MessageBoxA(GetHandle(), errorMessage, "Error", MB_OK);
		}
	}
}

void CtrlWatchList::EditWatch(int pos) {
	auto &watch = watches_[pos];
	WatchItemWindow win(nullptr, GetHandle(), cpu_);
	win.Init(watch.name, watch.originalExpression, watch.format);
	if (win.Exec()) {
		if (initExpression(cpu_, win.GetExpression().c_str(), watch.expression)) {
			watch.name = win.GetName();
			watch.originalExpression = win.GetExpression();
			watch.format = win.GetFormat();
			RefreshValues();
		} else {
			char errorMessage[512];
			snprintf(errorMessage, sizeof(errorMessage), "Invalid expression \"%s\": %s", win.GetExpression().c_str(), getExpressionError());
			MessageBoxA(GetHandle(), errorMessage, "Error", MB_OK);
		}
	}
}

void CtrlWatchList::DeleteWatch(int pos) {
	watches_.erase(watches_.begin() + pos);
	Update();
}

bool CtrlWatchList::HasWatchChanged(int pos) {
	return watches_[pos].lastValue != watches_[pos].currentValue;
}

//
// CtrlUserSymbolList
//

// Symbol-name validity check shared with the disasm-view "Add Symbol Here"
// flow (CtrlDisAsmView.cpp). Keep these two in sync — the rules exist so
// the assembler can disambiguate `0x...` and `pos_0x...` raw-address tokens
// from real identifiers without ambiguity.
static bool IsValidUserSymbolName(const std::string &name) {
	if (name.empty()) return false;
	if (isdigit((unsigned char)name[0])) return false;
	if (name.size() >= 2 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X')) return false;
	if (name.size() >= 6 && name.compare(0, 6, "pos_0x") == 0) return false;
	for (char c : name) {
		if (!isalnum((unsigned char)c) && c != '_') return false;
	}
	return true;
}

CtrlUserSymbolList::CtrlUserSymbolList(HWND hwnd, DebugInterface *cpu)
	: GenericListControl(hwnd, userSymListDef), cpu_(cpu) {
	// Forward right-clicks on the empty area (iItem == -1) to OnRightClick so the
	// "Add Symbol..." context menu appears, same as CtrlBreakpointList.
	SetSendInvalidRows(true);
	Refresh();
}

void CtrlUserSymbolList::Refresh() {
	symbols_.clear();
	if (g_symbolMap) {
		g_symbolMap->GetUserLabels(symbols_);
		std::sort(symbols_.begin(), symbols_.end(),
			[](const SymbolEntry &a, const SymbolEntry &b) { return a.address < b.address; });
	}
	Update();
}

bool CtrlUserSymbolList::WindowMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT &returnValue) {
	switch (msg) {
	case WM_KEYDOWN:
		switch (wParam) {
		case VK_TAB:
			returnValue = 0;
			SendMessage(GetParent(GetHandle()), WM_DEB_TABPRESSED, 0, 0);
			return true;
		case VK_RETURN:
			returnValue = 0;
			JumpTo(GetSelectedIndex());
			return true;
		case VK_DELETE:
			returnValue = 0;
			Delete(GetSelectedIndex());
			return true;
		}
		break;
	case WM_GETDLGCODE:
		if (lParam && ((MSG *)lParam)->message == WM_KEYDOWN) {
			if (wParam == VK_TAB || wParam == VK_RETURN || wParam == VK_DELETE) {
				returnValue = DLGC_WANTMESSAGE;
				return true;
			}
		}
		break;
	}
	return false;
}

void CtrlUserSymbolList::GetColumnText(wchar_t *dest, size_t destSize, int row, int col) {
	if (row < 0 || row >= (int)symbols_.size()) {
		dest[0] = 0;
		return;
	}
	const auto &s = symbols_[row];
	switch (col) {
	case USL_ADDRESS:
		swprintf_s(dest, destSize, L"0x%08X", s.address);
		break;
	case USL_NAME:
		wcsncpy(dest, ConvertUTF8ToWString(s.name).c_str(), destSize - 1);
		dest[destSize - 1] = 0;
		break;
	}
}

void CtrlUserSymbolList::OnDoubleClick(int itemIndex, int column) {
	JumpTo(itemIndex);
}

void CtrlUserSymbolList::OnRightClick(int itemIndex, int column, const POINT &pt) {
	ContextMenuID which = (itemIndex == -1) ? ContextMenuID::USERSYMADD : ContextMenuID::USERSYMLIST;
	switch (TriggerContextMenu(which, GetHandle(), ContextPoint::FromClient(pt))) {
	case ID_DISASM_USERSYM_JUMP:     JumpTo(itemIndex); break;
	case ID_DISASM_USERSYM_EDIT:     Edit(itemIndex); break;
	case ID_DISASM_USERSYM_DELETE:   Delete(itemIndex); break;
	case ID_DISASM_USERSYM_COPYADDR: CopyAddress(itemIndex); break;
	case ID_DISASM_USERSYM_ADD:      AddNew(); break;
	case ID_DISASM_USERSYM_IMPORT:   Import(); break;
	case ID_DISASM_USERSYM_EXPORT:   Export(); break;
	case ID_DISASM_USERSYM_CLEAR:    ClearAll(); break;
	}
}

void CtrlUserSymbolList::JumpTo(int pos) {
	if (pos < 0 || pos >= (int)symbols_.size()) return;
	SendMessage(GetParent(GetHandle()), WM_DEB_GOTOWPARAM, symbols_[pos].address, 0);
}

void CtrlUserSymbolList::Edit(int pos) {
	if (pos < 0 || pos >= (int)symbols_.size()) return;
	const u32 addr = symbols_[pos].address;
	std::string oldName = symbols_[pos].name;
	std::string newName;
	if (!InputBox_GetString(MainWindow::GetHInstance(), GetHandle(),
			L"Rename symbol", oldName, newName)) {
		return;
	}
	if (!IsValidUserSymbolName(newName)) {
		MessageBox(GetHandle(),
			L"Invalid name. Alphanumerics + underscore, no leading digit, can't begin with 0x or pos_0x.",
			L"Edit Symbol", MB_OK | MB_ICONWARNING);
		return;
	}
	g_symbolMap->SetLabelName(newName.c_str(), addr);
	g_symbolMap->MarkLabelAsUser(addr);
	Refresh();
	SendMessage(GetParent(GetHandle()), WM_DEB_MAPLOADED, 0, 0);
}

void CtrlUserSymbolList::Delete(int pos) {
	if (pos < 0 || pos >= (int)symbols_.size()) return;
	g_symbolMap->RemoveLabel(symbols_[pos].address);
	Refresh();
	SendMessage(GetParent(GetHandle()), WM_DEB_MAPLOADED, 0, 0);
}

void CtrlUserSymbolList::CopyAddress(int pos) {
	if (pos < 0 || pos >= (int)symbols_.size()) return;
	char buf[16];
	snprintf(buf, sizeof(buf), "0x%08X", symbols_[pos].address);
	W32Util::CopyTextToClipboard(GetHandle(), buf);
}

void CtrlUserSymbolList::AddNew() {
	std::string addrStr;
	if (!InputBox_GetString(MainWindow::GetHInstance(), GetHandle(),
			L"Address (hex, e.g. 0x08800100)", "0x", addrStr)) {
		return;
	}
	u32 addr = 0;
	if (!parseExpression(addrStr.c_str(), cpu_, addr)) {
		MessageBox(GetHandle(), L"Invalid address.", L"Add Symbol", MB_OK | MB_ICONWARNING);
		return;
	}
	std::string name;
	char def[32];
	snprintf(def, sizeof(def), "label_%08X", addr);
	if (!InputBox_GetString(MainWindow::GetHInstance(), GetHandle(),
			L"Symbol name", def, name)) {
		return;
	}
	if (!IsValidUserSymbolName(name)) {
		MessageBox(GetHandle(),
			L"Invalid name. Alphanumerics + underscore, no leading digit, can't begin with 0x or pos_0x.",
			L"Add Symbol", MB_OK | MB_ICONWARNING);
		return;
	}
	g_symbolMap->AddLabel(name.c_str(), addr);
	g_symbolMap->SetLabelName(name.c_str(), addr);
	g_symbolMap->MarkLabelAsUser(addr);
	Refresh();
	SendMessage(GetParent(GetHandle()), WM_DEB_MAPLOADED, 0, 0);
}

// File format: one symbol per line as `0xADDR NAME`. Lines starting with
// `#`, `;`, or `//` are comments (skipped); blank lines are also ignored.
// Names are validated against IsValidUserSymbolName — invalid lines are
// counted in the rejected total and reported in the summary.
void CtrlUserSymbolList::Import() {
	std::string path;
	if (!W32Util::BrowseForFileName(true, GetHandle(), L"Import user symbols",
			nullptr, nullptr,
			L"Symbol files (*.sym)\0*.sym\0Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0",
			L"sym", path)) {
		return;
	}

	// Ask: Replace existing or Merge? IDYES=replace, IDNO=merge, IDCANCEL=abort.
	int mode = MessageBox(GetHandle(),
		L"Replace existing user symbols?\n\n"
		L"Yes = clear current user symbols, then load.\n"
		L"No  = merge into current user symbols (new entries overwrite by address).\n"
		L"Cancel = abort.",
		L"Import .sym", MB_YESNOCANCEL | MB_ICONQUESTION);
	if (mode == IDCANCEL) return;

	std::ifstream in(path);
	if (!in) {
		MessageBox(GetHandle(), L"Failed to open file.", L"Import .sym", MB_OK | MB_ICONERROR);
		return;
	}

	if (mode == IDYES) {
		g_symbolMap->ClearUserLabels();
	}

	int loaded = 0;
	int rejected = 0;
	std::string line;
	while (std::getline(in, line)) {
		// Strip leading whitespace + comment.
		size_t s = 0;
		while (s < line.size() && (line[s] == ' ' || line[s] == '\t' || line[s] == '\r')) s++;
		if (s >= line.size()) continue;
		if (line[s] == '#' || line[s] == ';') continue;
		if (s + 1 < line.size() && line[s] == '/' && line[s+1] == '/') continue;

		// Parse address.
		u32 addr = 0;
		size_t consumed = 0;
		try {
			addr = (u32)std::stoul(line.substr(s), &consumed, 0);
		} catch (...) {
			rejected++;
			continue;
		}
		// Skip whitespace before name.
		size_t n = s + consumed;
		while (n < line.size() && (line[n] == ' ' || line[n] == '\t')) n++;
		// Trim trailing whitespace from name.
		size_t end = line.find_last_not_of(" \t\r\n");
		if (end == std::string::npos || end < n) {
			rejected++;
			continue;
		}
		std::string name = line.substr(n, end - n + 1);
		if (!IsValidUserSymbolName(name)) {
			rejected++;
			continue;
		}
		g_symbolMap->AddLabel(name.c_str(), addr);
		g_symbolMap->SetLabelName(name.c_str(), addr);
		g_symbolMap->MarkLabelAsUser(addr);
		loaded++;
	}

	Refresh();
	SendMessage(GetParent(GetHandle()), WM_DEB_MAPLOADED, 0, 0);

	wchar_t msg[256];
	swprintf_s(msg, L"Loaded %d symbol(s).%s",
		loaded, rejected ? (std::wstring(L"\nRejected ") + std::to_wstring(rejected) + L" invalid line(s).").c_str() : L"");
	MessageBox(GetHandle(), msg, L"Import .sym", MB_OK | MB_ICONINFORMATION);
}

void CtrlUserSymbolList::Export() {
	if (symbols_.empty()) {
		MessageBox(GetHandle(), L"No user symbols to export.", L"Export .sym",
			MB_OK | MB_ICONINFORMATION);
		return;
	}
	std::string path;
	if (!W32Util::BrowseForFileName(false, GetHandle(), L"Export user symbols",
			nullptr, L"usersymbols.sym",
			L"Symbol files (*.sym)\0*.sym\0All files (*.*)\0*.*\0\0",
			L"sym", path)) {
		return;
	}
	std::ofstream out(path, std::ios::trunc);
	if (!out) {
		MessageBox(GetHandle(), L"Failed to open file for write.", L"Export .sym",
			MB_OK | MB_ICONERROR);
		return;
	}
	out << "# PPSSPP user symbols. One symbol per line: <0xADDR> <NAME>\n";
	for (const auto &s : symbols_) {
		char buf[64];
		snprintf(buf, sizeof(buf), "0x%08X ", s.address);
		out << buf << s.name << '\n';
	}
}

void CtrlUserSymbolList::ClearAll() {
	if (symbols_.empty()) return;
	if (MessageBox(GetHandle(), L"Remove all user-defined symbols?", L"Clear All",
			MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
		return;
	}
	g_symbolMap->ClearUserLabels();
	Refresh();
	SendMessage(GetParent(GetHandle()), WM_DEB_MAPLOADED, 0, 0);
}
