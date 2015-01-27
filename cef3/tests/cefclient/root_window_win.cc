// Copyright (c) 2015 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "cefclient/root_window_win.h"

#include "include/base/cef_bind.h"
#include "include/cef_app.h"
#include "cefclient/browser_window_osr_win.h"
#include "cefclient/browser_window_std_win.h"
#include "cefclient/client_switches.h"
#include "cefclient/main_message_loop.h"
#include "cefclient/resource.h"
#include "cefclient/temp_window_win.h"
#include "cefclient/util_win.h"

#define MAX_URL_LENGTH  255
#define BUTTON_WIDTH    72
#define URLBAR_HEIGHT   24

namespace client {

namespace {

// Message handler for the About box.
INT_PTR CALLBACK AboutWndProc(HWND hDlg, UINT message,
                              WPARAM wParam, LPARAM lParam) {
  UNREFERENCED_PARAMETER(lParam);
  switch (message) {
    case WM_INITDIALOG:
      return TRUE;

    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
        EndDialog(hDlg, LOWORD(wParam));
        return TRUE;
      }
      break;
  }
  return FALSE;
}

}  // namespace

RootWindowWin::RootWindowWin()
    : delegate_(NULL),
      with_controls_(false),
      is_popup_(false),
      start_rect_(),
      initialized_(false),
      hwnd_(NULL),
      back_hwnd_(NULL),
      forward_hwnd_(NULL),
      reload_hwnd_(NULL),
      stop_hwnd_(NULL),
      edit_hwnd_(NULL),
      edit_wndproc_old_(NULL),
      find_hwnd_(NULL),
      find_message_id_(0),
      find_wndproc_old_(NULL),
      find_state_(),
      find_buff_(),
      find_next_(false),
      find_match_case_last_(false),
      window_destroyed_(false),
      browser_destroyed_(false) {
}

RootWindowWin::~RootWindowWin() {
  REQUIRE_MAIN_THREAD();

  // The window and browser should already have been destroyed.
  DCHECK(window_destroyed_);
  DCHECK(browser_destroyed_);
}

void RootWindowWin::Init(RootWindow::Delegate* delegate,
                         bool with_controls,
                         bool with_osr,
                         const CefRect& bounds,
                         const CefBrowserSettings& settings,
                         const std::string& url) {
  DCHECK(delegate);
  DCHECK(!initialized_);

  delegate_ = delegate;
  with_controls_ = with_controls;

  start_rect_.left = bounds.x;
  start_rect_.top = bounds.y;
  start_rect_.right = bounds.x + bounds.width;
  start_rect_.bottom = bounds.y + bounds.height;

  CreateBrowserWindow(with_osr, url);

  initialized_ = true;

  // Create the native root window on the main thread.
  if (CURRENTLY_ON_MAIN_THREAD()) {
    CreateRootWindow(settings);
  } else {
    MAIN_POST_CLOSURE(
        base::Bind(&RootWindowWin::CreateRootWindow, this, settings));
  }
}

void RootWindowWin::InitAsPopup(RootWindow::Delegate* delegate,
                                bool with_controls,
                                bool with_osr,
                                const CefPopupFeatures& popupFeatures,
                                CefWindowInfo& windowInfo,
                                CefRefPtr<CefClient>& client,
                                CefBrowserSettings& settings) {
  DCHECK(delegate);
  DCHECK(!initialized_);

  delegate_ = delegate;
  with_controls_ = with_controls;
  is_popup_ = true;

  if (popupFeatures.xSet)
    start_rect_.left = popupFeatures.x;
  if (popupFeatures.ySet)
    start_rect_.top = popupFeatures.y;
  if (popupFeatures.widthSet)
    start_rect_.right = start_rect_.left + popupFeatures.width;
  if (popupFeatures.heightSet)
    start_rect_.bottom = start_rect_.top + popupFeatures.height;

  CreateBrowserWindow(with_osr, std::string());

  initialized_ = true;

  // The new popup is initially parented to a temporary window. The native root
  // window will be created after the browser is created and the popup window
  // will be re-parented to it at that time.
  browser_window_->GetPopupConfig(TempWindowWin::GetHWND(),
                                  windowInfo, client, settings);
}

