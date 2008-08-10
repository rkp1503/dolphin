// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "Debugger.h"

#include "RegisterWindow.h"
#include "LogWindow.h"
#include "BreakpointWindow.h"
#include "MemoryWindow.h"
#include "IniFile.h"

#include "wx/button.h"
#include "wx/textctrl.h"
#include "wx/listctrl.h"
#include "wx/thread.h"
#include "wx/listctrl.h"
#include "wx/mstream.h"

#include "CodeWindow.h"
#include "CodeView.h"
#include "HW/CPU.h"
#include "PowerPC/PowerPC.h"
#include "Host.h"


#include "Debugger/PPCDebugInterface.h"
#include "Debugger/Debugger_SymbolMap.h"
#include "PowerPC/PPCAnalyst.h"

#include "Core.h"
#include "LogManager.h"

// ugly that this lib included code from the main
#include "../../DolphinWX/src/Globals.h"

extern "C" {
	#include "../resources/toolbar_play.c"
	#include "../resources/toolbar_pause.c"
	#include "../resources/toolbar_add_memorycheck.c"
	#include "../resources/toolbar_delete.c"
	#include "../resources/toolbar_add_breakpoint.c"
}

static const long TOOLBAR_STYLE = wxTB_FLAT | wxTB_DOCKABLE | wxTB_TEXT;

BEGIN_EVENT_TABLE(CCodeWindow, wxFrame)   
    EVT_LISTBOX(IDM_SYMBOLLIST,     CCodeWindow::OnSymbolListChange)
    EVT_LISTBOX(IDM_CALLSTACKLIST,  CCodeWindow::OnCallstackListChange)
    EVT_HOST_COMMAND(wxID_ANY,      CCodeWindow::OnHostMessage)
    EVT_MENU(IDM_LOGWINDOW,         CCodeWindow::OnToggleLogWindow)
    EVT_MENU(IDM_REGISTERWINDOW,    CCodeWindow::OnToggleRegisterWindow)
    EVT_MENU(IDM_BREAKPOINTWINDOW,  CCodeWindow::OnToggleBreakPointWindow)
    EVT_MENU(IDM_MEMORYWINDOW,      CCodeWindow::OnToggleMemoryWindow)

	EVT_MENU(IDM_SCANFUNCTIONS,     CCodeWindow::OnSymbolsMenu)
	EVT_MENU(IDM_LOADMAPFILE,       CCodeWindow::OnSymbolsMenu)
	EVT_MENU(IDM_SAVEMAPFILE,       CCodeWindow::OnSymbolsMenu)
	// toolbar
	EVT_MENU(IDM_DEBUG_GO,			CCodeWindow::OnCodeStep)
	EVT_MENU(IDM_STEP,				CCodeWindow::OnCodeStep)
	EVT_MENU(IDM_STEPOVER,			CCodeWindow::OnCodeStep)
	EVT_MENU(IDM_SKIP,				CCodeWindow::OnCodeStep)
	EVT_MENU(IDM_SETPC,				CCodeWindow::OnCodeStep)
	EVT_MENU(IDM_GOTOPC,			CCodeWindow::OnCodeStep)
	EVT_TEXT(IDM_ADDRBOX,           CCodeWindow::OnAddrBoxChange)
END_EVENT_TABLE()

#define wxGetBitmapFromMemory(name) _wxGetBitmapFromMemory(name, sizeof(name))
inline wxBitmap _wxGetBitmapFromMemory(const unsigned char* data, int length)
{
	wxMemoryInputStream is(data, length);
	return(wxBitmap(wxImage(is, wxBITMAP_TYPE_ANY, -1), -1));
}


