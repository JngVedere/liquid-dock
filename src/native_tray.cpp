#include "native_tray.h"
#include "system.h"
#include <shellapi.h>
#include <shldisp.h>
#include <uiautomation.h>
#include <mutex>
#include <thread>

// The native taskbar stays "shown" (just invisible, see NativeTaskbar), so Explorer's own tray
// windows open normally when their buttons are pressed.
namespace nativetray {

bool ToggleDesktop() {
    // Same Shell action as the native taskbar button. No synthetic key presses,
    // per-window minimization, or locally maintained restore list.
    ComPtr<IShellDispatch4> shell;
    HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&shell));
    if (SUCCEEDED(hr)) hr = shell->ToggleDesktop();
    if (FAILED(hr)) Log(L"show desktop failed: %08x", hr);
    return SUCCEEDED(hr);
}

// Stock Windows 11 taskbar (XAML): its tray buttons are only reachable through UI Automation.
// The overflow host can be positioned after Explorer creates it.
//
// The XAML tray content only exists in the automation tree while the taskbar is expanded; once it
// auto-hides (as the dock makes it) it can't be found any more. Elements found earlier keep
// working though, so they are looked up before the taskbar vanishes (CacheTrayButtons) and reused.
enum Button { Chevron, Network, Clock, ButtonCount };
static const wchar_t* kButtonClass[ButtonCount] = { L"SystemTray.NormalButton",    // first one: hidden icons
                                                    L"SystemTray.AccentButton",    // network / quick settings
                                                    L"SystemTray.OmniButton" };    // clock / calendar
static ComPtr<IUIAutomation> g_uia;
static ComPtr<IUIAutomationElement> g_button[ButtonCount];
static HWND g_buttonTray = nullptr;
static std::vector<ComPtr<IUIAutomationElement>> g_icons;   // promoted app icons (AutomationId NotifyItemIcon)

static ComPtr<IUIAutomationElement> FindButton(HWND tray, Button b) {
    ComPtr<IUIAutomationElement> root, btn;
    ComPtr<IUIAutomationCondition> cls;
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(kButtonClass[b]);
    g_uia->CreatePropertyCondition(UIA_ClassNamePropertyId, v, &cls);
    VariantClear(&v);
    if (cls && SUCCEEDED(g_uia->ElementFromHandle(tray, &root)) && root)
        root->FindFirst(TreeScope_Descendants, cls.Get(), &btn);
    return btn;
}

void CacheTrayButtons(HWND tray) {
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify && GetDlgItem(notify, 1502)) return;   // classic taskbar, no XAML buttons
    if (g_buttonTray != tray) { for (auto& b : g_button) b.Reset(); g_icons.clear(); g_buttonTray = tray; }
    if (!g_uia) {
        if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia)))) return;
        // a busy explorer must not freeze the dock: give up quickly
        ComPtr<IUIAutomation2> u2;
        if (SUCCEEDED(g_uia.As(&u2))) { u2->put_ConnectionTimeout(500); u2->put_TransactionTimeout(500); }
    }
    for (int i = 0; i < ButtonCount; ++i)
        if (!g_button[i] && (g_button[i] = FindButton(tray, (Button)i))) Log(L"tray button %d cached", i);
    // the promoted icons come and go, so they are looked up again on every call
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> id;
    ComPtr<IUIAutomationElementArray> found;
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(L"NotifyItemIcon");
    g_uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &id);
    VariantClear(&v);
    int n = 0;
    if (id && SUCCEEDED(g_uia->ElementFromHandle(tray, &root)) && root &&
        SUCCEEDED(root->FindAll(TreeScope_Descendants, id.Get(), &found)) && found && SUCCEEDED(found->get_Length(&n)) && n > 0) {
        g_icons.clear();
        for (int i = 0; i < n; ++i) { ComPtr<IUIAutomationElement> e; if (SUCCEEDED(found->GetElement(i, &e)) && e) g_icons.push_back(e); }
    }
}