void RootWindowWin::Show(ShowMode mode) {
  REQUIRE_MAIN_THREAD();

  if (!hwnd_)
    return;

  int nCmdShow = SW_SHOWNORMAL;
  switch (mode) {
    case ShowMinimized:
      nCmdShow = SW_SHOWNORMAL;
      break;
    case ShowMaximized:
      nCmdShow = SW_SHOWNORMAL;
      break;
    default:
      break;
  }

  ShowWindow(hwnd_, nCmdShow);
  UpdateWindow(hwnd_);
}

void RootWindowWin::Hide() {
  REQUIRE_MAIN_THREAD();

  if (hwnd_)
    ShowWindow(hwnd_, SW_HIDE);
}

void RootWindowWin::SetBounds(int x, int y, size_t width, size_t height) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_)
    SetWindowPos(hwnd_, NULL, 0, 0, 0, 0, SWP_NOZORDER);
}

void RootWindowWin::Close(bool force) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_) {
    if (force)
      DestroyWindow(hwnd_);
    else
      PostMessage(hwnd_, WM_CLOSE, 0, 0);
  }
}

CefRefPtr<CefBrowser> RootWindowWin::GetBrowser() const {
  REQUIRE_MAIN_THREAD();

  if (browser_window_)
    return browser_window_->GetBrowser();
  return NULL;
}

CefWindowHandle RootWindowWin::GetWindowHandle() const {
  REQUIRE_MAIN_THREAD();
  return hwnd_;
}

void RootWindowWin::CreateBrowserWindow(bool with_osr,
                                        const std::string& startup_url) {
  if (with_osr) {
    CefRefPtr<CefCommandLine> command_line =
        CefCommandLine::GetGlobalCommandLine();
    const bool transparent =
        command_line->HasSwitch(switches::kTransparentPaintingEnabled);
    const bool show_update_rect =
        command_line->HasSwitch(switches::kShowUpdateRect);
    browser_window_.reset(new BrowserWindowOsrWin(this, startup_url,
                                                  transparent,
                                                  show_update_rect));
  } else {
    browser_window_.reset(new BrowserWindowStdWin(this, startup_url));
  }
}

