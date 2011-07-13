/* Copyright 2006-2011 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

/* Code related to:
* user-initiated search
* DDE commands, including search
*/

#include "BaseUtil.h"
#include "StrUtil.h"
#include "WinUtil.h"

#include "Search.h"
#include "resource.h"
#include "translations.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "Notifications.h"
#include "PdfSync.h"

#include "SumatraDialogs.h"
#include "AppTools.h"

bool NeedsFindUI(WindowInfo *win)
{
    return !win->IsDocLoaded() || win->dm->engine && !win->dm->engine->IsImageCollection();
}

void OnMenuFind(WindowInfo *win)
{
    if (!win->IsDocLoaded() || !NeedsFindUI(win))
        return;

    // Don't show a dialog if we don't have to - use the Toolbar instead
    if (gGlobalPrefs.toolbarVisible && !win->fullScreen && PM_DISABLED == win->presentation) {
        if (GetFocus() == win->hwndFindBox)
            SendMessage(win->hwndFindBox, WM_SETFOCUS, 0, 0);
        else
            SetFocus(win->hwndFindBox);
        return;
    }

    ScopedMem<TCHAR> previousFind(win::GetText(win->hwndFindBox));
    WORD state = (WORD)SendMessage(win->hwndToolbar, TB_GETSTATE, IDM_FIND_MATCH, 0);
    bool matchCase = (state & TBSTATE_CHECKED) != 0;

    ScopedMem<TCHAR> findString(Dialog_Find(win->hwndFrame, previousFind, &matchCase));
    if (!findString)
        return;

    win::SetText(win->hwndFindBox, findString);
    Edit_SetModify(win->hwndFindBox, TRUE);

    bool matchCaseChanged = matchCase != (0 != (state & TBSTATE_CHECKED));
    if (matchCaseChanged) {
        if (matchCase)
            state |= TBSTATE_CHECKED;
        else
            state &= ~TBSTATE_CHECKED;
        SendMessage(win->hwndToolbar, TB_SETSTATE, IDM_FIND_MATCH, state);
        win->dm->textSearch->SetSensitive(matchCase);
    }

    FindTextOnThread(win);
}

void OnMenuFindNext(WindowInfo *win)
{
    if (!NeedsFindUI(win))
        return;
    if (SendMessage(win->hwndToolbar, TB_ISBUTTONENABLED, IDM_FIND_NEXT, 0))
        FindTextOnThread(win, FIND_FORWARD);
}

void OnMenuFindPrev(WindowInfo *win)
{
    if (!NeedsFindUI(win))
        return;
    if (SendMessage(win->hwndToolbar, TB_ISBUTTONENABLED, IDM_FIND_PREV, 0))
        FindTextOnThread(win, FIND_BACKWARD);
}

void OnMenuFindMatchCase(WindowInfo *win)
{
    if (!NeedsFindUI(win))
        return;
    WORD state = (WORD)SendMessage(win->hwndToolbar, TB_GETSTATE, IDM_FIND_MATCH, 0);
    win->dm->textSearch->SetSensitive((state & TBSTATE_CHECKED) != 0);
    Edit_SetModify(win->hwndFindBox, TRUE);
}

static void ShowSearchResult(WindowInfo& win, TextSel *result, bool addNavPt)
{
   assert(result->len > 0);
   if (addNavPt || !win.dm->pageShown(result->pages[0]) ||
       (win.dm->zoomVirtual() == ZOOM_FIT_PAGE || win.dm->zoomVirtual() == ZOOM_FIT_CONTENT))
       win.dm->goToPage(result->pages[0], 0, addNavPt);

   win.dm->textSelection->CopySelection(win.dm->textSearch);

   UpdateTextSelection(&win, false);
   win.dm->ShowResultRectToScreen(result);
   win.RepaintAsync();
}

void ClearSearchResult(WindowInfo *win)
{
   DeleteOldSelectionInfo(win, true);
   win->RepaintAsync();
}

class UpdateFindStatusWorkItem : public UIThreadWorkItem {
   NotificationWnd *wnd;
   int current, total;

public:
   UpdateFindStatusWorkItem(WindowInfo *win, NotificationWnd *wnd, int current, int total)
       : UIThreadWorkItem(win), wnd(wnd), current(current), total(total) { }