static bool InvokeButton(Button b) {
    ComPtr<IUIAutomationInvokePattern> inv;
    if (g_button[b] && SUCCEEDED(g_button[b]->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv && SUCCEEDED(inv->Invoke())) return true;
    g_button[b].Reset();   // stale (explorer restarted?)
    return false;
}

// Materialize Explorer's automation content only under the opacity guard
// (WithInvisibleTaskbars). An empty region alone is NOT sufficient:
// Explorer rebuilds it while expanding its taskbar.
// Polls found() for up to a second with the taskbar expanded, then runs act() before it auto-hides again.
static bool WhileExpanded(HWND tray, const std::function<bool()>& found, const std::function<bool()>& act) {
    APPBARDATA abd = { sizeof(abd) }; abd.hWnd = tray;
    UINT_PTR st = SHAppBarMessage(ABM_GETSTATE, &abd);
    bool autoHide = (st & ABS_AUTOHIDE) != 0;
    if (autoHide) { abd.lParam = st & ~ABS_AUTOHIDE; SHAppBarMessage(ABM_SETSTATE, &abd); }
    for (int i = 0; i < 40 && !found(); ++i) Sleep(25);
    bool ok = act();
    if (autoHide) { abd.lParam = st; SHAppBarMessage(ABM_SETSTATE, &abd); }
    return ok;
}

static bool OpenOverflowWin11(HWND tray) {
    bool cached = g_buttonTray == tray && g_button[Chevron], ok = cached && InvokeButton(Chevron);
    if (!ok) ok = WhileExpanded(tray, [&] { CacheTrayButtons(tray); return g_button[Chevron] != nullptr; },
                                [&] { return InvokeButton(Chevron); });
    Log(L"win11 overflow: cached=%d invoked=%d", (int)cached, (int)ok);
    return ok;
}

bool OpenOverflow(POINT anchor, Edge edge, RECT bar) {
    return g_taskbar.WithInvisibleTaskbars([&]() -> bool {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    HWND chevron = notify ? GetDlgItem(notify, 1502) : nullptr;   // classic (Windows 10 style) taskbar
    const wchar_t* popupClass = chevron ? L"NotifyIconOverflowWindow" : L"TopLevelWindowForOverflowXamlIsland";
    HWND previous = FindWindowW(popupClass, nullptr);
    bool wasOpen = previous && IsWindowVisible(previous);
    if (!chevron) {
        if (!OpenOverflowWin11(tray)) return false;
    } else {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(chevron, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 500, &r)) return false;
    }

    // A second click closes the existing flyout. Do not reopen or reposition it.
    if (wasOpen) return true;
    HWND ov = nullptr;
    for (int i = 0; i < 25 && !(ov = FindWindowW(popupClass, nullptr)); ++i) Sleep(10);
    for (int i = 0; i < 25 && ov && !IsWindowVisible(ov); ++i) Sleep(10);
    if (!ov || !IsWindowVisible(ov)) return false;
    RECT wr{}; if (!GetWindowRect(ov, &wr)) return false;
    int w = wr.right - wr.left, h = wr.bottom - wr.top, gap = 12;
    int x = anchor.x - w / 2, y = anchor.y - h / 2;
    switch (edge) {
    case Edge::Bottom: y = bar.top - h - gap; break;
    case Edge::Top:    y = bar.bottom + gap; break;
    case Edge::Left:   x = bar.right + gap; break;
    case Edge::Right:  x = bar.left - w - gap; break;
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    x = std::clamp<int>(x, mi.rcMonitor.left + 4, std::max(mi.rcMonitor.left + 4, mi.rcMonitor.right - w - 4));
    y = std::clamp<int>(y, mi.rcMonitor.top + 4, std::max(mi.rcMonitor.top + 4, mi.rcMonitor.bottom - h - 4));
    return SetWindowPos(ov, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
    });
}

// With the stock taskbar the buttons are invoked directly; the shortcuts are the fallback.
void OpenCalendar() { if (!InvokeButton(Clock)) SendKeyCombo({ VK_LWIN, VK_MENU, 'D' }); }

void OpenVolume(POINT c) {
    // SndVol's flyout mode takes the screen position as MAKELPARAM(x, y) of its centre
    wchar_t arg[32]; swprintf_s(arg, L"-f %ld", (long)MAKELPARAM((WORD)c.x, (WORD)c.y));
    ShellExecuteW(nullptr, L"open", L"SndVol.exe", arg, nullptr, SW_SHOWNORMAL);
}

void OpenQuickSettings() { if (!InvokeButton(Network)) SendKeyCombo({ VK_LWIN, 'A' }); }


// ---------------------------------------------------------------- promoted icons
//
// Explorer's undocumented notification-area interface (the old "Notification Area Icons" control
// panel; Chromium uses it to keep its own icon visible). Registering a callback makes Explorer
// report every icon synchronously, with its icon and whether it is shown on the taskbar.
struct NOTIFYITEM { PWSTR exe; PWSTR tip; HICON icon; HWND hwnd; DWORD preference; UINT id; GUID guid; };
constexpr DWORD kShowAlways = 2;   // 0 show when active (hidden icons), 1 never
struct __declspec(uuid("D782CCBA-AFB0-43F1-94DB-FDA3779EACCB")) INotificationCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Notify(ULONG event, NOTIFYITEM* item) = 0;
};
struct __declspec(uuid("D133CE13-3537-48BA-93A7-AFCD5D2053B4")) ITrayNotifyWin8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE RegisterCallback(INotificationCB* cb, ULONG* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterCallback(ULONG* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPreference(const NOTIFYITEM* item) = 0;   // never called
    virtual HRESULT STDMETHODCALLTYPE EnableAutoTray(BOOL enable) = 0;            // never called
    virtual HRESULT STDMETHODCALLTYPE DoAction(BOOL enable) = 0;                  // never called
};
static const CLSID CLSID_TrayNotify = { 0x25DEAD04, 0x1EAC, 0x4911, { 0x9E, 0x3A, 0xAD, 0x0A, 0x4A, 0xB5, 0x60, 0xFD } };

struct Collector : INotificationCB {
    LONG refs = 1;
    std::vector<PromotedIcon> icons;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(INotificationCB)) { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&refs); if (!r) delete this; return r; }
    HRESULT STDMETHODCALLTYPE Notify(ULONG, NOTIFYITEM* it) override {
        // past icons of programs that have exited are reported too: only live windows count
        if (it && it->preference == kShowAlways && it->icon && IsWindow(it->hwnd))
            icons.push_back({ it->tip ? it->tip : L"", nullptr, it->hwnd, it->id, it->icon });
        return S_OK;
    }
};

