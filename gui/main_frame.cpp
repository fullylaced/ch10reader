// gui/main_frame.cpp
#include "main_frame.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <wx/artprov.h>
#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/progdlg.h>

#include <fstream>

#include "ch10/ops.h"
#include "ch10/userdata.h"
#include "ch10/wordmap.h"
#include "theme.h"
#include "tool_dialogs.h"

using ch10::Session;

namespace {

std::filesystem::path ToPath(const wxString& s) {
    return std::filesystem::path(s.ToStdWstring());
}
wxString Utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

wxString HumanBytes(uint64_t b) {
    const char* unit[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(b);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? wxString::Format("%llu B", static_cast<unsigned long long>(b))
                  : wxString::Format("%.2f %s", v, unit[u]);
}

wxString Duration(double s) {
    int ms = static_cast<int>((s - static_cast<int>(s)) * 1000);
    int t = static_cast<int>(s);
    return wxString::Format("%02d:%02d:%02d.%03d", t / 3600, (t / 60) % 60, t % 60, ms);
}

class FileDrop : public wxFileDropTarget {
public:
    explicit FileDrop(MainFrame* f) : frame_(f) {}
    bool OnDropFiles(wxCoord, wxCoord, const wxArrayString& files) override {
        if (!files.IsEmpty()) frame_->OpenFile(files[0]);
        return true;
    }
private:
    MainFrame* frame_;
};

} // namespace

// ===========================================================================
// Construction
// ===========================================================================
MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "IRIG 106 Chapter 10 Workbench",
              wxDefaultPosition, wxSize(1500, 940)) {
    mgr_.SetManagedWindow(this);
    mgr_.SetFlags(mgr_.GetFlags() | wxAUI_MGR_LIVE_RESIZE);

    BuildMenu();
    BuildToolbar();
    CreateStatusBar(2);
    int widths[] = {-3, -1};
    GetStatusBar()->SetStatusWidths(2, widths);
    SetStatusText("Open a Chapter 10 recording to begin");

    recording_pane_ = BuildRecordingPane();
    mgr_.AddPane(recording_pane_, wxAuiPaneInfo().Name("recording")
                 .Caption("Recording").Top().Layer(1).CloseButton(true)
                 .CaptionVisible(true).MinSize(-1, 84).BestSize(-1, 84)
                 .Resizable(false));

    mgr_.AddPane(BuildCenter(), wxAuiPaneInfo().Name("center").CenterPane());

    video_pane_ = BuildVideoPane();
    mgr_.AddPane(video_pane_, wxAuiPaneInfo().Name("video").Caption("Video")
                 .Right().Layer(1).BestSize(560, 400).MinSize(320, 200)
                 .CloseButton(true).MaximizeButton(true));

    playback_pane_ = BuildPlaybackPane();
    mgr_.AddPane(playback_pane_, wxAuiPaneInfo().Name("playback")
                 .Caption("Playback and data flow").Bottom().Layer(2)
                 .BestSize(-1, 320).MinSize(-1, 160).CloseButton(true));

    audio_.Start(&controller_);
    audio_panel_ = new AudioPanel(this, &audio_);
    audio_panel_->cursor = [this] { return controller_.cursor(); };
    audio_panel_->range = [this](uint64_t& a, uint64_t& b) {
        if (!RangeValid()) return false;
        a = in_rtc_; b = out_rtc_;
        return true;
    };
    audio_panel_->on_status = [this](const wxString& st) { SetStatusText(st); };
    mgr_.AddPane(audio_panel_, wxAuiPaneInfo().Name("audio").Caption("Audio")
                 .Right().Layer(1).Position(1).BestSize(560, 380).MinSize(320, 340)
                 .CloseButton(true));

    mgr_.Update();
    default_perspective_ = mgr_.SavePerspective();

    SetDropTarget(new FileDrop(this));
    controller_.AddListener([this](uint64_t rtc, bool playing) { OnCursor(rtc, playing); });

    Bind(wxEVT_MENU, &MainFrame::OnMenuOpen,     this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::OnExportTmats,  this, ID_EXPORT_TMATS);
    Bind(wxEVT_MENU, &MainFrame::OnExport1553,   this, ID_EXPORT_1553);
    Bind(wxEVT_MENU, &MainFrame::OnExtractVideo, this, ID_EXTRACT_VIDEO);
    Bind(wxEVT_MENU, &MainFrame::OnPlay,         this, ID_PLAY);
    Bind(wxEVT_MENU, &MainFrame::OnStop,         this, ID_STOP);
    Bind(wxEVT_MENU, &MainFrame::OnRewind,       this, ID_REWIND);
    Bind(wxEVT_MENU, &MainFrame::OnForward,      this, ID_FORWARD);
    Bind(wxEVT_MENU, &MainFrame::OnStepBack,     this, ID_STEP_BACK);
    Bind(wxEVT_MENU, &MainFrame::OnStepFwd,      this, ID_STEP_FWD);
    Bind(wxEVT_MENU, &MainFrame::OnJumpStart,    this, ID_JUMP_START);
    Bind(wxEVT_MENU, &MainFrame::OnJumpEnd,      this, ID_JUMP_END);
    Bind(wxEVT_MENU, &MainFrame::OnViewPane,     this, ID_VIEW_RECORDING, ID_VIEW_RESET);
    Bind(wxEVT_MENU, &MainFrame::OnSpeedUp,      this, ID_SPEED_UP);
    Bind(wxEVT_MENU, &MainFrame::OnSpeedDown,    this, ID_SPEED_DOWN);
    Bind(wxEVT_MENU, &MainFrame::OnTheme,        this, ID_THEME_FIRST, ID_THEME_LAST);
    Bind(wxEVT_MENU, &MainFrame::OnSetIn,        this, ID_SET_IN);
    Bind(wxEVT_MENU, &MainFrame::OnSetOut,       this, ID_SET_OUT);
    Bind(wxEVT_MENU, &MainFrame::OnClearRange,   this, ID_CLEAR_RANGE);
    Bind(wxEVT_MENU, &MainFrame::OnAddAnnotation, this, ID_ADD_ANNOTATION);
    Bind(wxEVT_MENU, &MainFrame::OnToolCut,      this, ID_TOOL_CUT);
    Bind(wxEVT_MENU, &MainFrame::OnToolSplit,    this, ID_TOOL_SPLIT);
    Bind(wxEVT_MENU, &MainFrame::OnToolMerge,    this, ID_TOOL_MERGE);
    Bind(wxEVT_MENU, &MainFrame::OnToolSaveAnnotations, this, ID_TOOL_SAVE_ANNOTATIONS);
    Bind(wxEVT_MENU, &MainFrame::OnToolInject, this, ID_TOOL_INJECT);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ethernet_->ExportPcap(); }, ID_TOOL_PCAP);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ms1553_->LoadWordMap(); }, ID_TOOL_WORDMAP);
    Bind(wxEVT_MENU, &MainFrame::OnToolWordMapTemplate, this, ID_TOOL_WORDMAP_TEMPLATE);
    Bind(wxEVT_MENU, &MainFrame::OnAbout,        this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::OnExit,         this, wxID_EXIT);
    Bind(wxEVT_TOOL, &MainFrame::OnMenuOpen,     this, wxID_OPEN);
    Bind(wxEVT_TOOL, &MainFrame::OnExportTmats,  this, ID_EXPORT_TMATS);
    Bind(wxEVT_TOOL, &MainFrame::OnExport1553,   this, ID_EXPORT_1553);
    Bind(wxEVT_TOOL, &MainFrame::OnExtractVideo, this, ID_EXTRACT_VIDEO);

    // Plain-key shortcuts ([ ] I O Space): a char hook sees them before any
    // child control does, on every platform; typing in text boxes is left alone.
    Bind(wxEVT_CHAR_HOOK, &MainFrame::OnCharHook, this);

    ClearAll();
    ApplyCurrentTheme();
    Centre();
}