   virtual void Execute() {
       if (WindowInfoStillValid(win) && !win->findCanceled && win->notifications->Contains(wnd))
           wnd->UpdateProgress(current, total);
   }
};

struct FindThreadData : public ProgressUpdateUI {
   WindowInfo *win;
   TextSearchDirection direction;
   bool wasModified;
   TCHAR *text;

   FindThreadData(WindowInfo& win, TextSearchDirection direction, HWND findBox) :
       win(&win), direction(direction) {
       text = win::GetText(findBox);
       wasModified = Edit_GetModify(findBox);
   }
   ~FindThreadData() { free(text); }

   void ShowUI() const {
       const LPARAM disable = (LPARAM)MAKELONG(0, 0);

       NotificationWnd *wnd = new NotificationWnd(win->hwndCanvas, _T(""), _TR("Searching %d of %d..."), win->notifications);
       // let win->messages own the NotificationWnd (FindThreadData might get deleted before)
       win->notifications->Add(wnd, NG_FIND_PROGRESS);

       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_PREV, disable);
       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_NEXT, disable);
       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_MATCH, disable);
   }

   void HideUI(NotificationWnd *wnd, bool success, bool loopedAround) const {
       LPARAM enable = (LPARAM)MAKELONG(1, 0);

       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_PREV, enable);
       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_NEXT, enable);
       SendMessage(win->hwndToolbar, TB_ENABLEBUTTON, IDM_FIND_MATCH, enable);

       if (!success && !loopedAround || !wnd) // i.e. canceled
           win->notifications->RemoveNotification(wnd);
       else if (!success && loopedAround)
           wnd->UpdateMessage(_TR("No matches were found"), 3000);
       else if (!loopedAround) {
           ScopedMem<TCHAR> buf(str::Format(_TR("Found text at page %d"), win->dm->currentPageNo()));
           wnd->UpdateMessage(buf, 3000);
       } else {
           ScopedMem<TCHAR> buf(str::Format(_TR("Found text at page %d (again)"), win->dm->currentPageNo()));
           wnd->UpdateMessage(buf, 3000, true);
       }    
   }

   virtual bool UpdateProgress(int current, int total) {
       if (!WindowInfoStillValid(win) || !win->notifications->GetFirstInGroup(NG_FIND_PROGRESS) || win->findCanceled)
           return false;
       QueueWorkItem(new UpdateFindStatusWorkItem(win, win->notifications->GetFirstInGroup(NG_FIND_PROGRESS), current, total));
       return true;
   }
};

class FindEndWorkItem : public UIThreadWorkItem {
   FindThreadData *ftd;
   TextSel*textSel;
   bool    wasModifiedCanceled;
   bool    loopedAround;

public:
   FindEndWorkItem(WindowInfo *win, FindThreadData *ftd, TextSel *textSel,
                   bool wasModifiedCanceled, bool loopedAround=false) :
       UIThreadWorkItem(win), ftd(ftd), textSel(textSel),
       loopedAround(loopedAround), wasModifiedCanceled(wasModifiedCanceled) { }
   ~FindEndWorkItem() { delete ftd; }

   virtual void Execute() {
       if (!WindowInfoStillValid(win))
           return;
       if (!win->IsDocLoaded()) {
           // the UI has already been disabled and hidden
       } else if (textSel) {
           ShowSearchResult(*win, textSel, wasModifiedCanceled);
           ftd->HideUI(win->notifications->GetFirstInGroup(NG_FIND_PROGRESS), true, loopedAround);
       } else {
           // nothing found or search canceled
           ClearSearchResult(win);
           ftd->HideUI(win->notifications->GetFirstInGroup(NG_FIND_PROGRESS), false, !wasModifiedCanceled);
       }

       HANDLE hThread = win->findThread;
       win->findThread = NULL;
       CloseHandle(hThread);
   }
};