PromotedList::~PromotedList() { for (auto& i : icons) if (i.icon) DestroyIcon(i.icon); }

static std::mutex g_promotedMu;
static std::shared_ptr<const PromotedList> g_promoted;

static bool SameIcons(const std::vector<PromotedIcon>& a, const std::vector<PromotedIcon>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].hwnd != b[i].hwnd || a[i].id != b[i].id || a[i].source != b[i].source || a[i].tip != b[i].tip) return false;
    return true;
}

static void WatchPromoted() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // Explorer calls back into this apartment
    for (bool logged = false;; Sleep(2000)) {
        ComPtr<ITrayNotifyWin8> tn;
        Collector* cb = new Collector;
        HRESULT hr = CoCreateInstance(CLSID_TrayNotify, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&tn));   // anew: Explorer may restart
        ULONG id = 0;
        if (SUCCEEDED(hr) && SUCCEEDED(hr = tn->RegisterCallback(cb, &id))) tn->UnregisterCallback(&id);
        if (FAILED(hr) && !logged) { Log(L"promoted tray icons unavailable: %08x", hr); logged = true; }
        std::vector<PromotedIcon> icons = SUCCEEDED(hr) ? std::move(cb->icons) : std::vector<PromotedIcon>{};
        cb->Release();
        // shortcut: clicks reach only the Windows 11 taskbar's buttons, so the classic (Windows 10
        // style) taskbar gets no promoted icons; it would need its toolbar's TB_GETBUTTON data instead
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr), notify = tray ? FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) : nullptr;
        if (notify && GetDlgItem(notify, 1502)) icons.clear();
        {
            std::lock_guard<std::mutex> lk(g_promotedMu);
            if (g_promoted && SameIcons(g_promoted->icons, icons)) continue;
        }
        // The handles are Explorer's: copy them before it replaces or destroys them.
        auto list = std::make_shared<PromotedList>();
        list->icons = std::move(icons);
        for (auto& i : list->icons) i.icon = CopyIcon(i.source);
        std::lock_guard<std::mutex> lk(g_promotedMu);
        g_promoted = list;
    }
}

std::shared_ptr<const PromotedList> Promoted() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(WatchPromoted).detach(); });
    std::lock_guard<std::mutex> lk(g_promotedMu);
    return g_promoted;
}

// The taskbar button of this icon: by its tooltip, else by where Explorer says the icon is.
static ComPtr<IUIAutomationElement> FindIcon(const PromotedIcon& icon) {
    for (auto& e : g_icons) {
        BSTR name = nullptr;
        bool same = SUCCEEDED(e->get_CurrentName(&name)) && name && icon.tip == name;
        SysFreeString(name);
        if (same) return e;
    }
    NOTIFYICONIDENTIFIER nid = { sizeof(nid) }; nid.hWnd = icon.hwnd; nid.uID = icon.id;
    RECT r{};
    if (FAILED(Shell_NotifyIconGetRect(&nid, &r))) return nullptr;
    POINT c = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    for (auto& e : g_icons) {
        RECT b{};
        if (SUCCEEDED(e->get_CurrentBoundingRectangle(&b)) && PtInRect(&b, c)) return e;
    }
    return nullptr;
}

// Left: the button's own click (Invoke). Right: its context menu, which Explorer forwards to the app.
static bool Activate(IUIAutomationElement* e, bool right) {
    if (right) {
        ComPtr<IUIAutomationElement3> e3;
        return SUCCEEDED(e->QueryInterface(IID_PPV_ARGS(&e3))) && SUCCEEDED(e3->ShowContextMenu());
    }
    ComPtr<IUIAutomationInvokePattern> inv;
    return SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv && SUCCEEDED(inv->Invoke());
}

bool ClickPromoted(const PromotedIcon& icon, bool right) {
    // buttons found while the taskbar was last expanded usually still work
    if (auto e = FindIcon(icon); e && Activate(e.Get(), right)) return true;
    bool ok = g_taskbar.WithInvisibleTaskbars([&]() -> bool {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray) return false;
        ComPtr<IUIAutomationElement> e;
        return WhileExpanded(tray, [&] { CacheTrayButtons(tray); return (e = FindIcon(icon)) != nullptr; },
                             [&] { return e && Activate(e.Get(), right); });
    });
    Log(L"promoted icon click (right=%d): %d", (int)right, (int)ok);
    return ok;
}

}