MainFrame::~MainFrame() {
    controller_.Pause();
    audio_.Stop();
    mgr_.UnInit();
}

void MainFrame::BuildMenu() {
    auto* file = new wxMenu;
    file->Append(wxID_OPEN, "&Open recording...\tCtrl+O");
    file->AppendSeparator();
    file->Append(ID_EXPORT_TMATS,  "Export &TMATS...");
    file->Append(ID_EXPORT_1553,   "Export 1553 &CSV...");
    file->Append(ID_EXTRACT_VIDEO, "Extract &video stream...");
    file->AppendSeparator();
    file->Append(wxID_EXIT, "E&xit\tAlt+F4");

    auto* play = new wxMenu;
    play->Append(ID_PLAY,       "&Play / Pause   (F5, Space)");
    play->Append(ID_STOP,       "&Stop\tF6");
    play->Append(ID_REWIND,     "&Rewind\tF7");
    play->Append(ID_FORWARD,    "&Fast forward\tF8");
    play->AppendSeparator();
    play->Append(ID_STEP_BACK,  "Step &back 1 s\tCtrl+Left");
    play->Append(ID_STEP_FWD,   "Step &forward 1 s\tCtrl+Right");
    play->Append(ID_JUMP_START, "Jump to &start\tHome");
    play->Append(ID_JUMP_END,   "Jump to &end\tEnd");

    play->AppendSeparator();
    play->Append(ID_SET_IN,      "Set &In point at cursor   (I)");
    play->Append(ID_SET_OUT,     "Set &Out point at cursor   (O)");
    play->Append(ID_CLEAR_RANGE, "Clear In/Out");
    play->AppendSeparator();
    play->Append(ID_ADD_ANNOTATION, "Add &annotation at cursor...\tCtrl+M");

    auto* tools = new wxMenu;
    tools->Append(ID_TOOL_CUT,   "&Cut / filter to new file...\tCtrl+T");
    tools->Append(ID_TOOL_SPLIT, "&Split into parts...");
    tools->Append(ID_TOOL_MERGE, "&Merge recordings...");
    tools->AppendSeparator();
    tools->Append(ID_TOOL_SAVE_ANNOTATIONS, "Save &annotations into a file copy...");
    tools->Append(ID_TOOL_INJECT, "Add &user data from text file...");
    tools->AppendSeparator();
    tools->Append(ID_TOOL_PCAP, "Export Ethernet to &PCAP...");
    tools->Append(ID_TOOL_WORDMAP, "Load 1553 &word map...");
    tools->Append(ID_TOOL_WORDMAP_TEMPLATE, "Write word map &template...");

    auto* view = new wxMenu;
    view->Append(ID_VIEW_RECORDING, "Show &Recording pane");
    view->Append(ID_VIEW_VIDEO,     "Show &Video pane");
    view->Append(ID_VIEW_PLAYBACK,  "Show &Playback pane");
    view->Append(ID_VIEW_AUDIO,     "Show &Audio pane");
    view->AppendSeparator();
    view->Append(ID_VIEW_RESET,     "Reset &layout");
    view->AppendSeparator();
    auto* themes = new wxMenu;
    int tid = ID_THEME_FIRST;
    for (const Theme& t : Themes()) {
        themes->AppendRadioItem(tid, t.name);
        if (t.name == CurrentTheme().name) themes->Check(tid, true);
        ++tid;
    }
    view->AppendSubMenu(themes, "&Theme");

    auto* help = new wxMenu;
    help->Append(wxID_ABOUT, "&About");

    auto* bar = new wxMenuBar;
    bar->Append(file, "&File");
    bar->Append(play, "&Playback");
    bar->Append(tools, "&Tools");
    bar->Append(view, "&View");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
}