static DWORD WINAPI FindThread(LPVOID data)
{
   FindThreadData *ftd = (FindThreadData *)data;
   assert(ftd && ftd->win && ftd->win->dm);
   WindowInfo *win = ftd->win;

   TextSel *rect;
   win->dm->textSearch->SetDirection(ftd->direction);
   if (ftd->wasModified || !win->dm->validPageNo(win->dm->textSearch->GetCurrentPageNo()) ||
       !win->dm->getPageInfo(win->dm->textSearch->GetCurrentPageNo())->visibleRatio)
       rect = win->dm->textSearch->FindFirst(win->dm->currentPageNo(), ftd->text, ftd);
   else
       rect = win->dm->textSearch->FindNext(ftd);

   bool loopedAround = false;
   if (!win->findCanceled && !rect) {
       // With no further findings, start over (unless this was a new search from the beginning)
       int startPage = (FIND_FORWARD == ftd->direction) ? 1 : win->dm->pageCount();
       if (!ftd->wasModified || win->dm->currentPageNo() != startPage) {
           loopedAround = true;
           MessageBeep(MB_ICONINFORMATION);
           rect = win->dm->textSearch->FindFirst(startPage, ftd->text, ftd);
       }
   }

   if (!win->findCanceled && rect)
       QueueWorkItem(new FindEndWorkItem(win, ftd, rect, ftd->wasModified, loopedAround));
   else
       QueueWorkItem(new FindEndWorkItem(win, ftd, NULL, win->findCanceled));

   return 0;
}

void FindTextOnThread(WindowInfo* win, TextSearchDirection direction)
{
   win->AbortFinding(true);

   FindThreadData *ftd = new FindThreadData(*win, direction, win->hwndFindBox);
   Edit_SetModify(win->hwndFindBox, FALSE);

   if (str::IsEmpty(ftd->text)) {
       delete ftd;
       return;
   }

   ftd->ShowUI();
   win->findThread = CreateThread(NULL, 0, FindThread, ftd, 0, 0);
}

void PaintForwardSearchMark(WindowInfo *win, HDC hdc)
{
    PageInfo *pageInfo = win->dm->getPageInfo(win->fwdSearchMark.page);
    if (!pageInfo || 0.0 == pageInfo->visibleRatio)
        return;
    
    // Draw the rectangles highlighting the forward search results
    for (UINT i = 0; i < win->fwdSearchMark.rects.Count(); i++) {
        RectD recD = win->fwdSearchMark.rects[i].Convert<double>();
        RectI recI = win->dm->CvtToScreen(win->fwdSearchMark.page, recD);
        if (gGlobalPrefs.fwdSearchOffset > 0) {
            recI.x = max(pageInfo->pageOnScreen.x, 0) + (int)(gGlobalPrefs.fwdSearchOffset * win->dm->zoomReal());
            recI.dx = (int)((gGlobalPrefs.fwdSearchWidth > 0 ? gGlobalPrefs.fwdSearchWidth : 15.0) * win->dm->zoomReal());
            recI.y -= 4;
            recI.dy += 8;
        }
        BYTE alpha = (BYTE)(0x5f * 1.0f * (HIDE_FWDSRCHMARK_STEPS - win->fwdSearchMark.hideStep) / HIDE_FWDSRCHMARK_STEPS);
        PaintTransparentRectangle(hdc, win->canvasRc, &recI, gGlobalPrefs.fwdSearchColor, alpha, 0);
    }
}