void RootWindowWin::CreateRootWindow(const CefBrowserSettings& settings) {
  REQUIRE_MAIN_THREAD();
  DCHECK(!hwnd_);

  HINSTANCE hInstance = GetModuleHandle(NULL);

  // Load strings from the resource file.
  const std::wstring& window_title = GetResourceString(IDS_APP_TITLE);
  const std::wstring& window_class = GetResourceString(IDC_CEFCLIENT);

  // Register the window class.
  RegisterRootClass(hInstance, window_class);

  // Register the message used with the find dialog.
  find_message_id_ = RegisterWindowMessage(FINDMSGSTRING);
  CHECK(find_message_id_);

  const DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

  int x, y, width, height;
  if (::IsRectEmpty(&start_rect_)) {
    // Use the default window position/size.
    x = y = width = height = CW_USEDEFAULT;
  } else {
    // Adjust the window size to account for window frame and controls.
    RECT window_rect = start_rect_;
    ::AdjustWindowRectEx(&window_rect, dwStyle, with_controls_, 0);
    if (with_controls_)
      window_rect.bottom += URLBAR_HEIGHT;

    x = start_rect_.left;
    y = start_rect_.top;
    width = window_rect.right - window_rect.left;
    height = window_rect.bottom - window_rect.top;
  }

  // Create the main window initially hidden.
  hwnd_ = CreateWindow(window_class.c_str(), window_title.c_str(),
                       dwStyle,
                       x, y, width, height,
                       NULL, NULL, hInstance, NULL);
  CHECK(hwnd_);

  // Associate |this| with the main window.
  SetUserDataPtr(hwnd_, this);

  RECT rect;
  GetClientRect(hwnd_, &rect);

  if (with_controls_) {
    // Create the child controls.
    int x = 0;

    back_hwnd_ = CreateWindow(
        L"BUTTON", L"Back",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x, 0, BUTTON_WIDTH, URLBAR_HEIGHT,
        hwnd_, reinterpret_cast<HMENU>(IDC_NAV_BACK), hInstance, 0);
    CHECK(back_hwnd_);
    x += BUTTON_WIDTH;

    forward_hwnd_ = CreateWindow(
        L"BUTTON", L"Forward",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x, 0, BUTTON_WIDTH, URLBAR_HEIGHT,
        hwnd_, reinterpret_cast<HMENU>(IDC_NAV_FORWARD), hInstance, 0);
    CHECK(forward_hwnd_);
    x += BUTTON_WIDTH;

    reload_hwnd_ = CreateWindow(
        L"BUTTON", L"Reload",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON| WS_DISABLED,
        x, 0, BUTTON_WIDTH, URLBAR_HEIGHT,
        hwnd_, reinterpret_cast<HMENU>(IDC_NAV_RELOAD), hInstance, 0);
    CHECK(reload_hwnd_);
    x += BUTTON_WIDTH;

    stop_hwnd_ = CreateWindow(
        L"BUTTON", L"Stop",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        x, 0, BUTTON_WIDTH, URLBAR_HEIGHT,
        hwnd_, reinterpret_cast<HMENU>(IDC_NAV_STOP), hInstance, 0);
    CHECK(stop_hwnd_);
    x += BUTTON_WIDTH;

    edit_hwnd_ = CreateWindow(
        L"EDIT", 0,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOVSCROLL |
        ES_AUTOHSCROLL| WS_DISABLED,
        x, 0, rect.right - BUTTON_WIDTH * 4, URLBAR_HEIGHT,
        hwnd_, 0, hInstance, 0);
    CHECK(edit_hwnd_);

    // Override the edit control's window procedure.
    edit_wndproc_old_ = SetWndProcPtr(edit_hwnd_, EditWndProc);

    // Associate |this| with the edit window.
    SetUserDataPtr(edit_hwnd_, this);

    rect.top += URLBAR_HEIGHT;
  } else {
    // No controls so also remove the default menu.
    ::SetMenu(hwnd_, NULL);
  }

  if (!is_popup_) {
    // Create the browser window.
    browser_window_->CreateBrowser(hwnd_, rect, settings);
  } else {
    // With popups we already have a browser window. Parent the browser window
    // to the root window and show it in the correct location.
    browser_window_->ShowPopup(hwnd_,
                               rect.left, rect.top,
                               rect.right - rect.left,
                               rect.bottom - rect.top);
  }

  // Show this window.
  Show(ShowNormal);
}

// static
void RootWindowWin::RegisterRootClass(HINSTANCE hInstance,
                                      const std::wstring& window_class) {
  // Only register the class one time.
  static bool class_registered = false;
  if (class_registered)
    return;
  class_registered = true;

  WNDCLASSEX wcex;

  wcex.cbSize = sizeof(WNDCLASSEX);

  wcex.style         = CS_HREDRAW | CS_VREDRAW;
  wcex.lpfnWndProc   = RootWndProc;
  wcex.cbClsExtra    = 0;
  wcex.cbWndExtra    = 0;
  wcex.hInstance     = hInstance;
  wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_CEFCLIENT));
  wcex.hCursor       = LoadCursor(NULL, IDC_ARROW);
  wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
  wcex.lpszMenuName  = MAKEINTRESOURCE(IDC_CEFCLIENT);
  wcex.lpszClassName = window_class.c_str();
  wcex.hIconSm       = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

  RegisterClassEx(&wcex);
}