void MainFrame::BuildToolbar() {
    wxToolBar* tb = CreateToolBar(wxTB_HORIZONTAL | wxTB_FLAT | wxTB_TEXT);
    wxSize isz(24, 24);
    tb->SetToolBitmapSize(isz);
    tb->AddTool(wxID_OPEN, "Open", wxArtProvider::GetBitmap(wxART_FILE_OPEN, wxART_TOOLBAR, isz),
                "Open a Chapter 10 recording");
    tb->AddSeparator();
    path_ctrl_ = new wxTextCtrl(tb, ID_OPEN_PATH, "", wxDefaultPosition,
                                wxSize(560, -1), wxTE_PROCESS_ENTER);
    path_ctrl_->SetHint("Path to .ch10 file - press Enter, or drop a file anywhere on the window");
    tb->AddControl(path_ctrl_);
    tb->AddSeparator();
    tb->AddTool(ID_EXPORT_TMATS, "TMATS", wxArtProvider::GetBitmap(wxART_FILE_SAVE_AS, wxART_TOOLBAR, isz),
                "Export the TMATS setup record");
    tb->AddTool(ID_EXPORT_1553, "1553 CSV", wxArtProvider::GetBitmap(wxART_REPORT_VIEW, wxART_TOOLBAR, isz),
                "Export 1553 messages to CSV");
    tb->AddTool(ID_EXTRACT_VIDEO, "Video", wxArtProvider::GetBitmap(wxART_HARDDISK, wxART_TOOLBAR, isz),
                "Extract a video channel to .ts/.mpg");
    tb->Realize();
    path_ctrl_->Bind(wxEVT_TEXT_ENTER, &MainFrame::OnOpenPath, this);
}

wxPanel* MainFrame::BuildRecordingPane() {
    auto* p = new wxPanel(this);
    auto* grid = new wxFlexGridSizer(8, 8, 18);
    for (int c = 1; c < 8; c += 2) grid->AddGrowableCol(c, 1);

    const char* labels[] = {"File", "Recording start", "Time source", "TMATS program",
                            "Size", "Recording end",   "Time packets", "Channels",
                            "Packets", "Duration"};
    wxFont bold = p->GetFont().Bold();
    // Row-major fill of a 2-row x 4-pair grid, plus a third row for two pairs.
    int order[] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i : order) {
        auto* l = new wxStaticText(p, wxID_ANY, labels[i]);
        l->SetFont(bold);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        rec_values_[i] = new wxStaticText(p, wxID_ANY, "-");
        grid->Add(rec_values_[i], 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    }
    for (int i = 8; i < 10; ++i) {
        auto* l = new wxStaticText(p, wxID_ANY, labels[i]);
        l->SetFont(bold);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        rec_values_[i] = new wxStaticText(p, wxID_ANY, "-");
        grid->Add(rec_values_[i], 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
    }
    auto* box = new wxBoxSizer(wxVERTICAL);
    box->Add(grid, 1, wxEXPAND | wxALL, 8);
    p->SetSizer(box);
    return p;
}

wxWindow* MainFrame::BuildCenter() {
    notebook_ = new wxAuiNotebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_SCROLL_BUTTONS |
                                  wxAUI_NB_WINDOWLIST_BUTTON);
    packets_ = new PacketsPanel(notebook_);
    ms1553_  = new Ms1553Panel(notebook_);
    ms1553_->on_status = [this](const wxString& s) { SetStatusText(s); };
    annotations_ = new AnnotationsPanel(notebook_);
    annotations_->cursor = [this] { return controller_.cursor(); };
    annotations_->range  = [this](uint64_t& a, uint64_t& b) {
        if (!RangeValid()) return false;
        a = in_rtc_; b = out_rtc_;
        return true;
    };
    annotations_->on_seek = [this](uint64_t rtc) { controller_.Seek(rtc); };
    annotations_->on_changed = [this] { timeline_->MarkersChanged(); };
    annotations_->on_save_request = [this] { wxCommandEvent e; OnToolSaveAnnotations(e); };
    annotations_->on_status = [this](const wxString& st) { SetStatusText(st); };
    ethernet_ = new EthernetPanel(notebook_);
    ethernet_->on_status = [this](const wxString& st) { SetStatusText(st); };
    messages_ = new MessagesPanel(notebook_);
    messages_->on_status = [this](const wxString& st) { SetStatusText(st); };
    notebook_->AddPage(packets_, "Packets");
    notebook_->AddPage(ms1553_, "MIL-STD-1553");
    notebook_->AddPage(ethernet_, "Ethernet");
    notebook_->AddPage(messages_, "Messages");
    notebook_->AddPage(annotations_, "Annotations");
    notebook_->AddPage(BuildTmatsPage(notebook_), "TMATS");
    notebook_->AddPage(BuildChannelsPage(notebook_), "Channels");
    return notebook_;
}

wxPanel* MainFrame::BuildTmatsPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* box = new wxBoxSizer(wxVERTICAL);
    tmats_text_ = new wxTextCtrl(page, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                 wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    tmats_text_->SetFont(wxFont(wxFontInfo().Family(wxFONTFAMILY_TELETYPE)));
    box->Add(tmats_text_, 1, wxEXPAND | wxALL, 6);
    page->SetSizer(box);
    return page;
}