CCodeWindow::CCodeWindow(const SCoreStartupParameter& _LocalCoreStartupParameter, wxWindow* parent, wxWindowID id,
		const wxString& title, const wxPoint& pos, const wxSize& size, long style)
	: wxFrame(parent, id, title, pos, size, style)
	, m_RegisterWindow(NULL)
	, m_LogWindow(NULL)
{    
	InitBitmaps();

	CreateGUIControls(_LocalCoreStartupParameter);

	// Create the toolbar
	RecreateToolbar();

	UpdateButtonStates();

	wxTheApp->Connect(wxID_ANY, wxEVT_KEY_DOWN,
		wxKeyEventHandler(CCodeWindow::OnKeyDown),
		(wxObject*)0, this);

	// load ini...
	IniFile file;
	file.Load("Debugger.ini");

	this->Load(file);
	if (m_BreakpointWindow) m_BreakpointWindow->Load(file);
	if (m_LogWindow) m_LogWindow->Load(file);
	if (m_RegisterWindow) m_RegisterWindow->Load(file);
	if (m_MemoryWindow) m_MemoryWindow->Load(file);
}


CCodeWindow::~CCodeWindow()
{
	IniFile file;
	file.Load("Debugger.ini");

	this->Save(file);
	if (m_BreakpointWindow) m_BreakpointWindow->Save(file);
	if (m_LogWindow) m_LogWindow->Save(file);
	if (m_RegisterWindow) m_RegisterWindow->Save(file);
	if (m_MemoryWindow) m_MemoryWindow->Save(file);

	file.Save("Debugger.ini");
}


void CCodeWindow::Load( IniFile &file )
{
	int x,y,w,h;
	file.Get("Code", "x", &x, GetPosition().x);
	file.Get("Code", "y", &y, GetPosition().y);
	file.Get("Code", "w", &w, GetSize().GetWidth());
	file.Get("Code", "h", &h, GetSize().GetHeight());
	this->SetSize(x, y, w, h);
}


void CCodeWindow::Save(IniFile &file) const
{
	file.Set("Code", "x", GetPosition().x);
	file.Set("Code", "y", GetPosition().y);
	file.Set("Code", "w", GetSize().GetWidth());
	file.Set("Code", "h", GetSize().GetHeight());
}


void CCodeWindow::CreateGUIControls(const SCoreStartupParameter& _LocalCoreStartupParameter)
{
	CreateMenu(_LocalCoreStartupParameter);

	wxBoxSizer* sizerBig   = new wxBoxSizer(wxHORIZONTAL);
	wxBoxSizer* sizerLeft  = new wxBoxSizer(wxVERTICAL);

	DebugInterface* di = new PPCDebugInterface();

	sizerLeft->Add(callstack = new wxListBox(this, IDM_CALLSTACKLIST, wxDefaultPosition, wxSize(90, 100)), 0, wxEXPAND);
	sizerLeft->Add(symbols = new wxListBox(this, IDM_SYMBOLLIST, wxDefaultPosition, wxSize(90, 100), 0, NULL, wxLB_SORT), 1, wxEXPAND);
	codeview = new CCodeView(di, this, wxID_ANY);
	sizerBig->Add(sizerLeft, 2, wxEXPAND);
	sizerBig->Add(codeview, 5, wxEXPAND);

	SetSizer(sizerBig);

	sizerLeft->SetSizeHints(this);
	sizerLeft->Fit(this);
	sizerBig->SetSizeHints(this);
	sizerBig->Fit(this);

	sync_event.Init();

	// additional dialogs
	if (IsLoggingActivated())
	{
		m_LogWindow = new CLogWindow(this);
		m_LogWindow->Show(true);
	}

	m_RegisterWindow = new CRegisterWindow(this);
	m_RegisterWindow->Show(true);

	m_BreakpointWindow = new CBreakPointWindow(this, this);
	m_BreakpointWindow->Show(true);

	m_MemoryWindow = new CMemoryWindow(this);
	m_MemoryWindow->Show(true);
}