// returns true if the double-click was handled and false if it wasn't
bool OnInverseSearch(WindowInfo *win, int x, int y)
{
    if (!HasPermission(Perm_DiskAccess) || gPluginMode) return false;
    if (!win->IsDocLoaded() || win->dm->engineType != Engine_PDF) return false;

    // Clear the last forward-search result
    win->fwdSearchMark.rects.Reset();
    InvalidateRect(win->hwndCanvas, NULL, FALSE);

    // On double-clicking error message will be shown to the user
    // if the PDF does not have a synchronization file
    if (!win->pdfsync) {
        int err = Synchronizer::Create(win->loadedFilePath, win->dm, &win->pdfsync);
        if (err == PDFSYNCERR_SYNCFILE_NOTFOUND) {
            DBG_OUT("Pdfsync: Sync file not found!\n");
            // Fall back to selecting a word when double-clicking over text in
            // a document with no corresponding synchronization file
            if (win->dm->IsOverText(PointI(x, y)))
                return false;
            // In order to avoid confusion for non-LaTeX users, we do not show
            // any error message if the SyncTeX enhancements are hidden from UI
            if (gGlobalPrefs.enableTeXEnhancements)
                ShowNotification(win, _TR("No synchronization file found"));
            return true;
        }
        if (err != PDFSYNCERR_SUCCESS) {
            DBG_OUT("Pdfsync: Sync file cannot be loaded!\n");
            ShowNotification(win, _TR("Synchronization file cannot be opened"));
            return true;
        }
        gGlobalPrefs.enableTeXEnhancements = true;
    }

    int pageNo = win->dm->GetPageNoByPoint(PointI(x, y));
    if (!win->dm->validPageNo(pageNo))
        return false;

    PointI pt = win->dm->CvtFromScreen(PointI(x, y), pageNo).Convert<int>();
    ScopedMem<TCHAR> srcfilepath;
    UINT line, col;
    int err = win->pdfsync->pdf_to_source(pageNo, pt, srcfilepath, &line, &col);
    if (err != PDFSYNCERR_SUCCESS) {
        DBG_OUT("cannot sync from pdf to source!\n");
        ShowNotification(win, _TR("No synchronization info at this position"));
        return true;
    }

    TCHAR *inverseSearch = gGlobalPrefs.inverseSearchCmdLine;
    if (!inverseSearch)
        // Detect a text editor and use it as the default inverse search handler for now
        inverseSearch = AutoDetectInverseSearchCommands();

    ScopedMem<TCHAR> cmdline;
    if (inverseSearch)
        cmdline.Set(win->pdfsync->prepare_commandline(inverseSearch, srcfilepath, line, col));
    if (!str::IsEmpty(cmdline.Get())) {
        ScopedHandle process(LaunchProcess(cmdline));
        if (!process)
            ShowNotification(win, _TR("Cannot start inverse search command. Please check the command line in the settings."));
    }
    else if (gGlobalPrefs.enableTeXEnhancements)
        ShowNotification(win, _TR("Cannot start inverse search command. Please check the command line in the settings."));

    if (inverseSearch != gGlobalPrefs.inverseSearchCmdLine)
        free(inverseSearch);

    return true;
}

// Show the result of a PDF forward-search synchronization (initiated by a DDE command)
static void ShowForwardSearchResult(WindowInfo *win, const TCHAR *fileName, UINT line, UINT col, UINT ret, UINT page, Vec<RectI> &rects)
{
    win->fwdSearchMark.rects.Reset();
    const PageInfo *pi = win->dm->getPageInfo(page);
    if ((ret == PDFSYNCERR_SUCCESS) && (rects.Count() > 0) && (NULL != pi)) {
        // remember the position of the search result for drawing the rect later on
        win->fwdSearchMark.rects = rects;
        win->fwdSearchMark.page = page;
        win->fwdSearchMark.show = true;
        if (!gGlobalPrefs.fwdSearchPermanent)  {
            win->fwdSearchMark.hideStep = 0;
            SetTimer(win->hwndCanvas, HIDE_FWDSRCHMARK_TIMER_ID, HIDE_FWDSRCHMARK_DELAY_IN_MS, NULL);
        }

        // Scroll to show the overall highlighted zone
        int pageNo = page;
        RectI overallrc = rects[0];
        for (size_t i = 1; i < rects.Count(); i++)
            overallrc = overallrc.Union(rects[i]);
        TextSel res = { 1, &pageNo, &overallrc };
        if (!win->dm->pageVisible(page))
            win->dm->goToPage(page, 0, true);
        if (!win->dm->ShowResultRectToScreen(&res))
            win->RepaintAsync();
        if (IsIconic(win->hwndFrame))
            ShowWindowAsync(win->hwndFrame, SW_RESTORE);
        return;
    }

    ScopedMem<TCHAR> buf;
    if (ret == PDFSYNCERR_SYNCFILE_NOTFOUND )
        ShowNotification(win, _TR("No synchronization file found"));
    else if (ret == PDFSYNCERR_SYNCFILE_CANNOT_BE_OPENED)
        ShowNotification(win, _TR("Synchronization file cannot be opened"));
    else if (ret == PDFSYNCERR_INVALID_PAGE_NUMBER)
        buf.Set(str::Format(_TR("Page number %u inexistant"), page));
    else if (ret == PDFSYNCERR_NO_SYNC_AT_LOCATION)
        ShowNotification(win, _TR("No synchronization info at this position"));
    else if (ret == PDFSYNCERR_UNKNOWN_SOURCEFILE)
        buf.Set(str::Format(_TR("Unknown source file (%s)"), fileName));
    else if (ret == PDFSYNCERR_NORECORD_IN_SOURCEFILE)
        buf.Set(str::Format(_TR("Source file %s has no synchronization point"), fileName));
    else if (ret == PDFSYNCERR_NORECORD_FOR_THATLINE)
        buf.Set(str::Format(_TR("No result found around line %u in file %s"), line, fileName));
    else if (ret == PDFSYNCERR_NOSYNCPOINT_FOR_LINERECORD)
        buf.Set(str::Format(_TR("No result found around line %u in file %s"), line, fileName));
    if (buf)
        ShowNotification(win, buf);
}