wxPanel* MainFrame::BuildChannelsPage(wxWindow* parent) {
    auto* page = new wxPanel(parent);
    auto* box = new wxBoxSizer(wxVERTICAL);
    channel_list_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES);
    channel_list_->AppendColumn("Channel", wxLIST_FORMAT_RIGHT, 70);
    channel_list_->AppendColumn("Data type", wxLIST_FORMAT_LEFT, 300);
    channel_list_->AppendColumn("Packets", wxLIST_FORMAT_RIGHT, 90);
    channel_list_->AppendColumn("Size", wxLIST_FORMAT_RIGHT, 90);
    channel_list_->AppendColumn("Messages / MPEG pkts", wxLIST_FORMAT_RIGHT, 150);
    channel_list_->AppendColumn("TMATS source", wxLIST_FORMAT_LEFT, 220);
    channel_list_->AppendColumn("TMATS type", wxLIST_FORMAT_LEFT, 100);
    box->Add(channel_list_, 1, wxEXPAND | wxALL, 6);
    page->SetSizer(box);
    return page;
}

wxScrolledWindow* MainFrame::BuildVideoPane() {
    auto* w = new wxScrolledWindow(this, wxID_ANY);
    w->SetBackgroundColour(wxColour(22, 22, 26));
    w->SetScrollRate(0, 10);
    video_sizer_ = new wxBoxSizer(wxVERTICAL);
    video_hint_ = new wxStaticText(w, wxID_ANY, "Video channels appear here when a recording is open");
    video_hint_->SetForegroundColour(wxColour(150, 154, 164));
    video_sizer_->Add(video_hint_, 0, wxALIGN_CENTER | wxALL, 30);
    w->SetSizer(video_sizer_);
    return w;
}

wxPanel* MainFrame::BuildPlaybackPane() {
    auto* p = new wxPanel(this);
    auto* box = new wxBoxSizer(wxVERTICAL);

    // --- transport row
    auto* bar = new wxBoxSizer(wxHORIZONTAL);
    auto add = [&](int id, const wxString& label, const wxString& tip, int w = 44) {
        auto* b = new wxButton(p, id, label, wxDefaultPosition, wxSize(w, -1));
        b->SetToolTip(tip);
        bar->Add(b, 0, wxRIGHT, 4);
        return b;
    };
    add(ID_JUMP_START, "|<", "Jump to start (Home)");
    add(ID_STEP_BACK,  "-1s", "Step back one second (Ctrl+Left)");
    add(ID_REWIND,     "<<", "Rewind (F7)");
    play_btn_ = add(ID_PLAY, "Play", "Play / pause (F5)", 70);
    add(ID_STOP,       "Stop", "Stop and return to start (F6)", 56);
    add(ID_FORWARD,    ">>", "Play forward (F8)");
    add(ID_STEP_FWD,   "+1s", "Step forward one second (Ctrl+Right)");
    add(ID_JUMP_END,   ">|", "Jump to end (End)");
    bar->AddSpacer(12);
    add(ID_SET_IN,      "In",  "Set In point at cursor (I)", 40);
    add(ID_SET_OUT,     "Out", "Set Out point at cursor (O)", 44);
    add(ID_CLEAR_RANGE, "Clear", "Clear In/Out range", 56);
    range_label_ = new wxStaticText(p, wxID_ANY, "");
    bar->Add(range_label_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    bar->AddSpacer(8);
    bar->Add(new wxStaticText(p, wxID_ANY, "Speed"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    speed_ = new wxChoice(p, ID_SPEED);
    for (double s : PlaybackController::Speeds()) speed_->Append(wxString::Format("%gx", s));
    speed_->SetSelection(controller_.SpeedIndex());
    auto* slower = new wxButton(p, ID_SPEED_DOWN, "Slower", wxDefaultPosition, wxSize(64, -1));
    auto* faster = new wxButton(p, ID_SPEED_UP, "Faster", wxDefaultPosition, wxSize(64, -1));
    slower->SetToolTip("Slower ([)"); faster->SetToolTip("Faster (])");
    bar->Add(slower, 0, wxRIGHT, 2);
    bar->Add(speed_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
    bar->Add(faster, 0, wxRIGHT, 16);

    time_rel_ = new wxStaticText(p, wxID_ANY, "+00:00:00.000 / 00:00:00.000");
    time_rel_->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE).Bold()));
    bar->Add(time_rel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
    time_abs_ = new wxStaticText(p, wxID_ANY, "");
    time_abs_->SetFont(wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE)));
    bar->Add(time_abs_, 0, wxALIGN_CENTER_VERTICAL);
    box->Add(bar, 0, wxEXPAND | wxALL, 6);

    slider_ = new wxSlider(p, ID_SLIDER, 0, 0, 10000);
    box->Add(slider_, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

    timeline_ = new TimelinePanel(p);
    timeline_->on_seek = [this](uint64_t rtc) { controller_.Seek(rtc); };
    timeline_->on_marker = [this](const ch10::Annotation& a, bool) {
        notebook_->SetSelection(4);
        SetStatusText(wxString::FromUTF8(a.text.c_str()));
    };
    box->Add(timeline_, 1, wxEXPAND | wxALL, 6);
    p->SetSizer(box);

    p->Bind(wxEVT_BUTTON, &MainFrame::OnJumpStart, this, ID_JUMP_START);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnStepBack,  this, ID_STEP_BACK);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnRewind,    this, ID_REWIND);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnPlay,      this, ID_PLAY);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnStop,      this, ID_STOP);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnForward,   this, ID_FORWARD);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnStepFwd,   this, ID_STEP_FWD);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnJumpEnd,   this, ID_JUMP_END);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnSetIn,     this, ID_SET_IN);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnSetOut,    this, ID_SET_OUT);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnClearRange, this, ID_CLEAR_RANGE);
    p->Bind(wxEVT_CHOICE, &MainFrame::OnSpeed,     this, ID_SPEED);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnSpeedDown, this, ID_SPEED_DOWN);
    p->Bind(wxEVT_BUTTON, &MainFrame::OnSpeedUp,   this, ID_SPEED_UP);
    p->Bind(wxEVT_SLIDER, &MainFrame::OnSlider,    this, ID_SLIDER);
    return p;
}