void CCodeWindow::CreateMenu(const SCoreStartupParameter& _LocalCoreStartupParameter)
{
	wxMenuBar* pMenuBar = new wxMenuBar(wxMB_DOCKABLE);

	{
		wxMenu* pCoreMenu = new wxMenu;
		wxMenuItem* interpreter = pCoreMenu->Append(IDM_INTERPRETER, _T("&Interpreter"), wxEmptyString, wxITEM_CHECK);
		interpreter->Check(!_LocalCoreStartupParameter.bUseDynarec);

//		wxMenuItem* dualcore = pDebugMenu->Append(IDM_DUALCORE, _T("&DualCore"), wxEmptyString, wxITEM_CHECK);
//		dualcore->Check(_LocalCoreStartupParameter.bUseDualCore);

		pMenuBar->Append(pCoreMenu, _T("&Core Startup"));
	}

	{
		wxMenu* pDebugDialogs = new wxMenu;

		if (IsLoggingActivated())
		{
			wxMenuItem* pLogWindow = pDebugDialogs->Append(IDM_LOGWINDOW, _T("&LogManager"), wxEmptyString, wxITEM_CHECK);
			pLogWindow->Check(true);
		}

		wxMenuItem* pRegister = pDebugDialogs->Append(IDM_REGISTERWINDOW, _T("&Registers"), wxEmptyString, wxITEM_CHECK);
		pRegister->Check(true);

		wxMenuItem* pBreakPoints = pDebugDialogs->Append(IDM_BREAKPOINTWINDOW, _T("&BreakPoints"), wxEmptyString, wxITEM_CHECK);
		pBreakPoints->Check(true);

		wxMenuItem* pMemory = pDebugDialogs->Append(IDM_MEMORYWINDOW, _T("&Memory"), wxEmptyString, wxITEM_CHECK);
		pMemory->Check(true);
		pMenuBar->Append(pDebugDialogs, _T("&Views"));
	}

	{
		wxMenu *pSymbolsMenu = new wxMenu;
		pSymbolsMenu->Append(IDM_SCANFUNCTIONS, _T("&Scan for functions"));
		pMenuBar->Append(pSymbolsMenu, _T("&Symbols"));
	}
	SetMenuBar(pMenuBar);
}


bool CCodeWindow::UseInterpreter()
{
	return(GetMenuBar()->IsChecked(IDM_INTERPRETER));
}


bool CCodeWindow::UseDualCore()
{
	return(GetMenuBar()->IsChecked(IDM_DUALCORE));
}


void CCodeWindow::JumpToAddress(u32 _Address)
{
    codeview->Center(_Address);
}

void CCodeWindow::OnSymbolsMenu(wxCommandEvent& event) 
{
	if (Core::GetState() == Core::CORE_UNINITIALIZED)
	{
		// TODO: disable menu items instead :P
		return;
	}
	wxString path;
	switch (event.GetId())
	{
	case IDM_SCANFUNCTIONS:
		PPCAnalyst::FindFunctions(0x80003100, 0x80400000);
		PPCAnalyst::LoadFuncDB("Data/totaldb.dsy");
		Debugger::GetFromAnalyzer();
		NotifyMapLoaded();
		break;
	case IDM_LOADMAPFILE:
		path = wxFileSelector(
				_T("Select the mapfile to load"),
				wxEmptyString, wxEmptyString, wxEmptyString,
				wxString::Format
				(
						_T("Map files (*.map)|*.map|All files (%s)|%s"),
						wxFileSelectorDefaultWildcardStr,
						wxFileSelectorDefaultWildcardStr
				),
				wxFD_OPEN | wxFD_FILE_MUST_EXIST,
				this);
		if (!path)
		{
			return;
		}
		Debugger::LoadSymbolMap(path.c_str());
		break;
	case IDM_SAVEMAPFILE:
		path = wxFileSelector(
				_T("Name your mapfile"),
				wxEmptyString, wxEmptyString, wxEmptyString,
				wxString::Format
				(
						_T("Map files (*.map)|*.map|All files (%s)|%s"),
						wxFileSelectorDefaultWildcardStr,
						wxFileSelectorDefaultWildcardStr
				),
				wxFD_SAVE,
				this);
		if (!path)
		{
			return;
		}
		Debugger::SaveSymbolMap(path.c_str());
		break;
	}
}