// DDE commands handling

LRESULT OnDDEInitiate(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    DBG_OUT("received WM_DDE_INITIATE from %p with %08lx\n", (HWND)wparam, lparam);

    ATOM aServer = GlobalAddAtom(PDFSYNC_DDE_SERVICE);
    ATOM aTopic = GlobalAddAtom(PDFSYNC_DDE_TOPIC);

    if (LOWORD(lparam) == aServer && HIWORD(lparam) == aTopic) {
        if (!IsWindowUnicode((HWND)wparam))
            DBG_OUT("The client window is ANSI!\n");
        DBG_OUT("Sending WM_DDE_ACK to %p\n", (HWND)wparam);
        SendMessage((HWND)wparam, WM_DDE_ACK, (WPARAM)hwnd, MAKELPARAM(aServer, 0));
    }
    else {
        GlobalDeleteAtom(aServer);
        GlobalDeleteAtom(aTopic);
    }
    return 0;
}

// DDE commands

static void SetFocusHelper(HWND hwnd)
{
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);
    SetFocus(hwnd);
}

// Synchronization command format:
// [<DDECOMMAND_SYNC>(["<pdffile>",]"<srcfile>",<line>,<col>[,<newwindow>,<setfocus>])]
static const TCHAR *HandleSyncCmd(const TCHAR *cmd, DDEACK& ack)
{
    ScopedMem<TCHAR> pdfFile, srcFile;
    BOOL line = 0, col = 0, newWindow = 0, setFocus = 0;
    const TCHAR *next = str::Parse(cmd, _T("[") DDECOMMAND_SYNC _T("(\"%S\",%? \"%S\",%u,%u)]"),
                                   &pdfFile, &srcFile, &line, &col);
    if (!next)
        next = str::Parse(cmd, _T("[") DDECOMMAND_SYNC _T("(\"%S\",%? \"%S\",%u,%u,%u,%u)]"),
                          &pdfFile, &srcFile, &line, &col, &newWindow, &setFocus);
    // allow to omit the pdffile path, so that editors don't have to know about
    // multi-file projects (requires that the PDF has already been opened)
    if (!next) {
        pdfFile.Set(NULL);
        next = str::Parse(cmd, _T("[") DDECOMMAND_SYNC _T("(\"%S\",%u,%u)]"),
                          &srcFile, &line, &col);
        if (!next)
            next = str::Parse(cmd, _T("[") DDECOMMAND_SYNC _T("(\"%S\",%u,%u,%u,%u)]"),
                              &srcFile, &line, &col, &newWindow, &setFocus);
    }

    if (!next)
        return NULL;

    WindowInfo *win = NULL;
    if (pdfFile) {
        // check if the PDF is already opened
        win = FindWindowInfoByFile(pdfFile);
        // if not then open it
        if (newWindow || !win)
            win = LoadDocument(pdfFile, !newWindow ? win : NULL);
        else if (win && !win->IsDocLoaded())
            ReloadDocument(win);
    }
    else {
        // check if any opened PDF has sync information for the source file
        win = FindWindowInfoBySyncFile(srcFile);
        if (!win)
            DBG_OUT("PdfSync: No open PDF file found for %s!", srcFile);
        else if (newWindow)
            win = LoadDocument(win->loadedFilePath);
    }
    
    if (!win || !win->IsDocLoaded())
        return next;
    if (!win->pdfsync) {
        DBG_OUT("PdfSync: No sync file loaded!\n");
        return next;
    }

    ack.fAck = 1;
    assert(win->IsDocLoaded());
    UINT page;
    Vec<RectI> rects;
    int ret = win->pdfsync->source_to_pdf(srcFile, line, col, &page, rects);
    ShowForwardSearchResult(win, srcFile, line, col, ret, page, rects);
    if (setFocus)
        SetFocusHelper(win->hwndFrame);

    return next;
}