// ===========================================================================
// Opening
// ===========================================================================
void MainFrame::OpenFile(const wxString& path) {
    controller_.Pause();
    path_ctrl_->SetValue(path);

    wxProgressDialog dlg("Scanning recording", "Indexing packets...", 1000, this,
                         wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
                         wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    bool ok = session_.Open(ToPath(path), [&dlg](uint64_t done, uint64_t total) {
        return dlg.Update(total ? static_cast<int>(std::min<uint64_t>(999, done * 1000 / total)) : 0,
                          wxString::Format("%s of %s", HumanBytes(done), HumanBytes(total)));
    });
    dlg.Update(1000);
    if (!ok) {
        if (session_.error() != "cancelled")
            wxMessageBox(Utf8(session_.error()), "Cannot open recording", wxOK | wxICON_ERROR, this);
        session_.Close();
        ClearAll();
        return;
    }
    RefreshAll();
}

void MainFrame::OnMenuOpen(wxCommandEvent&) {
    wxFileDialog dlg(this, "Open Chapter 10 recording", "", "",
                     "Chapter 10 files (*.ch10;*.c10)|*.ch10;*.c10|All files (*.*)|*.*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK) OpenFile(dlg.GetPath());
}

void MainFrame::OnOpenPath(wxCommandEvent&) {
    wxString p = path_ctrl_->GetValue().Trim().Trim(false);
    if (p.IsEmpty()) return;
    if (!wxFileName::FileExists(p)) {
        wxMessageBox("File not found:\n" + p, "Open", wxOK | wxICON_WARNING, this);
        return;
    }
    OpenFile(p);
}

// ===========================================================================
// Refresh
// ===========================================================================
void MainFrame::ClearAll() {
    for (auto* v : rec_values_) if (v) v->SetLabel("-");
    packets_->SetSession(nullptr);
    ms1553_->SetSession(nullptr);
    annotations_->SetSession(nullptr);
    ethernet_->SetSession(nullptr);
    messages_->SetSession(nullptr);
    if (audio_panel_) audio_panel_->SetSession(nullptr);
    in_set_ = out_set_ = false;
    UpdateRangeDisplay();
    tmats_text_->Clear();
    channel_list_->DeleteAllItems();
    timeline_->SetSession(nullptr);
    for (auto* v : video_panels_) v->Destroy();
    video_panels_.clear();
    video_hint_->Show(true);
    video_pane_->Layout();
    controller_.SetRange(0, 0);
    GetToolBar()->EnableTool(ID_EXPORT_TMATS, false);
    GetToolBar()->EnableTool(ID_EXPORT_1553, false);
    GetToolBar()->EnableTool(ID_EXTRACT_VIDEO, false);
    SetStatusText("No recording loaded");
    SetStatusText("", 1);
}

void MainFrame::RefreshAll() {
    RefreshRecordingPane();
    packets_->SetSession(&session_);
    ms1553_->SetSession(&session_);
    annotations_->SetSession(&session_);
    ethernet_->SetSession(&session_);
    messages_->SetSession(&session_);
    audio_panel_->SetSession(&session_);
    in_set_ = out_set_ = false;
    UpdateRangeDisplay();
    tmats_text_->SetValue(Utf8(session_.tmats().text()));
    RefreshChannelsPage();
    RefreshVideoPane();
    timeline_->SetSession(&session_);
    controller_.SetRange(session_.rtc_min(), session_.rtc_max());

    bool has_1553 = !session_.ChannelsOfType(ch10::kMilStd1553F1).empty();
    bool has_video = !video_panels_.empty() ||
                     !session_.ChannelsOfType(ch10::kVideoF0).empty() ||
                     !session_.ChannelsOfType(ch10::kVideoF1).empty();
    GetToolBar()->EnableTool(ID_EXPORT_TMATS, !session_.tmats().empty());
    GetToolBar()->EnableTool(ID_EXPORT_1553, has_1553);
    GetToolBar()->EnableTool(ID_EXTRACT_VIDEO, has_video);

    wxString st = wxString::Format("%s - %llu packets, %zu channels",
                                   wxFileName(path_ctrl_->GetValue()).GetFullName(),
                                   static_cast<unsigned long long>(session_.packet_count()),
                                   session_.channels().size());
    if (session_.resync_count())
        st += wxString::Format(" (%llu resyncs after corrupt headers)",
                               static_cast<unsigned long long>(session_.resync_count()));
    if (session_.anomaly_total())
        st += wxString::Format(" - %llu anomalies flagged",
                               static_cast<unsigned long long>(session_.anomaly_total()));
    SetStatusText(st);
    ApplyCurrentTheme();
    mgr_.Update();
}

void MainFrame::ApplyCurrentTheme() {
    ApplyTheme(this, &mgr_, notebook_);
    timeline_->Refresh();
    for (auto* v : video_panels_) v->Refresh();
}

void MainFrame::OnTheme(wxCommandEvent& ev) {
    int i = ev.GetId() - ID_THEME_FIRST;
    if (i < 0 || i >= static_cast<int>(Themes().size())) return;
    SetCurrentTheme(Themes()[static_cast<std::size_t>(i)].name);
    ApplyCurrentTheme();
#if defined(__WXMSW__) && wxCHECK_VERSION(3, 3, 0)
    SetStatusText("Theme saved; restart to switch the native title bar and menus", 1);
#endif
}

// ===========================================================================
// In/Out range
// ===========================================================================
void MainFrame::UpdateRangeDisplay() {
    if (!session_.IsOpen() || (!in_set_ && !out_set_)) {
        range_label_->SetLabel("");
        timeline_->SetRange(0, 0, false);
    } else {
        wxString l;
        if (in_set_)  l += "In " + Utf8(session_.RelativeString(in_rtc_));
        if (out_set_) l += (l.IsEmpty() ? "" : "  ") + wxString("Out ") + Utf8(session_.RelativeString(out_rtc_));
        if (RangeValid()) l += wxString::Format("  (%.3f s)", (out_rtc_ - in_rtc_) / 1e7);
        range_label_->SetLabel(l);
        timeline_->SetRange(in_set_ ? in_rtc_ : session_.rtc_min(),
                            out_set_ ? out_rtc_ : session_.rtc_max(), in_set_ || out_set_);
    }
    playback_pane_->Layout();
    annotations_->Rebuild();
}

void MainFrame::OnSetIn(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    in_rtc_ = controller_.cursor(); in_set_ = true;
    if (out_set_ && out_rtc_ <= in_rtc_) out_set_ = false;
    UpdateRangeDisplay();
}

void MainFrame::OnSetOut(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    out_rtc_ = controller_.cursor(); out_set_ = true;
    if (in_set_ && in_rtc_ >= out_rtc_) in_set_ = false;
    UpdateRangeDisplay();
}

void MainFrame::OnClearRange(wxCommandEvent&) {
    in_set_ = out_set_ = false;
    UpdateRangeDisplay();
}

// ===========================================================================
// Tools: cut / split / merge / save annotations
// ===========================================================================
void MainFrame::ReportOperation(const wxString& title, bool ok, const ch10::OpResult& r) {
    if (!ok) {
        if (r.error != "cancelled")
            wxMessageBox(Utf8(r.error), title, wxOK | wxICON_ERROR, this);
        return;
    }
    wxString msg = wxString::Format("%llu packets written (%s) to:\n",
                                    static_cast<unsigned long long>(r.packets_out),
                                    HumanBytes(r.bytes_out));
    for (const auto& o : r.outputs) msg += "  " + wxString(o.wstring()) + "\n";
    if (!r.warning.empty()) msg += "\n" + Utf8(r.warning);
    msg += "\nOpen the result now?";
    if (wxMessageBox(msg, title, wxYES_NO | wxICON_INFORMATION, this) == wxYES && !r.outputs.empty())
        OpenFile(wxString(r.outputs.front().wstring()));
}

void MainFrame::OnToolCut(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    controller_.Pause();
    auto user = session_.UserAnnotations();
    CutDialog dlg(this, session_, RangeValid(), in_rtc_, out_rtc_, user.size());
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Cutting", "Writing...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    ch10::OpResult r;
    bool ok = ch10::Cut(session_, dlg.Spec(), dlg.EmbedAnnotations() ? user : std::vector<ch10::Annotation>(),
                        dlg.Output(), r, [&pd](uint64_t d, uint64_t t) {
                            return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    ReportOperation("Cut / filter", ok, r);
}

void MainFrame::OnToolSplit(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    controller_.Pause();
    SplitDialog dlg(this, session_);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Splitting", "Writing parts...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    ch10::OpResult r;
    bool ok = ch10::Split(session_, dlg.Seconds(), dlg.MaxBytes(), dlg.Prefix(), r,
                          [&pd](uint64_t d, uint64_t t) {
                              return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    ReportOperation("Split", ok, r);
}

void MainFrame::OnToolMerge(wxCommandEvent&) {
    controller_.Pause();
    std::filesystem::path first = session_.IsOpen() ? session_.path() : std::filesystem::path();
    MergeDialog dlg(this, first);
    if (dlg.ShowModal() != wxID_OK) return;
    auto inputs = dlg.Inputs();
    if (inputs.size() < 2) {
        wxMessageBox("Add at least two recordings to merge.", "Merge", wxOK | wxICON_INFORMATION, this);
        return;
    }
    wxProgressDialog pd("Merging", "Writing...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    ch10::OpResult r;
    bool ok = ch10::Merge(inputs, dlg.Output(), r, [&pd](uint64_t d, uint64_t t) {
        return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    ReportOperation("Merge", ok, r);
}

void MainFrame::OnToolSaveAnnotations(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    auto user = session_.UserAnnotations();
    if (user.empty()) {
        wxMessageBox("There are no unsaved annotations. Add some from the Annotations tab first.",
                     "Save annotations", wxOK | wxICON_INFORMATION, this);
        return;
    }
    controller_.Pause();
    wxFileName base(path_ctrl_->GetValue());
    wxFileDialog dlg(this, "Save a copy of the recording with the annotations embedded",
                     base.GetPath(), base.GetName() + "_annotated.ch10",
                     "Chapter 10 files (*.ch10)|*.ch10", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    wxProgressDialog pd("Saving annotations", "Copying recording...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    ch10::OpResult r;
    bool ok = ch10::Cut(session_, ch10::FilterSpec(), user, ToPath(dlg.GetPath()), r,
                        [&pd](uint64_t d, uint64_t t) {
                            return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    ReportOperation("Save annotations", ok, r);
}

void MainFrame::RefreshRecordingPane() {
    const Session& s = session_;
    int n1553 = static_cast<int>(s.ChannelsOfType(ch10::kMilStd1553F1).size());
    int nvid  = static_cast<int>(s.ChannelsOfType(ch10::kVideoF0).size() +
                                 s.ChannelsOfType(ch10::kVideoF1).size());
    wxString tsrc = s.time().HasReference()
        ? wxString::Format("%s, %s", s.time().reference().FmtName(), s.time().reference().SrcName())
        : "none (raw RTC)";
    wxString prog = Utf8(s.tmats().Get("G\\PN"));
    if (!s.tmats().empty())
        prog += wxString::Format("%s(%s, %zu attrs)", prog.IsEmpty() ? "" : "  ",
                                 s.tmats().Ch10VersionName(), s.tmats().attributes().size());

    rec_values_[0]->SetLabel(wxFileName(path_ctrl_->GetValue()).GetFullName());
    rec_values_[1]->SetLabel(Utf8(s.RecordingStart()));
    rec_values_[2]->SetLabel(tsrc);
    rec_values_[3]->SetLabel(prog.IsEmpty() ? "-" : prog);
    rec_values_[4]->SetLabel(HumanBytes(s.file_size()));
    rec_values_[5]->SetLabel(Utf8(s.RecordingEnd()));
    rec_values_[6]->SetLabel(wxString::Format("%llu", static_cast<unsigned long long>(s.time_packet_count())));
    rec_values_[7]->SetLabel(wxString::Format("%zu  (1553: %d, video: %d)",
                                              s.channels().size(), n1553, nvid));
    rec_values_[8]->SetLabel(wxString::Format("%llu", static_cast<unsigned long long>(s.packet_count())));
    rec_values_[9]->SetLabel(Duration(s.duration_s()));
    recording_pane_->Layout();
}

void MainFrame::RefreshChannelsPage() {
    channel_list_->DeleteAllItems();
    long row = 0;
    for (const auto& c : session_.channels()) {
        long i = channel_list_->InsertItem(row++, wxString::Format("%d", c.channel_id));
        channel_list_->SetItem(i, 1, wxString::Format("0x%02X %s", c.data_type,
                                                      ch10::DataTypeName(c.data_type)));
        channel_list_->SetItem(i, 2, wxString::Format("%llu", static_cast<unsigned long long>(c.packets)));
        channel_list_->SetItem(i, 3, HumanBytes(c.bytes));
        channel_list_->SetItem(i, 4, c.messages ? wxString::Format("%llu",
                               static_cast<unsigned long long>(c.messages)) : wxString());
        channel_list_->SetItem(i, 5, Utf8(c.source));
        channel_list_->SetItem(i, 6, Utf8(c.tmats_type));
        channel_list_->SetItemTextColour(i, TypeColour(c.data_type).ChangeLightness(60));
    }
}

void MainFrame::RefreshVideoPane() {
    for (auto* v : video_panels_) v->Destroy();
    video_panels_.clear();
    video_sizer_->Clear(false);

    std::vector<int> ids = session_.ChannelsOfType(ch10::kVideoF0);
    for (int id : session_.ChannelsOfType(ch10::kVideoF1)) ids.push_back(id);
    std::sort(ids.begin(), ids.end());

    if (ids.empty() || !ch10::VideoStream::Available()) {
        video_hint_->SetLabel(ids.empty() ? "This recording has no video channels"
                                          : "Built without FFmpeg - video playback unavailable "
                                            "(extraction to .ts still works)");
        video_hint_->Show(true);
        video_sizer_->Add(video_hint_, 0, wxALIGN_CENTER | wxALL, 30);
    } else {
        video_hint_->Show(false);
        for (int id : ids) {
            auto* v = new VideoPanel(video_pane_, session_, id);
            video_panels_.push_back(v);
            video_sizer_->Add(v, 1, wxEXPAND | wxALL, 4);
        }
    }
    video_pane_->Layout();
    video_pane_->FitInside();
}

// ===========================================================================
// Playback
// ===========================================================================
void MainFrame::OnCursor(uint64_t rtc, bool playing) {
    play_btn_->SetLabel(playing ? "Pause" : "Play");
    if (speed_->GetSelection() != controller_.SpeedIndex()) speed_->SetSelection(controller_.SpeedIndex());
    if (!session_.IsOpen()) {
        time_rel_->SetLabel("+00:00:00.000 / 00:00:00.000");
        time_abs_->SetLabel("");
        return;
    }
    time_rel_->SetLabel(Utf8(session_.RelativeString(rtc)) + " / " +
                        Duration(session_.duration_s()));
    time_abs_->SetLabel(Utf8(session_.TimeString(rtc)));
    if (playing) {
        double sp = controller_.speed();
        SetStatusText(wxString::Format("%s %gx", sp < 0 ? "Rewinding" : "Playing",
                                       sp < 0 ? -sp : sp), 1);
    } else {
        SetStatusText(wxString::Format("Paused  (%gx)", std::fabs(controller_.speed())), 1);
    }

    updating_slider_ = true;
    slider_->SetValue(static_cast<int>(controller_.Fraction() * 10000));
    updating_slider_ = false;

    timeline_->SetCursor(rtc);
    for (auto* v : video_panels_) v->UpdateTo(rtc);
    packets_->FollowCursor(rtc);
    ms1553_->FollowCursor(rtc);
    ethernet_->FollowCursor(rtc);
    messages_->FollowCursor(rtc);
}

void MainFrame::OnCharHook(wxKeyEvent& ev) {
    wxWindow* focus = wxWindow::FindFocus();
    bool typing = focus && (dynamic_cast<wxTextCtrl*>(focus) != nullptr ||
                            dynamic_cast<wxSpinCtrl*>(focus) != nullptr);
    if (typing || ev.HasAnyModifiers() || !session_.IsOpen()) { ev.Skip(); return; }
    switch (ev.GetKeyCode()) {
        case '[':      controller_.SlowDown(); break;
        case ']':      controller_.SpeedUp(); break;
        case 'I':      { wxCommandEvent e; OnSetIn(e); } break;
        case 'O':      { wxCommandEvent e; OnSetOut(e); } break;
        case WXK_SPACE: controller_.TogglePlay(); break;
        default:       ev.Skip(); return;
    }
}

void MainFrame::OnSpeed(wxCommandEvent&) {
    int i = speed_->GetSelection();
    if (i >= 0) controller_.SetSpeed(PlaybackController::Speeds()[static_cast<std::size_t>(i)]);
}

void MainFrame::OnSlider(wxCommandEvent&) {
    if (updating_slider_ || !session_.IsOpen()) return;
    double f = slider_->GetValue() / 10000.0;
    controller_.Seek(session_.rtc_min() +
                     static_cast<uint64_t>(f * (session_.rtc_max() - session_.rtc_min())));
}

void MainFrame::OnViewPane(wxCommandEvent& ev) {
    switch (ev.GetId()) {
        case ID_VIEW_RECORDING: mgr_.GetPane("recording").Show(); break;
        case ID_VIEW_VIDEO:     mgr_.GetPane("video").Show(); break;
        case ID_VIEW_PLAYBACK:  mgr_.GetPane("playback").Show(); break;
        case ID_VIEW_AUDIO:     mgr_.GetPane("audio").Show(); break;
        case ID_VIEW_RESET:     mgr_.LoadPerspective(default_perspective_); break;
        default: break;
    }
    mgr_.Update();
}

// ===========================================================================
// Exports
// ===========================================================================
void MainFrame::OnExportTmats(wxCommandEvent&) {
    if (session_.tmats().empty()) return;
    wxFileName base(path_ctrl_->GetValue());
    wxFileDialog dlg(this, "Export TMATS", base.GetPath(), base.GetName() + "_tmats.txt",
                     "Text files (*.txt)|*.txt|TMATS files (*.tmt)|*.tmt|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    if (session_.ExportTmats(ToPath(dlg.GetPath())))
        SetStatusText("TMATS written to " + dlg.GetPath());
    else
        wxMessageBox("Could not write the file.", "Export TMATS", wxOK | wxICON_ERROR, this);
}

void MainFrame::OnExtractVideo(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    std::vector<int> ids = session_.ChannelsOfType(ch10::kVideoF0);
    for (int id : session_.ChannelsOfType(ch10::kVideoF1)) ids.push_back(id);
    if (ids.empty()) return;

    int channel = ids[0];
    if (ids.size() > 1) {
        wxArrayString names;
        for (int id : ids) {
            const auto* c = session_.Channel(id);
            names.Add(wxString::Format("Channel %d - %s (%s)", id, Utf8(c->source),
                                       c->video.FormatName()));
        }
        int pick = wxGetSingleChoiceIndex("Which video channel?", "Extract video", names, this);
        if (pick < 0) return;
        channel = ids[pick];
    }
    const auto* c = session_.Channel(channel);
    wxString ext = ch10::video::Extension(c->video);
    wxFileName base(path_ctrl_->GetValue());
    wxFileDialog dlg(this, "Extract video channel", base.GetPath(),
                     base.GetName() + wxString::Format("_ch%d", channel) + ext,
                     wxString::Format("MPEG stream (*%s)|*%s|All files (*.*)|*.*", ext, ext),
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    wxProgressDialog pd("Extracting video", "Writing stream...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT |
                        wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    Session::VideoResult res;
    bool ok = session_.ExtractVideo(channel, ToPath(dlg.GetPath()), res,
        [&pd](uint64_t done, uint64_t total) {
            return pd.Update(total ? static_cast<int>(std::min<uint64_t>(999, done * 1000 / total)) : 0);
        });
    pd.Update(1000);
    if (!ok) {
        if (session_.error() != "cancelled")
            wxMessageBox(Utf8(session_.error()), "Extract video", wxOK | wxICON_ERROR, this);
        return;
    }
    wxString msg = wxString::Format("Wrote %s (%llu MPEG packets) to\n%s", HumanBytes(res.bytes),
                                    static_cast<unsigned long long>(res.packets), dlg.GetPath());
    if (res.raw_copy_warning)
        msg += "\n\nNote: program stream with intra-packet time stamps was copied raw.";
    wxMessageBox(msg, "Extract video", wxOK | wxICON_INFORMATION, this);
    SetStatusText("Video written to " + dlg.GetPath());
}

void MainFrame::OnToolInject(wxCommandEvent&) {
    if (!session_.IsOpen()) return;
    controller_.Pause();
    InjectDialog dlg(this, session_);
    if (dlg.ShowModal() != wxID_OK) return;
    ch10::UserDataParse up = ch10::UserData::ParseFile(dlg.InputFile().string(), session_);
    if (up.records.empty()) { wxMessageBox("Nothing to add.", "User data", wxOK | wxICON_INFORMATION, this); return; }
    ch10::Injection inj = ch10::UserData::ToInjection(up.records, session_);
    wxProgressDialog pd("Adding user data", "Writing...", 1000, this,
                        wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME);
    ch10::OpResult r;
    bool ok = ch10::Cut(session_, ch10::FilterSpec(), session_.UserAnnotations(), inj, dlg.Output(), r,
                        [&pd](uint64_t d, uint64_t t) {
                            return pd.Update(t ? static_cast<int>(std::min<uint64_t>(999, d * 1000 / t)) : 0); });
    pd.Update(1000);
    ReportOperation("Add user data", ok, r);
}

void MainFrame::OnToolWordMapTemplate(wxCommandEvent&) {
    wxFileDialog dlg(this, "Write 1553 word map template", "", "wordmap.csv", "CSV files (*.csv)|*.csv",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::ofstream f(ToPath(dlg.GetPath()), std::ios::binary);
    f << ch10::WordMap::ExampleCsv();
    SetStatusText("Word map template written to " + dlg.GetPath());
}

void MainFrame::OnAbout(wxCommandEvent&) {
    wxMessageBox("IRIG 106 Chapter 10 Workbench\n\n"
                 "Reads Chapter 10 recordings: TMATS setup records, Time F1, "
                 "MIL-STD-1553 F1 and Video F0/F1, with synchronized playback of "
                 "video and bus data on the recording's RTC clock.\n\n"
                 "Built with " + wxString(wxVERSION_STRING) +
                 (ch10::VideoStream::Available() ? " and FFmpeg." : ". (no FFmpeg)"),
                 "About", wxOK | wxICON_INFORMATION, this);
}