void CCodeWindow::OnCodeStep(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	    case IDM_DEBUG_GO:
	    {
		    // [F|RES] prolly we should disable the other buttons in go mode too ...
		    codeview->Center(PC);

		    if (CCPU::IsStepping())
		    {
			    CCPU::EnableStepping(false);
		    }
		    else
		    {
			    CCPU::EnableStepping(true);
			    Host_UpdateLogDisplay();
		    }

		    Update();
	    }
		    break;

	    case IDM_STEP:
			SingleCPUStep();

		    break;

	    case IDM_STEPOVER:
		    CCPU::EnableStepping(true);
		    break;

	    case IDM_SKIP:
		    PC += 4;
		    Update();
		    break;

	    case IDM_SETPC:
		    PC = codeview->GetSelection();
		    Update();
		    break;

	    case IDM_GOTOPC:
		    codeview->Center(PC);
		    break;
	}

	UpdateButtonStates();
}


void CCodeWindow::OnAddrBoxChange(wxCommandEvent& event)
{
	wxTextCtrl* pAddrCtrl = (wxTextCtrl*)GetToolBar()->FindControl(IDM_ADDRBOX);
	wxString txt = pAddrCtrl->GetValue();

	if (txt.size() == 8)
	{
		u32 addr;
		sscanf(txt.mb_str(), "%08x", &addr);
		codeview->Center(addr);
	}

	event.Skip(1);
}


void CCodeWindow::Update()
{
	codeview->Refresh();

	callstack->Clear();

	std::vector<Debugger::SCallstackEntry>stack;

	if (Debugger::GetCallstack(stack))
	{
		for (size_t i = 0; i < stack.size(); i++)
		{
			int idx = callstack->Append(wxString::FromAscii(stack[i].Name.c_str()));
			callstack->SetClientData(idx, (void*)(u64)stack[i].vAddress);
		}
	}
	else
	{
		callstack->Append(wxString::FromAscii("invalid callstack"));
	}

	UpdateButtonStates();

	codeview->Center(PC);

	Host_UpdateLogDisplay();
}


void CCodeWindow::NotifyMapLoaded()
{
	symbols->Show(false); // hide it for faster filling
	symbols->Clear();
#ifdef _WIN32
	const Debugger::XVectorSymbol& syms = Debugger::AccessSymbols();

	for (int i = 0; i < (int)syms.size(); i++)
	{
		int idx = symbols->Append(syms[i].GetName().c_str());
		symbols->SetClientData(idx, (void*)&syms[i]);
	}

	//
#endif

	symbols->Show(true);
	Update();
}


void CCodeWindow::UpdateButtonStates()
{
	wxToolBar* toolBar = GetToolBar();
	if (Core::GetState() == Core::CORE_UNINITIALIZED)
	{
		toolBar->EnableTool(IDM_DEBUG_GO, false);
		toolBar->EnableTool(IDM_STEP, false);
		toolBar->EnableTool(IDM_STEPOVER, false);
		toolBar->EnableTool(IDM_SKIP, false);
	}
	else
	{
		if (!CCPU::IsStepping())
		{
			toolBar->SetToolShortHelp(IDM_DEBUG_GO, _T("&Pause"));
			toolBar->SetToolNormalBitmap(IDM_DEBUG_GO, m_Bitmaps[Toolbar_Pause]);
			toolBar->EnableTool(IDM_DEBUG_GO, true);
			toolBar->EnableTool(IDM_STEP, false);
			toolBar->EnableTool(IDM_STEPOVER, false);
			toolBar->EnableTool(IDM_SKIP, false);
		}
		else
		{
			toolBar->SetToolShortHelp(IDM_DEBUG_GO, _T("&Play"));
			toolBar->SetToolNormalBitmap(IDM_DEBUG_GO, m_Bitmaps[Toolbar_DebugGo]);
			toolBar->EnableTool(IDM_DEBUG_GO, true);
			toolBar->EnableTool(IDM_STEP, true);
			toolBar->EnableTool(IDM_STEPOVER, true);
			toolBar->EnableTool(IDM_SKIP, true);
		}
	}
}