// Open file DDE command, format:
// [<DDECOMMAND_OPEN>("<pdffilepath>"[,<newwindow>,<setfocus>,<forcerefresh>])]
static const TCHAR *HandleOpenCmd(const TCHAR *cmd, DDEACK& ack)
{
    ScopedMem<TCHAR> pdfFile;
    BOOL newWindow = 0, setFocus = 0, forceRefresh = 0;
    const TCHAR *next = str::Parse(cmd, _T("[") DDECOMMAND_OPEN _T("(\"%S\")]"), &pdfFile);
    if (!next)
        next = str::Parse(cmd, _T("[") DDECOMMAND_OPEN _T("(\"%S\",%u,%u,%u)]"),
                          &pdfFile, &newWindow, &setFocus, &forceRefresh);
    if (!next)
        return NULL;
    
    WindowInfo *win = FindWindowInfoByFile(pdfFile);
    if (newWindow || !win)
        win = LoadDocument(pdfFile, !newWindow ? win : NULL);
    else if (win && !win->IsDocLoaded()) {
        ReloadDocument(win);
        forceRefresh = 0;
    }
    
    assert(!win || !win->IsAboutWindow());
    if (!win)
        return next;

    ack.fAck = 1;
    if (forceRefresh)
        ReloadDocument(win, true);
    if (setFocus)
        SetFocusHelper(win->hwndFrame);

    return next;
}

// Jump to named destination DDE command. Command format:
// [<DDECOMMAND_GOTO>("<pdffilepath>", "<destination name>")]
static const TCHAR *HandleGotoCmd(const TCHAR *cmd, DDEACK& ack)
{
    ScopedMem<TCHAR> pdfFile, destName;
    const TCHAR *next = str::Parse(cmd, _T("[") DDECOMMAND_GOTO _T("(\"%S\",%? \"%S\")]"),
                                   &pdfFile, &destName);
    if (!next)
        return NULL;

    WindowInfo *win = FindWindowInfoByFile(pdfFile);
    if (!win)
        return next;
    if (!win->IsDocLoaded()) {
        ReloadDocument(win);
        if (!win->IsDocLoaded())
            return next;
    }

    win->linkHandler->GotoNamedDest(destName);
    ack.fAck = 1;
    SetFocusHelper(win->hwndFrame);
    return next;
}

// Jump to page DDE command. Format:
// [<DDECOMMAND_PAGE>("<pdffilepath>", <page number>)]
static const TCHAR *HandlePageCmd(const TCHAR *cmd, DDEACK& ack)
{
    ScopedMem<TCHAR> pdfFile;
    UINT page;
    const TCHAR *next = str::Parse(cmd, _T("[") DDECOMMAND_PAGE _T("(\"%S\",%u)]"),
                                   &pdfFile, &page);
    if (!next)
        return false;

    // check if the PDF is already opened
    WindowInfo *win = FindWindowInfoByFile(pdfFile);
    if (!win)
        return next;
    if (!win->IsDocLoaded()) {
        ReloadDocument(win);
        if (!win->IsDocLoaded())
            return next;
    }

    if (!win->dm->validPageNo(page))
        return next;

    win->dm->goToPage(page, 0, true);
    ack.fAck = 1;
    SetFocusHelper(win->hwndFrame);
    return next;
}