// static
LRESULT CALLBACK RootWindowWin::EditWndProc(HWND hWnd, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
  DCHECK(self);
  DCHECK(hWnd == self->edit_hwnd_);

  switch (message) {
    case WM_CHAR:
      if (wParam == VK_RETURN) {
        // When the user hits the enter key load the URL.
        CefRefPtr<CefBrowser> browser = self->GetBrowser();
        if (browser) {
          wchar_t strPtr[MAX_URL_LENGTH+1] = {0};
          *((LPWORD)strPtr) = MAX_URL_LENGTH;
          LRESULT strLen = SendMessage(hWnd, EM_GETLINE, 0, (LPARAM)strPtr);
          if (strLen > 0) {
            strPtr[strLen] = 0;
            browser->GetMainFrame()->LoadURL(strPtr);
          }
        }
        return 0;
      }
      break;
    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->edit_hwnd_ = NULL;
      break;
  }

  return CallWindowProc(self->edit_wndproc_old_, hWnd, message, wParam, lParam);
}

// static
LRESULT CALLBACK RootWindowWin::FindWndProc(HWND hWnd, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
  DCHECK(self);
  DCHECK(hWnd == self->find_hwnd_);

  switch (message) {
    case WM_ACTIVATE:
      // Set this dialog as current when activated.
      MainMessageLoop::Get()->SetCurrentModelessDialog(
          wParam == 0 ? NULL : hWnd);
      return FALSE;
    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->find_hwnd_ = NULL;
      break;
  }

  return CallWindowProc(self->find_wndproc_old_, hWnd, message, wParam, lParam);
}

// static
LRESULT CALLBACK RootWindowWin::RootWndProc(HWND hWnd, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
  REQUIRE_MAIN_THREAD();

  RootWindowWin* self = GetUserDataPtr<RootWindowWin*>(hWnd);
  if (!self)
    return DefWindowProc(hWnd, message, wParam, lParam);
  DCHECK(hWnd == self->hwnd_);

  if (message == self->find_message_id_) {
    // Message targeting the find dialog.
    LPFINDREPLACE lpfr = reinterpret_cast<LPFINDREPLACE>(lParam);
    CHECK(lpfr == &self->find_state_);
    self->OnFindEvent();
    return 0;
  }

  // Callback for the main window
  switch (message) {
    case WM_COMMAND:
      if (self->OnCommand(LOWORD(wParam)))
        return 0;
      break;

    case WM_PAINT:
      self->OnPaint();
      return 0;

    case WM_SETFOCUS:
      self->OnFocus();
      return 0;

    case WM_SIZE:
      self->OnSize(wParam == SIZE_MINIMIZED);
      break;

    case WM_MOVING:
    case WM_MOVE:
      self->OnMove();
      return 0;

    case WM_ERASEBKGND:
      // Never erase the background.
      return 0;

    case WM_ENTERMENULOOP:
      if (!wParam) {
        // Entering the menu loop for the application menu.
        CefSetOSModalLoop(true);
      }
      break;

    case WM_EXITMENULOOP:
      if (!wParam) {
        // Exiting the menu loop for the application menu.
        CefSetOSModalLoop(false);
      }
      break;

    case WM_CLOSE:
      if (self->OnClose())
        return 0;  // Cancel the close.
      break;

    case WM_NCDESTROY:
      // Clear the reference to |self|.
      SetUserDataPtr(hWnd, NULL);
      self->hwnd_ = NULL;
      self->OnDestroyed();
      return 0;
  }

  return DefWindowProc(hWnd, message, wParam, lParam);
}

void RootWindowWin::OnPaint() {
  PAINTSTRUCT ps;
  BeginPaint(hwnd_, &ps);
  EndPaint(hwnd_, &ps);
}