void CCodeWindow::OnSymbolListChange(wxCommandEvent& event)
{
	int index = symbols->GetSelection();
	Debugger::CSymbol* pSymbol = static_cast<Debugger::CSymbol*>(symbols->GetClientData(index));

	if (pSymbol != NULL)
	{
		codeview->Center(pSymbol->vaddress);
	}
}

void CCodeWindow::OnSymbolListContextMenu(wxContextMenuEvent& event)
{
	int index = symbols->GetSelection();
}


void CCodeWindow::OnCallstackListChange(wxCommandEvent& event)
{
	int index   = callstack->GetSelection();
	u32 address = (u32)(u64)(callstack->GetClientData(index));

	if (address != 0x00)
	{
		codeview->Center(address);
	}
}


void CCodeWindow::OnToggleLogWindow(wxCommandEvent& event)
{
	if (IsLoggingActivated())
	{
		bool show = GetMenuBar()->IsChecked(event.GetId());

		if (show)
		{
			if (!m_LogWindow)
			{
				m_LogWindow = new CLogWindow(this);
			}

			m_LogWindow->Show(true);
		}
		else // hide
		{
			// If m_dialog is NULL, then possibly the system
			// didn't report the checked menu item status correctly.
			// It should be true just after the menu item was selected,
			// if there was no modeless dialog yet.
			wxASSERT(m_LogWindow != NULL);

			if (m_LogWindow)
			{
				m_LogWindow->Hide();
			}
		}
	}
}


void CCodeWindow::OnToggleRegisterWindow(wxCommandEvent& event)
{
	bool show = GetMenuBar()->IsChecked(event.GetId());

	if (show)
	{
		if (!m_RegisterWindow)
		{
			m_RegisterWindow = new CRegisterWindow(this);
		}

		m_RegisterWindow->Show(true);
	}
	else // hide
	{
		// If m_dialog is NULL, then possibly the system
		// didn't report the checked menu item status correctly.
		// It should be true just after the menu item was selected,
		// if there was no modeless dialog yet.
		wxASSERT(m_RegisterWindow != NULL);

		if (m_RegisterWindow)
		{
			m_RegisterWindow->Hide();
		}
	}
}

void CCodeWindow::OnToggleBreakPointWindow(wxCommandEvent& event)
{
	bool show = GetMenuBar()->IsChecked(event.GetId());

	if (show)
	{
		if (!m_BreakpointWindow)
		{
			m_BreakpointWindow = new CBreakPointWindow(this, this);
		}

		m_BreakpointWindow->Show(true);
	}
	else // hide
	{
		// If m_dialog is NULL, then possibly the system
		// didn't report the checked menu item status correctly.
		// It should be true just after the menu item was selected,
		// if there was no modeless dialog yet.
		wxASSERT(m_BreakpointWindow != NULL);

		if (m_BreakpointWindow)
		{
			m_BreakpointWindow->Hide();
		}
	}
}

void CCodeWindow::OnToggleMemoryWindow(wxCommandEvent& event)
{
	bool show = GetMenuBar()->IsChecked(event.GetId());

	if (show)
	{
		if (!m_MemoryWindow)
		{
			m_MemoryWindow = new CMemoryWindow(this);
		}

		m_MemoryWindow->Show(true);
	}
	else // hide
	{
		// If m_dialog is NULL, then possibly the system
		// didn't report the checked menu item status correctly.
		// It should be true just after the menu item was selected,
		// if there was no modeless dialog yet.
		wxASSERT(m_MemoryWindow != NULL);

		if (m_MemoryWindow)
		{
			m_MemoryWindow->Hide();
		}
	}
}

void CCodeWindow::OnHostMessage(wxCommandEvent& event)
{
	switch (event.GetId())
	{
	    case IDM_NOTIFYMAPLOADED:
		    NotifyMapLoaded();
		    break;

	    case IDM_UPDATELOGDISPLAY:

		    if (m_LogWindow)
		    {
			    m_LogWindow->NotifyUpdate();
		    }

		    break;

	    case IDM_UPDATEDISASMDIALOG:
		    Update();

		    if (m_RegisterWindow)
		    {
			    m_RegisterWindow->NotifyUpdate();
		    }

		    break;

		case IDM_UPDATEBREAKPOINTS:
            Update();

			if (m_BreakpointWindow)
			{
				m_BreakpointWindow->NotifyUpdate();
			}
			break;

	}
}