// Set view mode and zoom level. Format:
// [<DDECOMMAND_SETVIEW>("<pdffilepath>", "<view mode>", <zoom level>[, <scrollX>, <scrollY>])]
static const TCHAR *HandleSetViewCmd(const TCHAR *cmd, DDEACK& ack)
{
    ScopedMem<TCHAR> pdfFile, viewMode;
    float zoom = INVALID_ZOOM;
    PointI scroll(-1, -1);
    const TCHAR *next = str::Parse(cmd, _T("[") DDECOMMAND_SETVIEW _T("(\"%S\",%? \"%S\",%f)]"),
                                   &pdfFile, &viewMode, &zoom);
    if (!next)
        next = str::Parse(cmd, _T("[") DDECOMMAND_SETVIEW _T("(\"%S\",%? \"%S\",%f,%d,%d)]"),
                          &pdfFile, &viewMode, &zoom, &scroll.x, &scroll.y);
    if (!next)
        return NULL;

    WindowInfo *win = FindWindowInfoByFile(pdfFile);
    if (!win)
        return next;
    if (!win->IsDocLoaded()) {
        ReloadDocument(win);
        if (!win->IsDocLoaded())
            return next;
    }

    DisplayMode mode;
    if (DisplayModeConv::EnumFromName(viewMode, &mode) && mode != DM_AUTOMATIC)
        SwitchToDisplayMode(win, mode);

    if (zoom != INVALID_ZOOM)
        ZoomToSelection(win, zoom, false);

    if (scroll.x != -1 || scroll.y != -1) {
        ScrollState ss = win->dm->GetScrollState();
        ss.x = scroll.x;
        ss.y = scroll.y;
        win->dm->SetScrollState(ss);
    }

    ack.fAck = 1;
    return next;
}

LRESULT OnDDExecute(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    DBG_OUT("Received WM_DDE_EXECUTE from %p with %08lx\n", (HWND)wparam, lparam);

    UINT_PTR lo, hi;
    UnpackDDElParam(WM_DDE_EXECUTE, lparam, &lo, &hi);
    DBG_OUT("%08lx => lo %04x hi %04x\n", lparam, lo, hi);

    ScopedMem<TCHAR> cmd;
    DDEACK ack = { 0 };

    LPVOID command = GlobalLock((HGLOBAL)hi);
    if (!command) {
        DBG_OUT("WM_DDE_EXECUTE: No command specified\n");
        goto Exit;
    }

    if (IsWindowUnicode((HWND)wparam)) {
        DBG_OUT("The client window is UNICODE!\n");
        cmd.Set(str::conv::FromWStr((const WCHAR*)command));
    } else {
        DBG_OUT("The client window is ANSI!\n");
        cmd.Set(str::conv::FromAnsi((const char*)command));
    }

    const TCHAR *currCmd = cmd;
    while (!str::IsEmpty(currCmd)) {
        const TCHAR *nextCmd = NULL;
        if (!nextCmd) nextCmd = HandleSyncCmd(currCmd, ack);
        if (!nextCmd) nextCmd = HandleOpenCmd(currCmd, ack);
        if (!nextCmd) nextCmd = HandleGotoCmd(currCmd, ack);
        if (!nextCmd) nextCmd = HandlePageCmd(currCmd, ack);
        if (!nextCmd) nextCmd = HandleSetViewCmd(currCmd, ack);
        if (!nextCmd) {
            DBG_OUT("WM_DDE_EXECUTE: unknown DDE command or bad command format\n");
            ScopedMem<TCHAR> tmp;
            nextCmd = str::Parse(currCmd, _T("%S]"), &tmp);
        }
        currCmd = nextCmd;
    }

Exit:
    GlobalUnlock((HGLOBAL)hi);

    DBG_OUT("Posting %s WM_DDE_ACK to %p\n", ack.fAck ? _T("ACCEPT") : _T("REJECT"), (HWND)wparam);
    lparam = ReuseDDElParam(lparam, WM_DDE_EXECUTE, WM_DDE_ACK, *(WORD *)&ack, hi);
    PostMessage((HWND)wparam, WM_DDE_ACK, (WPARAM)hwnd, lparam);
    return 0;
}

LRESULT OnDDETerminate(HWND hwnd, WPARAM wparam, LPARAM lparam)
{
    DBG_OUT("Received WM_DDE_TERMINATE from %p with %08lx\n", (HWND)wparam, lparam);

    // Respond with another WM_DDE_TERMINATE message
    PostMessage((HWND)wparam, WM_DDE_TERMINATE, (WPARAM)hwnd, 0L);
    return 0;
}