void RootWindowWin::OnFocus() {
  if (browser_window_)
    browser_window_->SetFocus();
}

void RootWindowWin::OnSize(bool minimized) {
  if (minimized) {
    // Notify the browser window that it was hidden and do nothing further.
    if (browser_window_)
      browser_window_->Hide();
    return;
  }

  if (browser_window_)
    browser_window_->Show();

  RECT rect;
  GetClientRect(hwnd_, &rect);

  if (with_controls_) {
    // Resize the window and address bar to match the new frame size.
    rect.top += URLBAR_HEIGHT;

    int urloffset = rect.left + BUTTON_WIDTH * 4;

    if (browser_window_) {
      HWND browser_hwnd = browser_window_->GetHWND();
      HDWP hdwp = BeginDeferWindowPos(1);
      hdwp = DeferWindowPos(hdwp, edit_hwnd_, NULL, urloffset,
          0, rect.right - urloffset, URLBAR_HEIGHT, SWP_NOZORDER);
      hdwp = DeferWindowPos(hdwp, browser_hwnd, NULL,
          rect.left, rect.top, rect.right - rect.left,
          rect.bottom - rect.top, SWP_NOZORDER);
      EndDeferWindowPos(hdwp);
    } else {
      SetWindowPos(edit_hwnd_, NULL, urloffset,
          0, rect.right - urloffset, URLBAR_HEIGHT, SWP_NOZORDER);
    }
  } else if (browser_window_) {
    // Size the browser window to the whole client area.
    browser_window_->SetBounds(0, 0, rect.right, rect.bottom);
  }
}

void RootWindowWin::OnMove() {
  // Notify the browser of move events so that popup windows are displayed
  // in the correct location and dismissed when the window moves.
  CefRefPtr<CefBrowser> browser = GetBrowser();
  if (browser)
    browser->GetHost()->NotifyMoveOrResizeStarted();
}

bool RootWindowWin::OnCommand(UINT id) {
  if (id >= ID_TESTS_FIRST && id <= ID_TESTS_LAST) {
    delegate_->OnTest(this, id);
    return true;
  }

  switch (id) {
    case IDM_ABOUT:
      OnAbout();
      return true;
    case IDM_EXIT:
      delegate_->OnExit(this);
      return true;
    case ID_FIND:
      OnFind();
      return true;
    case IDC_NAV_BACK:   // Back button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->GoBack();
      return true;
    case IDC_NAV_FORWARD:  // Forward button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->GoForward();
      return true;
    case IDC_NAV_RELOAD:  // Reload button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->Reload();
      return true;
    case IDC_NAV_STOP:  // Stop button
      if (CefRefPtr<CefBrowser> browser = GetBrowser())
        browser->StopLoad();
      return true;
  }

  return false;
}

void RootWindowWin::OnFind() {
  if (find_hwnd_) {
    // Give focus to the existing find dialog.
    ::SetFocus(find_hwnd_);
    return;
  }

  // Configure dialog state.
  ZeroMemory(&find_state_, sizeof(find_state_));
  find_state_.lStructSize = sizeof(find_state_);
  find_state_.hwndOwner = hwnd_;
  find_state_.lpstrFindWhat = find_buff_;
  find_state_.wFindWhatLen = sizeof(find_buff_);
  find_state_.Flags = FR_HIDEWHOLEWORD | FR_DOWN;

  // Create the dialog.
  find_hwnd_ = FindText(&find_state_);

  // Override the dialog's window procedure.
  find_wndproc_old_ = SetWndProcPtr(find_hwnd_, FindWndProc);

  // Associate |self| with the dialog.
  SetUserDataPtr(find_hwnd_, this);
}