void CCodeWindow::PopulateToolbar(wxToolBar* toolBar)
{
	int w = m_Bitmaps[Toolbar_DebugGo].GetWidth(),
		h = m_Bitmaps[Toolbar_DebugGo].GetHeight();

	toolBar->SetToolBitmapSize(wxSize(w, h));
	toolBar->AddTool(IDM_DEBUG_GO,	_T("Play"),			m_Bitmaps[Toolbar_DebugGo]);
	toolBar->AddTool(IDM_STEP,		_T("Step"),			m_Bitmaps[Toolbar_Step]);
	toolBar->AddTool(IDM_STEPOVER,	_T("Step Over"),    m_Bitmaps[Toolbar_StepOver]);
	toolBar->AddTool(IDM_SKIP,		_T("Skip"),			m_Bitmaps[Toolbar_Skip]);
	toolBar->AddSeparator();
	toolBar->AddTool(IDM_GOTOPC,    _T("Goto PC"),		m_Bitmaps[Toolbar_GotoPC]);
	toolBar->AddTool(IDM_SETPC,		_T("Set PC"),		m_Bitmaps[Toolbar_SetPC]);
	toolBar->AddSeparator();
	toolBar->AddControl(new wxTextCtrl(toolBar, IDM_ADDRBOX, _T("")));

	// after adding the buttons to the toolbar, must call Realize() to reflect
	// the changes
	toolBar->Realize();
}


void CCodeWindow::RecreateToolbar()
{
	// delete and recreate the toolbar
	wxToolBarBase* toolBar = GetToolBar();
	delete toolBar;
	SetToolBar(NULL);

	long style = TOOLBAR_STYLE;
	style &= ~(wxTB_HORIZONTAL | wxTB_VERTICAL | wxTB_BOTTOM | wxTB_RIGHT | wxTB_HORZ_LAYOUT | wxTB_TOP);
	wxToolBar* theToolBar = CreateToolBar(style, ID_TOOLBAR);

	PopulateToolbar(theToolBar);
	SetToolBar(theToolBar);
}


void CCodeWindow::InitBitmaps()
{
	// load original size 48x48
	m_Bitmaps[Toolbar_DebugGo] = wxGetBitmapFromMemory(toolbar_play_png);
	m_Bitmaps[Toolbar_Step] = wxGetBitmapFromMemory(toolbar_add_breakpoint_png);
	m_Bitmaps[Toolbar_StepOver] = wxGetBitmapFromMemory(toolbar_add_memcheck_png);
	m_Bitmaps[Toolbar_Skip] = wxGetBitmapFromMemory(toolbar_add_memcheck_png);
	m_Bitmaps[Toolbar_GotoPC] = wxGetBitmapFromMemory(toolbar_add_memcheck_png);
	m_Bitmaps[Toolbar_SetPC] = wxGetBitmapFromMemory(toolbar_add_memcheck_png);
	m_Bitmaps[Toolbar_Pause] = wxGetBitmapFromMemory(toolbar_pause_png);


	// scale to 16x16 for toolbar
	for (size_t n = Toolbar_DebugGo; n < Bitmaps_max; n++)
	{
		m_Bitmaps[n] = wxBitmap(m_Bitmaps[n].ConvertToImage().Scale(16, 16));
	}
}


void CCodeWindow::OnKeyDown(wxKeyEvent& event)
{
	if ((event.GetKeyCode() == WXK_SPACE) && IsActive())
	{
		SingleCPUStep();	
	}
	else
	{
		event.Skip();
	}
}


void CCodeWindow::SingleCPUStep()
{
	CCPU::StepOpcode(&sync_event);
	//            if (CCPU::IsStepping())
	//	            sync_event.Wait();
	wxThread::Sleep(20);
	// need a short wait here
	codeview->Center(PC);
	Update();
	Host_UpdateLogDisplay();
}