void RootWindowWin::OnFindEvent() {
  CefRefPtr<CefBrowser> browser = GetBrowser();

  if (find_state_.Flags & FR_DIALOGTERM) {
    // The find dialog box has been dismissed so invalidate the handle and
    // reset the search results.
    if (browser) {
      browser->GetHost()->StopFinding(true);
      find_what_last_.clear();
      find_next_ = false;
    }
  } else if ((find_state_.Flags & FR_FINDNEXT) && browser)  {
    // Search for the requested string.
    bool match_case = ((find_state_.Flags & FR_MATCHCASE) ? true : false);
    const std::wstring& find_what = find_buff_;
    if (match_case != find_match_case_last_ || find_what != find_what_last_) {
      // The search string has changed, so reset the search results.
      if (!find_what.empty()) {
        browser->GetHost()->StopFinding(true);
        find_next_ = false;
      }
      find_match_case_last_ = match_case;
      find_what_last_ = find_buff_;
    }

    browser->GetHost()->Find(0, find_what,
                             (find_state_.Flags & FR_DOWN) ? true : false,
                             match_case, find_next_);
    if (!find_next_)
      find_next_ = true;
  }
}

void RootWindowWin::OnAbout() {
  // Show the about box.
  DialogBox(GetModuleHandle(NULL), MAKEINTRESOURCE(IDD_ABOUTBOX), hwnd_,
            AboutWndProc);
}

bool RootWindowWin::OnClose() {
  if (browser_window_ && !browser_window_->IsClosing()) {
    CefRefPtr<CefBrowser> browser = GetBrowser();
    if (browser) {
      // Notify the browser window that we would like to close it. This
      // will result in a call to ClientHandler::DoClose() if the
      // JavaScript 'onbeforeunload' event handler allows it.
      browser->GetHost()->CloseBrowser(false);

      // Cancel the close.
      return true;
    }
  }

  // Allow the close.
  return false;
}

void RootWindowWin::OnDestroyed() {
  window_destroyed_ = true;
  NotifyDestroyedIfDone();
}

void RootWindowWin::OnBrowserCreated(CefRefPtr<CefBrowser> browser) {
  REQUIRE_MAIN_THREAD();

  // For popup browsers create the root window once the browser has been
  // created.
  if (is_popup_)
    CreateRootWindow(CefBrowserSettings());
}

void RootWindowWin::OnBrowserWindowDestroyed() {
  REQUIRE_MAIN_THREAD();

  browser_window_.reset();
  browser_destroyed_ = true;

  if (!window_destroyed_) {
    // The browser was destroyed first. This could be due to the use of
    // off-screen rendering or execution of JavaScript window.close().
    // Close the RootWindow asynchronously.
    Close(false);
  }

  NotifyDestroyedIfDone();
}

void RootWindowWin::OnSetAddress(const std::string& url) {
  REQUIRE_MAIN_THREAD();

  if (edit_hwnd_)
    SetWindowText(edit_hwnd_, CefString(url).ToWString().c_str());
}

void RootWindowWin::OnSetTitle(const std::string& title) {
  REQUIRE_MAIN_THREAD();

  if (hwnd_)
    SetWindowText(hwnd_, CefString(title).ToWString().c_str());
}

void RootWindowWin::OnSetLoadingState(bool isLoading,
                                      bool canGoBack,
                                      bool canGoForward) {
  REQUIRE_MAIN_THREAD();

  if (with_controls_) {
    EnableWindow(back_hwnd_, canGoBack);
    EnableWindow(forward_hwnd_, canGoForward);
    EnableWindow(reload_hwnd_, !isLoading);
    EnableWindow(stop_hwnd_, isLoading);
    EnableWindow(edit_hwnd_, TRUE);
  }
}

void RootWindowWin::NotifyDestroyedIfDone() {
  // Notify once both the window and the browser have been destroyed.
  if (window_destroyed_ && browser_destroyed_)
    delegate_->OnRootWindowDestroyed(this);
}

// static
scoped_refptr<RootWindow> RootWindow::Create() {
  return new RootWindowWin();
}

}  // namespace client
