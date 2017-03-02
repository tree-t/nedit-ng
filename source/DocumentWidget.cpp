
#include "DocumentWidget.h"
#include "DialogMoveDocument.h"
#include "DialogOutput.h"
#include "DialogPrint.h"
#include "DialogReplace.h"
#include "HighlightData.h"
#include "LanguageMode.h"
#include "MainWindow.h"
#include "SignalBlocker.h"
#include "SmartIndent.h"
#include "TextArea.h"
#include "TextBuffer.h"
#include "UndoInfo.h"
#include "WindowHighlightData.h"
#include "calltips.h"
#include "clearcase.h"
#include "dragEndCBStruct.h"
#include "file.h"
#include "fileUtils.h"
#include "highlight.h"
#include "highlightData.h"
#include "interpret.h"
#include "macro.h"
#include "nedit.h"
#include "parse.h"
#include "preferences.h"
#include "search.h"
#include "selection.h"
#include "smartIndent.h"
#include "smartIndentCBStruct.h"
#include "tags.h"
#include "utils.h"
#include <QBoxLayout>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFuture>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QRadioButton>
#include <QSplitter>
#include <QTextCodec>
#include <QTimer>
#include <QtDebug>
#include <fcntl.h>
#include <memory>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


namespace {

// Tuning parameters
const int BANNER_WAIT_TIME	  = 6000; // how long to wait (msec) before putting up Shell Command Executing... banner


/* data attached to window during shell command execution with
   information for controling and communicating with the process */
struct shellCmdInfoEx {
    QProcess *process;
    int flags;
    QByteArray standardOutput;
    QByteArray standardError;
    TextArea *area;
    int leftPos;
    int rightPos;
    QTimer *bannerTimer;
    bool bannerIsUp;
    bool fromMacro;
};


// flags for issueCommand
enum {
    ACCUMULATE  	  = 1,
    ERROR_DIALOGS	  = 2,
    REPLACE_SELECTION = 4,
    RELOAD_FILE_AFTER = 8,
    OUTPUT_TO_DIALOG  = 16,
    OUTPUT_TO_STRING  = 32
};

const int MAX_TAG_LEN = 256;

struct CharMatchTable {
    char c;
    char match;
    char direction;
};

const int N_MATCH_CHARS = 13;

static const CharMatchTable MatchingChars[N_MATCH_CHARS] = {
    {'{', '}',   SEARCH_FORWARD},
    {'}', '{',   SEARCH_BACKWARD},
    {'(', ')',   SEARCH_FORWARD},
    {')', '(',   SEARCH_BACKWARD},
    {'[', ']',   SEARCH_FORWARD},
    {']', '[',   SEARCH_BACKWARD},
    {'<', '>',   SEARCH_FORWARD},
    {'>', '<',   SEARCH_BACKWARD},
    {'/', '/',   SEARCH_FORWARD},
    {'"', '"',   SEARCH_FORWARD},
    {'\'', '\'', SEARCH_FORWARD},
    {'`', '`',   SEARCH_FORWARD},
    {'\\', '\\', SEARCH_FORWARD},
};

/*
 * Number of bytes read at once by cmpWinAgainstFile
 */
const int PREFERRED_CMPBUF_LEN = 0x8000;

// TODO(eteran): use an enum for this
const int FORWARD = 1;
const int REVERSE = 2;

/* Maximum frequency in miliseconds of checking for external modifications.
   The periodic check is only performed on buffer modification, and the check
   interval is only to prevent checking on every keystroke in case of a file
   system which is slow to process stat requests (which I'm not sure exists) */
const int MOD_CHECK_INTERVAL = 3000;

void modifiedCB(int pos, int nInserted, int nDeleted, int nRestyled, view::string_view deletedText, void *user) {
    if(auto document = static_cast<DocumentWidget *>(user)) {
        document->modifiedCallback(pos, nInserted, nDeleted, nRestyled, deletedText);
    }
}

void smartIndentCB(TextArea *area, smartIndentCBStruct *data, void *user) {
    if(auto document = static_cast<DocumentWidget *>(user)) {
        document->smartIndentCallback(area, data);
	}
}

void movedCB(TextArea *area, void *user) {
    if(auto document = static_cast<DocumentWidget *>(user)) {
        document->movedCallback(area);
	}
}

void dragStartCB(TextArea *area, void *user) {
    if(auto document = static_cast<DocumentWidget *>(user)) {
        document->dragStartCallback(area);
	}
}

void dragEndCB(TextArea *area, dragEndCBStruct *data, void *user) {
    if(auto document = static_cast<DocumentWidget *>(user)) {
        document->dragEndCallback(area, data);
	}
}

/*
** Buffer replacement wrapper routine to be used for inserting output from
** a command into the buffer, which takes into account that the buffer may
** have been shrunken by the user (eg, by Undo). If necessary, the starting
** and ending positions (part of the state of the command) are corrected.
*/
void safeBufReplace(TextBuffer *buf, int *start, int *end, const std::string &text) {
    if (*start > buf->BufGetLength()) {
        *start = buf->BufGetLength();
    }

    if (*end > buf->BufGetLength()) {
        *end = buf->BufGetLength();
    }

    buf->BufReplaceEx(*start, *end, text);
}

/*
** Update a position across buffer modifications specified by
** "modPos", "nDeleted", and "nInserted".
*/
void maintainPosition(int *position, int modPos, int nInserted, int nDeleted) {
    if (modPos > *position) {
        return;
    }

    if (modPos + nDeleted <= *position) {
        *position += nInserted - nDeleted;
    } else {
        *position = modPos;
    }
}

/*
** Update a selection across buffer modifications specified by
** "pos", "nDeleted", and "nInserted".
*/
void maintainSelection(TextSelection *sel, int pos, int nInserted, int nDeleted) {
    if (!sel->selected || pos > sel->end) {
        return;
    }

    maintainPosition(&sel->start, pos, nInserted, nDeleted);
    maintainPosition(&sel->end, pos, nInserted, nDeleted);
    if (sel->end <= sel->start) {
        sel->selected = false;
    }
}

/*
**
*/
UndoTypes determineUndoType(int nInserted, int nDeleted) {
    int textDeleted, textInserted;

    textDeleted = (nDeleted > 0);
    textInserted = (nInserted > 0);

    if (textInserted && !textDeleted) {
        // Insert
        if (nInserted == 1)
            return ONE_CHAR_INSERT;
        else
            return BLOCK_INSERT;
    } else if (textInserted && textDeleted) {
        // Replace
        if (nInserted == 1)
            return ONE_CHAR_REPLACE;
        else
            return BLOCK_REPLACE;
    } else if (!textInserted && textDeleted) {
        // Delete
        if (nDeleted == 1)
            return ONE_CHAR_DELETE;
        else
            return BLOCK_DELETE;
    } else {
        // Nothing deleted or inserted
        return UNDO_NOOP;
    }
}

}

/*
** Open an existing file specified by name and path.  Use the window inWindow
** unless inWindow is nullptr or points to a window which is already in use
** (displays a file other than Untitled, or is Untitled but modified).  Flags
** can be any of:
**
**	CREATE: 		If file is not found, (optionally) prompt the
**				user whether to create
**	SUPPRESS_CREATE_WARN	When creating a file, don't ask the user
**	PREF_READ_ONLY		Make the file read-only regardless
**
** If languageMode is passed as nullptr, it will be determined automatically
** from the file extension or file contents.
**
** If bgOpen is true, then the file will be open in background. This
** works in association with the SetLanguageMode() function that has
** the syntax highlighting deferred, in order to speed up the file-
** opening operation when multiple files are being opened in succession.
*/

// TODO(eteran): inWindow is a DocumentWidget, not a MainWindow...
//               so either rename the parameter, OR rework this in
//               terms of MainWindow
DocumentWidget *DocumentWidget::EditExistingFileEx(DocumentWidget *inWindow, const QString &name, const QString &path, int flags, const QString &geometry, int iconic, const QString &languageMode, bool tabbed, bool bgOpen) {

    // first look to see if file is already displayed in a window
    if(DocumentWidget *document = MainWindow::FindWindowWithFile(name, path)) {
        if (!bgOpen) {
            if (iconic) {
                document->RaiseDocument();
            } else {
                document->RaiseDocumentWindow();
            }
        }
        return document;
    }

    /* If an existing window isn't specified; or the window is already
       in use (not Untitled or Untitled and modified), or is currently
       busy running a macro; create the window */
    DocumentWidget *document = nullptr;
    if(!inWindow) {
        MainWindow *const win = new MainWindow();
        document = win->CreateDocument(name);
        if(iconic) {
            win->showMinimized();
        } else {
            win->showNormal();
        }
        win->parseGeometry(geometry);
    } else if (inWindow->filenameSet_ || inWindow->fileChanged_ || inWindow->macroCmdData_) {
        if (tabbed) {
            if(auto win = inWindow->toWindow()) {
                document = win->CreateDocument(name);
            }
        } else {
            MainWindow *const win = new MainWindow();
            document = win->CreateDocument(name);
            if(iconic) {
                win->showMinimized();
            } else {
                win->showNormal();
            }
            win->parseGeometry(geometry);
        }
    } else {
        // open file in untitled document
        document            = inWindow;
        document->path_     = path;
        document->filename_ = name;

        if (!iconic && !bgOpen) {
            document->RaiseDocumentWindow();
        }
    }

    MainWindow *const win = document->toWindow();
    if(!win) {
        return nullptr;
    }

    // Open the file
    if (!document->doOpen(name, path, flags)) {
        // The user may have destroyed the window instead of closing
        // the warning dialog; don't close it twice
        document->safeCloseEx();
        return nullptr;
    }

    win->forceShowLineNumbers();

    // Decide what language mode to use, trigger language specific actions
    if(languageMode.isNull()) {
        document->DetermineLanguageMode(true);
    } else {
        document->SetLanguageMode(FindLanguageMode(languageMode), true);
    }

    // update tab label and tooltip
    document->RefreshTabState();
    win->SortTabBar();

    if (!bgOpen) {
        document->RaiseDocument();
    }

    /* Bring the title bar and statistics line up to date, doOpen does
       not necessarily set the window title or read-only status */
    win->UpdateWindowTitle(document);
    win->UpdateWindowReadOnly(document);

    document->UpdateStatsLine(nullptr);

    // Add the name to the convenience menu of previously opened files
    QString fullname = tr("%1%2").arg(path, name);

    if (GetPrefAlwaysCheckRelTagsSpecs()) {
        AddRelTagsFileEx(GetPrefTagFile(), path, TAG);
    }

    MainWindow::AddToPrevOpenMenu(fullname);
    return document;
}

//------------------------------------------------------------------------------
// Name: DocumentWidget
//------------------------------------------------------------------------------
DocumentWidget::DocumentWidget(const QString &name, QWidget *parent, Qt::WindowFlags f) : QWidget(parent, f) {

	ui.setupUi(this);

    ui.labelFileAndSize->setElideMode(Qt::ElideLeft);

	buffer_ = new TextBuffer();
    buffer_->BufAddModifyCB(SyntaxHighlightModifyCBEx, this);

	// create the text widget
	splitter_ = new QSplitter(Qt::Vertical, this);
	splitter_->setChildrenCollapsible(false);
	ui.verticalLayout->addWidget(splitter_);
	ui.verticalLayout->setContentsMargins(0, 0, 0, 0);

	// initialize the members
	multiFileReplSelected_ = false;
	multiFileBusy_         = false;
	fileChanged_           = false;
	fileMissing_           = true;
	fileMode_              = 0;
	fileUid_               = 0;
	fileGid_               = 0;
	filenameSet_           = false;
	fileFormat_            = UNIX_FILE_FORMAT;
	lastModTime_           = 0;
	filename_              = name;
	undo_                  = std::list<UndoInfo *>();
	redo_                  = std::list<UndoInfo *>();
	autoSaveCharCount_     = 0;
	autoSaveOpCount_       = 0;
	undoMemUsed_           = 0;
	
	lockReasons_.clear();
	
	indentStyle_           = GetPrefAutoIndent(PLAIN_LANGUAGE_MODE);
	autoSave_              = GetPrefAutoSave();
	saveOldVersion_        = GetPrefSaveOldVersion();
	wrapMode_              = GetPrefWrap(PLAIN_LANGUAGE_MODE);
	overstrike_            = false;
    showMatchingStyle_     = static_cast<ShowMatchingStyle>(GetPrefShowMatching());
	matchSyntaxBased_      = GetPrefMatchSyntaxBased();
	highlightSyntax_       = GetPrefHighlightSyntax();
	backlightCharTypes_    = QString();
	backlightChars_        = GetPrefBacklightChars();
	
	if (backlightChars_) {
        QString cTypes = GetPrefBacklightCharTypes();
        if (!cTypes.isNull() && backlightChars_) {
            backlightCharTypes_ = cTypes;
		}
	}
	
	modeMessageDisplayed_  = false;
	ignoreModify_          = false;

    flashTimer_            = new QTimer(this);
    contextMenu_           = nullptr;
	fontName_              = GetPrefFontName();
	italicFontName_        = GetPrefItalicFontName();
	boldFontName_          = GetPrefBoldFontName();
	boldItalicFontName_    = GetPrefBoldItalicFontName();
	dialogColors_          = nullptr;

    fontStruct_            = GetPrefDefaultFont();
	italicFontStruct_      = GetPrefItalicFont();
	boldFontStruct_        = GetPrefBoldFont();
	boldItalicFontStruct_  = GetPrefBoldItalicFont();

    dialogFonts_           = nullptr;
	nMarks_                = 0;
	highlightData_         = nullptr;
	shellCmdData_          = nullptr;
	macroCmdData_          = nullptr;
	smartIndentData_       = nullptr;
	languageMode_          = PLAIN_LANGUAGE_MODE;
	device_                = 0;
    inode_                 = 0;

    // TODO(eteran): inherit from current value, not default state?
    showStats_             = GetPrefStatsLine();
	
    ShowStatsLine(showStats_);

    flashTimer_->setInterval(1500);
    flashTimer_->setSingleShot(true);
    connect(flashTimer_, SIGNAL(timeout()), this, SLOT(flashTimerTimeout()));

    auto area = createTextArea(buffer_);
    static int n = 0;
    area->setObjectName(tr("TextArea_%1").arg(n++));
    splitter_->addWidget(area);
}

//------------------------------------------------------------------------------
// Name: createTextArea
//------------------------------------------------------------------------------
TextArea *DocumentWidget::createTextArea(TextBuffer *buffer) {

    int P_marginWidth  = 5;
    int P_marginHeight = 5;
    int lineNumCols    = 0;
    int marginWidth    = 0;
    int charWidth      = 0;
    int l = P_marginWidth + ((lineNumCols == 0) ? 0 : marginWidth + charWidth * lineNumCols);
    int h = P_marginHeight;

    auto area = new TextArea(this,
                             l,
                             h,
                             100,
                             100,
                             0,
                             0,
                             buffer,
                             GetPrefDefaultFont(),
                             Qt::white,
                             Qt::black,
                             Qt::white,
                             Qt::blue,
                             Qt::black, //QColor highlightFGPixel,
                             Qt::black, //QColor highlightBGPixel,
                             Qt::black, //QColor cursorFGPixel,
                             Qt::black  //QColor lineNumFGPixel,
                             );

    QColor textFgPix   = AllocColor(GetPrefColorName(TEXT_FG_COLOR));
    QColor textBgPix   = AllocColor(GetPrefColorName(TEXT_BG_COLOR));
    QColor selectFgPix = AllocColor(GetPrefColorName(SELECT_FG_COLOR));
    QColor selectBgPix = AllocColor(GetPrefColorName(SELECT_BG_COLOR));
    QColor hiliteFgPix = AllocColor(GetPrefColorName(HILITE_FG_COLOR));
    QColor hiliteBgPix = AllocColor(GetPrefColorName(HILITE_BG_COLOR));
    QColor lineNoFgPix = AllocColor(GetPrefColorName(LINENO_FG_COLOR));
    QColor cursorFgPix = AllocColor(GetPrefColorName(CURSOR_FG_COLOR));

    area->TextDSetColors(
        textFgPix,
        textBgPix,
        selectFgPix,
        selectBgPix,
        hiliteFgPix,
        hiliteBgPix,
        lineNoFgPix,
        cursorFgPix);

    // add focus, drag, cursor tracking, and smart indent callbacks
    area->addCursorMovementCallback(movedCB, this);
    area->addDragStartCallback(dragStartCB, this);
    area->addDragEndCallback(dragEndCB, this);
    area->addSmartIndentCallback(smartIndentCB, this);


    // NOTE(eteran): we kinda cheat here. We want to have a custom context menu
    // but we don't want it to fire if the user is pressing Ctrl when they right click
    // so TextArea captures the default context menu event, then if appropriate
    // fires off a customContextMenuRequested event manually. So no need to set the
    // policy here, in fact, that would break things.
    //area->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(area, SIGNAL(customContextMenuRequested(const QPoint &)), SLOT(customContextMenuRequested(const QPoint &)));

    buffer_->BufAddModifyCB(modifiedCB, this);

    // Set the requested hardware tab distance and useTabs in the text buffer
    buffer_->BufSetTabDistance(GetPrefTabDist(PLAIN_LANGUAGE_MODE));
    buffer_->useTabs_ = GetPrefInsertTabs();

    return area;
}

MainWindow *DocumentWidget::toWindow() const {
    return qobject_cast<MainWindow *>(window());
}

/*
** Change the window appearance and the window data structure to show
** that the file it contains has been modified
*/
void DocumentWidget::SetWindowModified(bool modified) {
    if(auto win = toWindow()) {
		if (!fileChanged_ && modified) {
			win->ui.action_Close->setEnabled(true);
			fileChanged_ = true;
			win->UpdateWindowTitle(this);
			RefreshTabState();
		} else if (fileChanged_ && !modified) {
			fileChanged_ = false;
			win->UpdateWindowTitle(this);
			RefreshTabState();
		}
	}
}

/*
** update the tab label, etc. of a tab, per the states of it's document.
*/
void DocumentWidget::RefreshTabState() {

    if(auto w = toWindow()) {
        QTabWidget *tabWidget = w->ui.tabWidget;
		int index = tabWidget->indexOf(this);

		QString labelString;

		/* Set tab label to document's filename. Position of
		   "*" (modified) will change per label alignment setting */

        // TODO(eteran): for version 2.0, consider switching the "*" here
        //               with a "disk" icon, which is more conventional
        //               and more robust with regard to themeing
        QStyle *style = tabWidget->tabBar()->style();
		int alignment = style->styleHint(QStyle::SH_TabBar_Alignment);

		if (alignment != Qt::AlignRight) {
			labelString = tr("%1%2").arg(fileChanged_ ? tr("*") : tr("")).arg(filename_);
		} else {
			labelString = tr("%1%2").arg(filename_).arg(fileChanged_ ? tr("*") : tr(""));
		}

		tabWidget->setTabText(index, labelString);

		QString tipString;
		if (GetPrefShowPathInWindowsMenu() && filenameSet_) {
			tipString = tr("%1  - %2").arg(labelString, path_);
		} else {
			tipString = labelString;
		}

        tabWidget->setTabToolTip(index, tipString);
	}
}

/*
** Apply language mode matching criteria and set languageMode_ to
** the appropriate mode for the current file, trigger language mode
** specific actions (turn on/off highlighting), and update the language
** mode menu item.  If forceNewDefaults is true, re-establish default
** settings for language-specific preferences regardless of whether
** they were previously set by the user.
*/
void DocumentWidget::DetermineLanguageMode(bool forceNewDefaults) {
	SetLanguageMode(matchLanguageMode(), forceNewDefaults);
}

/*
** Set the language mode for the window, update the menu and trigger language
** mode specific actions (turn on/off highlighting).  If forceNewDefaults is
** true, re-establish default settings for language-specific preferences
** regardless of whether they were previously set by the user.
*/
void DocumentWidget::SetLanguageMode(int mode, bool forceNewDefaults) {

    // Do mode-specific actions
    reapplyLanguageMode(mode, forceNewDefaults);

	// Select the correct language mode in the sub-menu
    if (IsTopDocument()) {
        if(auto win = toWindow()) {
            QList<QAction *> languages = win->ui.action_Language_Mode->menu()->actions();
            for(QAction *action : languages) {
                if(action->data().value<int>() == mode) {
                    action->setChecked(true);
                    break;
                }
            }
        }
	}
}

/*
** Find and return the name of the appropriate languange mode for
** the file in "window".  Returns a pointer to a string, which will
** remain valid until a change is made to the language modes list.
*/
int DocumentWidget::matchLanguageMode() {

	/*... look for an explicit mode statement first */

	// Do a regular expression search on for recognition pattern
    const std::string first200 = buffer_->BufGetRangeEx(0, 200);
    for (int i = 0; i < LanguageModes.size(); i++) {
        if (!LanguageModes[i].recognitionExpr.isNull()) {
            int beginPos;
            int endPos;

            if (SearchString(first200, LanguageModes[i].recognitionExpr, SEARCH_FORWARD, SEARCH_REGEX, false, 0, &beginPos, &endPos, nullptr, nullptr, nullptr)) {
				return i;
			}
		}
	}

	/* Look at file extension ("@@/" starts a ClearCase version extended path,
	   which gets appended after the file extension, and therefore must be
	   stripped off to recognize the extension to make ClearCase users happy) */
    int fileNameLen = filename_.size();

    const int versionExtendedPathIndex = GetClearCaseVersionExtendedPathIndex(filename_);
	if (versionExtendedPathIndex != -1) {
		fileNameLen = versionExtendedPathIndex;
	}

    for (int i = 0; i < LanguageModes.size(); i++) {
        for(const QString &ext : LanguageModes[i].extensions) {

            if(filename_.midRef(0, fileNameLen).endsWith(ext)) {
                return i;
            }
		}
	}

	// no appropriate mode was found
	return PLAIN_LANGUAGE_MODE;
}

/*
** Update the optional statistics line.
*/
void DocumentWidget::UpdateStatsLine(TextArea *area) {

    if (!IsTopDocument()) {
        return;
    }

    /* This routine is called for each character typed, so its performance
       affects overall editor perfomance.  Only update if the line is on. */
    if (!showStats_) {
        return;
    }

    if(auto win = toWindow()) {
        if(!area) {
            area = win->lastFocus_;
            if(!area) {
                area = firstPane();
            }
		}

		// Compose the string to display. If line # isn't available, leave it off
		int pos = area->TextGetCursorPos();

		QString format;
		switch(fileFormat_) {
		case DOS_FILE_FORMAT:
			format = tr(" DOS");
			break;
		case MAC_FILE_FORMAT:
			format = tr(" Mac");
			break;
		default:
			format = tr("");
			break;
		}

		QString string;
		QString slinecol;
		int line;
		int colNum;

		if (!area->TextDPosToLineAndCol(pos, &line, &colNum)) {
			string   = tr("%1%2%3 %4 bytes").arg(path_, filename_, format).arg(buffer_->BufGetLength());
			slinecol = tr("L: ---  C: ---");
		} else {
			slinecol = tr("L: %1  C: %2").arg(line).arg(colNum);
			if (win->showLineNumbers_) {
				string = tr("%1%2%3 byte %4 of %5").arg(path_, filename_, format).arg(pos).arg(buffer_->BufGetLength());
			} else {
				string = tr("%1%2%3 %4 bytes").arg(path_, filename_, format).arg(buffer_->BufGetLength());
			}
		}

		// Update the line/column number
		ui.labelStats->setText(slinecol);

		// Don't clobber the line if there's a special message being displayed
		if(!modeMessageDisplayed_) {
			ui.labelFileAndSize->setText(string);
		}
	}
}

void DocumentWidget::movedCallback(TextArea *area) {

	if (ignoreModify_) {
		return;
	}

	// update line and column nubers in statistics line
	UpdateStatsLine(area);

	// Check the character before the cursor for matchable characters
    FlashMatchingEx(this, area);

	// Check for changes to read-only status and/or file modifications
    CheckForChangesToFileEx();

	/*  This callback is not only called for focussed panes, but for newly
		created panes as well. So make sure that the cursor is left alone
		for unfocussed panes.
		TextWidget have no state per se about focus, so we use the related
		ID for the blink procedure.  */
    if(QTimer *const blinkTimer = area->cursorBlinkTimer()) {
        if(!blinkTimer->isActive()) {
            //  Start blinking the caret again.
            blinkTimer->start();
        }
    }
}

void DocumentWidget::dragStartCallback(TextArea *area) {
	Q_UNUSED(area);
	// don't record all of the intermediate drag steps for undo
	ignoreModify_ = true;
}

/*
** Keep the marks in the windows book-mark table up to date across
** changes to the underlying buffer
*/
void DocumentWidget::UpdateMarkTable(int pos, int nInserted, int nDeleted) {

    for (int i = 0; i < nMarks_; i++) {
        maintainSelection(&markTable_[i].sel,       pos, nInserted, nDeleted);
        maintainPosition (&markTable_[i].cursorPos, pos, nInserted, nDeleted);
    }
}

/*
**
*/
void DocumentWidget::modifiedCallback(int pos, int nInserted, int nDeleted, int nRestyled, view::string_view deletedText) {

    Q_UNUSED(nRestyled);

    bool selected = buffer_->primary_.selected;

    // update the table of bookmarks
    if (!ignoreModify_) {
        UpdateMarkTable(pos, nInserted, nDeleted);
    }

    if(auto win = toWindow()) {
        // Check and dim/undim selection related menu items
        if ((win->wasSelected_ && !selected) || (!win->wasSelected_ && selected)) {
            win->wasSelected_ = selected;

            /* do not refresh shell-level items (window, menu-bar etc)
               when motifying non-top document */
            if (IsTopDocument()) {
                win->ui.action_Print_Selection->setEnabled(selected);
                win->ui.action_Cut->setEnabled(selected);
                win->ui.action_Copy->setEnabled(selected);
                win->ui.action_Delete->setEnabled(selected);

                /* Note we don't change the selection for items like
                   "Open Selected" and "Find Selected".  That's because
                   it works on selections in external applications.
                   Desensitizing it if there's no NEdit selection
                   disables this feature. */

                win->ui.action_Filter_Selection->setEnabled(selected);

                DimSelectionDepUserMenuItems(selected);

                if(auto dialog = win->getDialogReplace()) {
                    dialog->UpdateReplaceActionButtons();
                }
            }

        }

        /* When the program needs to make a change to a text area without without
           recording it for undo or marking file as changed it sets ignoreModify */
        if (ignoreModify_ || (nDeleted == 0 && nInserted == 0)) {
            return;
        }

        // Make sure line number display is sufficient for new data
        win->updateLineNumDisp();

        /* Save information for undoing this operation (this call also counts
           characters and editing operations for triggering autosave */
        SaveUndoInformation(pos, nInserted, nDeleted, deletedText);

        // Trigger automatic backup if operation or character limits reached
        if (autoSave_ && (autoSaveCharCount_ > AUTOSAVE_CHAR_LIMIT || autoSaveOpCount_ > AUTOSAVE_OP_LIMIT)) {
            WriteBackupFile();
            autoSaveCharCount_ = 0;
            autoSaveOpCount_   = 0;
        }

        // Indicate that the window has now been modified
        SetWindowModified(true);

        // Update # of bytes, and line and col statistics
        UpdateStatsLine(nullptr);

        // Check if external changes have been made to file and warn user
        CheckForChangesToFileEx();

    }
}

void DocumentWidget::dragEndCallback(TextArea *area, dragEndCBStruct *data) {

    // NOTE(eteran): so, it's a minor shame here that we don't use the area
    // variable. The **buffer** is what reports the modifications, and that is
    // lower level than a TextArea. So it doesn't pass that along to the other
    // versions of the modified callback. The code will eventually call
    // "UpdateStatsLine(nullptr)", which will force it to fall back on the
    // "lastFocus_" variable. Which **probably** points to the correct widget.
    // TODO(eteran): improve this somehow!
    Q_UNUSED(area);

	// restore recording of undo information
	ignoreModify_ = false;

	// Do nothing if drag operation was canceled
    if (data->nCharsInserted == 0) {
		return;
    }

	/* Save information for undoing this operation not saved while
	   undo recording was off */
    modifiedCallback(data->startPos, data->nCharsInserted, data->nCharsDeleted, 0, data->deletedText);
}

void DocumentWidget::smartIndentCallback(TextArea *area, smartIndentCBStruct *data) {

    Q_UNUSED(area);

	if (!smartIndentData_) {
		return;
	}

	switch(data->reason) {
	case CHAR_TYPED:
        executeModMacroEx(data);
		break;
	case NEWLINE_INDENT_NEEDED:
        executeNewlineMacroEx(data);
        break;
	}
}

/*
** raise the document and its shell window and optionally focus.
*/
void DocumentWidget::RaiseFocusDocumentWindow(bool focus) {
    RaiseDocument();
    if(auto win = toWindow()) {

        if(!win->isMaximized()) {
            win->showNormal();
        }

        if(focus) {
            win->activateWindow();
            //setFocus();
        }
    }
}

/**
 * @brief DocumentWidget::RaiseDocumentWindow
 * raise the document and its shell window and focus depending on pref.
 */
void DocumentWidget::RaiseDocumentWindow() {
    RaiseFocusDocumentWindow(GetPrefFocusOnRaise());
}

void DocumentWidget::documentRaised() {
    // Turn on syntax highlight that might have been deferred.
    if (highlightSyntax_ && !highlightData_) {
        StartHighlightingEx(this, false);
    }

    RefreshTabState();

    /* now refresh this state/info. RefreshWindowStates()
       has a lot of work to do, so we update the screen first so
       the document appears to switch swiftly. */
    RefreshWindowStates();

    RefreshTabState();

    /* Make sure that the "In Selection" button tracks the presence of a
       selection and that the this inherits the proper search scope. */
    if(auto win = toWindow()) {
        if(auto dialog = win->getDialogReplace()) {
            dialog->UpdateReplaceActionButtons();
        }
    }

#if 0
    UpdateWMSizeHints();
#endif
}

void DocumentWidget::RaiseDocument() {
    if(auto win = toWindow()) {

        // TODO(eteran): this used to do some tracking for the last active
        //               window/document here. I think there is likely a better
        //               approach


        // set the document as top document
        // show the new top document
        // NOTE(eteran): indirectly triggers a call to documentRaised()
        win->ui.tabWidget->setCurrentWidget(this);
    }
}

/*
** Change the language mode to the one indexed by "mode", reseting word
** delimiters, syntax highlighting and other mode specific parameters
*/
void DocumentWidget::reapplyLanguageMode(int mode, bool forceDefaults) {

    if(auto win = toWindow()) {
        const int oldMode = languageMode_;

        /* If the mode is the same, and changes aren't being forced (as might
           happen with Save As...), don't mess with already correct settings */
        if (languageMode_ == mode && !forceDefaults) {
            return;
        }

        // Change the mode name stored in the window
        languageMode_ = mode;

        // Decref oldMode's default calltips file if needed
        if (oldMode != PLAIN_LANGUAGE_MODE && !LanguageModes[oldMode].defTipsFile.isNull()) {
            DeleteTagsFileEx(LanguageModes[oldMode].defTipsFile, TIP, false);
        }

        // Set delimiters for all text widgets
        QString delimiters;
        if (mode == PLAIN_LANGUAGE_MODE || LanguageModes[mode].delimiters.isNull()) {
            delimiters = GetPrefDelimiters();
        } else {
            delimiters = LanguageModes[mode].delimiters;
        }

        const QList<TextArea *> textAreas = textPanes();
        for(TextArea *area : textAreas) {
            area->setWordDelimiters(delimiters);
        }

        /* Decide on desired values for language-specific parameters.  If a
           parameter was set to its default value, set it to the new default,
           otherwise, leave it alone */
        bool wrapModeIsDef = wrapMode_ == GetPrefWrap(oldMode);
        bool tabDistIsDef = buffer_->BufGetTabDistance() == GetPrefTabDist(oldMode);

        int oldEmTabDist = textAreas[0]->getEmulateTabs();
        QString oldlanguageModeName = LanguageModeName(oldMode);

        bool emTabDistIsDef     = oldEmTabDist == GetPrefEmTabDist(oldMode);
        bool indentStyleIsDef   = indentStyle_ == GetPrefAutoIndent(oldMode)   || (GetPrefAutoIndent(oldMode) == SMART_INDENT && indentStyle_ == AUTO_INDENT && !SmartIndentMacrosAvailable(LanguageModeName(oldMode)));
        bool highlightIsDef     = highlightSyntax_ == GetPrefHighlightSyntax() || (GetPrefHighlightSyntax() && FindPatternSet(!oldlanguageModeName.isNull() ? oldlanguageModeName : QLatin1String("")) == nullptr);
        int wrapMode            = wrapModeIsDef                                || forceDefaults ? GetPrefWrap(mode)        : wrapMode_;
        int tabDist             = tabDistIsDef                                 || forceDefaults ? GetPrefTabDist(mode)     : buffer_->BufGetTabDistance();
        int emTabDist           = emTabDistIsDef                               || forceDefaults ? GetPrefEmTabDist(mode)   : oldEmTabDist;
        IndentStyle indentStyle = indentStyleIsDef                             || forceDefaults ? GetPrefAutoIndent(mode)  : indentStyle_;
        int highlight           = highlightIsDef                               || forceDefaults ? GetPrefHighlightSyntax() : highlightSyntax_;

        /* Dim/undim smart-indent and highlighting menu items depending on
           whether patterns/macros are available */
        QString languageModeName   = LanguageModeName(mode);
        bool haveHighlightPatterns = FindPatternSet(languageModeName);
        bool haveSmartIndentMacros = SmartIndentMacrosAvailable(LanguageModeName(mode));

        if (IsTopDocument()) {
            win->ui.action_Highlight_Syntax->setEnabled(haveHighlightPatterns);
            win->ui.action_Indent_Smart    ->setEnabled(haveSmartIndentMacros);
        }

        // Turn off requested options which are not available
        highlight = haveHighlightPatterns && highlight;
        if (indentStyle == SMART_INDENT && !haveSmartIndentMacros) {
            indentStyle = AUTO_INDENT;
        }

        // Change highlighting
        highlightSyntax_ = highlight;

        no_signals(win->ui.action_Highlight_Syntax)->setChecked(highlight);

        StopHighlightingEx();

        // we defer highlighting to RaiseDocument() if doc is hidden
        if (IsTopDocument() && highlight) {
            StartHighlightingEx(this, false);
        }

        // Force a change of smart indent macros (SetAutoIndent will re-start)
        if (indentStyle_ == SMART_INDENT) {
            EndSmartIndentEx(this);
            indentStyle_ = AUTO_INDENT;
        }

        // set requested wrap, indent, and tabs
        SetAutoWrap(wrapMode);
        SetAutoIndent(indentStyle);
        SetTabDist(tabDist);
        SetEmTabDist(emTabDist);

        // Load calltips files for new mode
        if (mode != PLAIN_LANGUAGE_MODE && !LanguageModes[mode].defTipsFile.isNull()) {
            AddTagsFileEx(LanguageModes[mode].defTipsFile, TIP);
        }

        // Add/remove language specific menu items
        win->UpdateUserMenus(this);
    }
}

/*
**
*/
void DocumentWidget::SetTabDist(int tabDist) {
    if (buffer_->tabDist_ != tabDist) {
        int saveCursorPositions[MAX_PANES + 1];
        int saveVScrollPositions[MAX_PANES + 1];
        int saveHScrollPositions[MAX_PANES + 1];

        ignoreModify_ = true;

        const QList<TextArea *> textAreas = textPanes();
        for(int paneIndex = 0; paneIndex < textAreas.size(); ++paneIndex) {
            TextArea *area = textAreas[paneIndex];

            area->TextDGetScroll(&saveVScrollPositions[paneIndex], &saveHScrollPositions[paneIndex]);
            saveCursorPositions[paneIndex] = area->TextGetCursorPos();
            area->setModifyingTabDist(1);
        }

        buffer_->BufSetTabDistance(tabDist);

        for(int paneIndex = 0; paneIndex < textAreas.size(); ++paneIndex) {
            TextArea *area = textAreas[paneIndex];

            area->setModifyingTabDist(0);
            area->TextSetCursorPos(saveCursorPositions[paneIndex]);
            area->TextDSetScroll(saveVScrollPositions[paneIndex], saveHScrollPositions[paneIndex]);
        }

        ignoreModify_ = false;
    }
}

/*
**
*/
void DocumentWidget::SetEmTabDist(int emTabDist) {

    const QList<TextArea *> textAreas = textPanes();
    for(TextArea *area : textAreas) {
        area->setEmulateTabs(emTabDist);
    }
}

/*
** Set autoindent state to one of  NO_AUTO_INDENT, AUTO_INDENT, or SMART_INDENT.
*/
void DocumentWidget::SetAutoIndent(IndentStyle state) {
    bool autoIndent  = (state == AUTO_INDENT);
    bool smartIndent = (state == SMART_INDENT);

    if (indentStyle_ == SMART_INDENT && !smartIndent) {
        EndSmartIndentEx(this);
    } else if (smartIndent && indentStyle_ != SMART_INDENT) {
        BeginSmartIndentEx(true);
    }

    indentStyle_ = state;

    const QList<TextArea *> textAreas = textPanes();
    for(TextArea *area : textAreas) {
        area->setAutoIndent(autoIndent);
        area->setSmartIndent(smartIndent);
    }

    if (IsTopDocument()) {
        if(auto win = toWindow()) {
            no_signals(win->ui.action_Indent_Smart)->setChecked(smartIndent);
            no_signals(win->ui.action_Indent_On)->setChecked(autoIndent);
            no_signals(win->ui.action_Indent_Off)->setChecked(state == NO_AUTO_INDENT);
        }
    }
}

/*
** Select auto-wrap mode, one of NO_WRAP, NEWLINE_WRAP, or CONTINUOUS_WRAP
*/
void DocumentWidget::SetAutoWrap(int state) {
    const bool autoWrap = (state == NEWLINE_WRAP);
    const bool contWrap = (state == CONTINUOUS_WRAP);

    const QList<TextArea *> textAreas = textPanes();
    for(TextArea *area : textAreas) {
        area->setAutoWrap(autoWrap);
        area->setContinuousWrap(contWrap);
    }

    wrapMode_ = state;

    if (IsTopDocument()) {
        if(auto win = toWindow()) {
            no_signals(win->ui.action_Wrap_Auto_Newline)->setChecked(autoWrap);
            no_signals(win->ui.action_Wrap_Continuous)->setChecked(contWrap);
            no_signals(win->ui.action_Wrap_None)->setChecked(state == NO_WRAP);
        }
    }
}

int DocumentWidget::textPanesCount() const {
    return splitter_->count();
}

QList<TextArea *> DocumentWidget::textPanes() const {

    QList<TextArea *> list;
    list.reserve(splitter_->count());

    for(int i = 0; i < splitter_->count(); ++i) {
        if(auto area = qobject_cast<TextArea *>(splitter_->widget(i))) {
            list.push_back(area);
        }
    }

    return list;
}

bool DocumentWidget::IsTopDocument() const {
    if(auto window = toWindow()) {
        return window->currentDocument() == this;
    }

    return false;
}



QString DocumentWidget::getWindowsMenuEntry() {

    QString fullTitle = tr("%1%2").arg(filename_).arg(fileChanged_ ? tr("*") : tr(""));

    if (GetPrefShowPathInWindowsMenu() && filenameSet_) {
        fullTitle.append(tr(" - "));
        fullTitle.append(path_);
    }

    return fullTitle;
}

void DocumentWidget::setLanguageMode(const QString &mode) {

    // TODO(eteran): this is inefficient, we started with the mode number
    //               converted it to a string, and now we look up the number
    //               again to pass to this function. We should just pass the
    //               number and skip the round trip
    SetLanguageMode(FindLanguageMode(mode), false);
}

/*
** Turn off syntax highlighting and free style buffer, compiled patterns, and
** related data.
*/
void DocumentWidget::StopHighlightingEx() {
    if (!highlightData_) {
        return;
    }

    if(TextArea *area = firstPane()) {

        /* Get the line height being used by the highlight fonts in the window,
           to be used after highlighting is turned off to resize the window
           back to the line height of the primary font */
        int oldFontHeight = area->getFontHeight();

        // Free and remove the highlight data from the window
        freeHighlightData(static_cast<WindowHighlightData *>(highlightData_));
        highlightData_ = nullptr;

        /* Remove and detach style buffer and style table from all text
           display(s) of window, and redisplay without highlighting */
        const QList<TextArea *> textAreas = textPanes();
        for(TextArea *area : textAreas) {
            RemoveWidgetHighlightEx(area);
        }

        Q_UNUSED(oldFontHeight);
    #if 0
        /* Re-size the window to fit the primary font properly & tell the window
           manager about the potential line-height change as well */
        updateWindowHeight(window, oldFontHeight);
        window->UpdateWMSizeHints();
        window->UpdateMinPaneHeights();
    #endif
    }
}

TextArea *DocumentWidget::firstPane() const {

    for(int i = 0; i < splitter_->count(); ++i) {
        if(auto area = qobject_cast<TextArea *>(splitter_->widget(i))) {
            return area;
        }
    }

    return nullptr;
}

/*
** Free allocated memory associated with highlight data, including compiled
** regular expressions, style buffer and style table.  Note: be sure to
** nullptr out the widget references to the objects in this structure before
** calling this.  Because of the slow, multi-phase destruction of
** widgets, this data can be referenced even AFTER destroying the widget.
*/
void DocumentWidget::freeHighlightData(WindowHighlightData *hd) {

    if(hd) {
        freePatterns(hd->pass1Patterns);
        freePatterns(hd->pass2Patterns);
        delete hd->styleBuffer;
        delete[] hd->styleTable;
        delete hd;
    }
}

/*
** Free a pattern list and all of its allocated components
*/
void DocumentWidget::freePatterns(HighlightData *patterns) {

    if(patterns) {
        for (int i = 0; patterns[i].style != 0; i++) {
            delete patterns[i].startRE;
            delete patterns[i].endRE;
            delete patterns[i].errorRE;
            delete patterns[i].subPatternRE;
        }

        for (int i = 0; patterns[i].style != 0; i++) {
            delete [] patterns[i].subPatterns;
        }

        delete [] patterns;
    }
}

/*
** Get the set of word delimiters for the language mode set in the current
** window.  Returns nullptr when no language mode is set (it would be easy to
** return the default delimiter set when the current language mode is "Plain",
** or the mode doesn't have its own delimiters, but this is usually used
** to supply delimiters for RE searching, and ExecRE can skip compiling a
** delimiter table when delimiters is nullptr).
*/
QString DocumentWidget::GetWindowDelimiters() const {
    if (languageMode_ == PLAIN_LANGUAGE_MODE)
        return QString();
    else
        return LanguageModes[languageMode_].delimiters;
}

void DocumentWidget::flashTimerTimeout() {
    eraseFlashEx(this);
}

/*
** Dim/undim user programmable menu items which depend on there being
** a selection in their associated window.
*/
void DocumentWidget::DimSelectionDepUserMenuItems(bool enabled) {

    if (!IsTopDocument()) {
        return;
    }

    if(auto win = toWindow()) {
        dimSelDepItemsInMenu(win->ui.menu_Shell, ShellMenuData, enabled);
        dimSelDepItemsInMenu(win->ui.menu_Macro, MacroMenuData, enabled);
        dimSelDepItemsInMenu(contextMenu_,       BGMenuData,    enabled);

    }
}

void DocumentWidget::dimSelDepItemsInMenu(QMenu *menuPane, const QVector<MenuData> &menuList, bool enabled) {

    if(menuPane) {
        const QList<QAction *> actions = menuPane->actions();
        for(QAction *action : actions) {
            if(QMenu *subMenu = action->menu()) {
                dimSelDepItemsInMenu(subMenu, menuList, enabled);
            } else {
                int index = action->data().value<int>();
                if (index < 0 || index >= menuList.size()) {
                    return;
                }

                if (menuList[index].item->input == FROM_SELECTION) {
                    action->setEnabled(enabled);
                }
            }
        }
    }
}

/*
** SaveUndoInformation stores away the changes made to the text buffer.  As a
** side effect, it also increments the autoSave operation and character counts
** since it needs to do the classification anyhow.
**
** Note: This routine must be kept efficient.  It is called for every
**       character typed.
*/
void DocumentWidget::SaveUndoInformation(int pos, int nInserted, int nDeleted, view::string_view deletedText) {

    const int isUndo = (!undo_.empty() && undo_.front()->inUndo);
    const int isRedo = (!redo_.empty() && redo_.front()->inUndo);

    /* redo operations become invalid once the user begins typing or does
       other editing.  If this is not a redo or undo operation and a redo
       list still exists, clear it and dim the redo menu item */
    if (!(isUndo || isRedo) && !redo_.empty()) {
        ClearRedoList();
    }

    /* figure out what kind of editing operation this is, and recall
       what the last one was */
    const UndoTypes newType = determineUndoType(nInserted, nDeleted);
    if (newType == UNDO_NOOP) {
        return;
    }

    UndoInfo *const currentUndo = undo_.empty() ? nullptr : undo_.front();

    const UndoTypes oldType = (!currentUndo || isUndo) ? UNDO_NOOP : currentUndo->type;

    /*
    ** Check for continuations of single character operations.  These are
    ** accumulated so a whole insertion or deletion can be undone, rather
    ** than just the last character that the user typed.  If the this
    ** is currently in an unmodified state, don't accumulate operations
    ** across the save, so the user can undo back to the unmodified state.
    */
    if (fileChanged_) {

        // normal sequential character insertion
        if (((oldType == ONE_CHAR_INSERT || oldType == ONE_CHAR_REPLACE) && newType == ONE_CHAR_INSERT) && (pos == currentUndo->endPos)) {
            currentUndo->endPos++;
            autoSaveCharCount_++;
            return;
        }

        // overstrike mode replacement
        if ((oldType == ONE_CHAR_REPLACE && newType == ONE_CHAR_REPLACE) && (pos == currentUndo->endPos)) {
            appendDeletedText(deletedText, nDeleted, FORWARD);
            currentUndo->endPos++;
            autoSaveCharCount_++;
            return;
        }

        // forward delete
        if ((oldType == ONE_CHAR_DELETE && newType == ONE_CHAR_DELETE) && (pos == currentUndo->startPos)) {
            appendDeletedText(deletedText, nDeleted, FORWARD);
            return;
        }

        // reverse delete
        if ((oldType == ONE_CHAR_DELETE && newType == ONE_CHAR_DELETE) && (pos == currentUndo->startPos - 1)) {
            appendDeletedText(deletedText, nDeleted, REVERSE);
            currentUndo->startPos--;
            currentUndo->endPos--;
            return;
        }
    }

    /*
    ** The user has started a new operation, create a new undo record
    ** and save the new undo data.
    */
    auto undo = new UndoInfo(newType, pos, pos + nInserted);

    // if text was deleted, save it
    if (nDeleted > 0) {
        undo->oldLen = nDeleted + 1; // +1 is for null at end
        undo->oldText = deletedText.to_string();
    }

    // increment the operation count for the autosave feature
    autoSaveOpCount_++;

    /* if the this is currently unmodified, remove the previous
       restoresToSaved marker, and set it on this record */
    if (!fileChanged_) {
        undo->restoresToSaved = true;

        for(UndoInfo *u : undo_) {
            u->restoresToSaved = false;
        }

        for(UndoInfo *u : redo_) {
            u->restoresToSaved = false;
        }
    }

    /* Add the new record to the undo list  unless SaveUndoInfo is
       saving information generated by an Undo operation itself, in
       which case, add the new record to the redo list. */
    if (isUndo) {
        addRedoItem(undo);
    } else {
        addUndoItem(undo);
    }
}

/*
** ClearUndoList, ClearRedoList
**
** Functions for clearing all of the information off of the undo or redo
** lists and adjusting the edit menu accordingly
*/
void DocumentWidget::ClearUndoList() {
    while (!undo_.empty()) {
        removeUndoItem();
    }
}
void DocumentWidget::ClearRedoList() {
    while (!redo_.empty()) {
        removeRedoItem();
    }
}

/*
** Add deleted text to the beginning or end
** of the text saved for undoing the last operation.  This routine is intended
** for continuing of a string of one character deletes or replaces, but will
** work with more than one character.
*/
void DocumentWidget::appendDeletedText(view::string_view deletedText, int deletedLen, int direction) {
    UndoInfo *undo = undo_.front();

    // re-allocate, adding space for the new character(s)
    std::string comboText;
    comboText.reserve(undo->oldLen + deletedLen);

    // copy the new character and the already deleted text to the new memory
    if (direction == FORWARD) {
        comboText.append(undo->oldText);
        comboText.append(deletedText.begin(), deletedText.end());
    } else {
        comboText.append(deletedText.begin(), deletedText.end());
        comboText.append(undo->oldText);
    }

    // keep track of the additional memory now used by the undo list
    undoMemUsed_++;

    // free the old saved text and attach the new
    undo->oldText = comboText;
    undo->oldLen += deletedLen;
}

/*
** Add an undo record (already allocated by the caller) to the this's undo
** list if the item pushes the undo operation or character counts past the
** limits, trim the undo list to an acceptable length.
*/
void DocumentWidget::addUndoItem(UndoInfo *undo) {

    // Make the undo menu item sensitive now that there's something to undo
    if (undo_.empty()) {
        if(auto win = toWindow()) {
            win->ui.action_Undo->setEnabled(true);
        }
    }

    // Add the item to the beginning of the list
    undo_.push_front(undo);

    // Increment the operation and memory counts
    undoMemUsed_ += undo->oldLen;

    // Trim the list if it exceeds any of the limits
    if (undo_.size() > UNDO_OP_LIMIT) {
        trimUndoList(UNDO_OP_TRIMTO);
    }

    if (undoMemUsed_ > UNDO_WORRY_LIMIT) {
        trimUndoList(UNDO_WORRY_TRIMTO);
    }

    if (undoMemUsed_ > UNDO_PURGE_LIMIT) {
        trimUndoList(UNDO_PURGE_TRIMTO);
    }
}

/*
** Add an item (already allocated by the caller) to the this's redo list.
*/
void DocumentWidget::addRedoItem(UndoInfo *redo) {
    // Make the redo menu item sensitive now that there's something to redo
    if (redo_.empty()) {
        if(auto win = toWindow()) {
            win->ui.action_Redo->setEnabled(true);
        }
    }

    // Add the item to the beginning of the list
    redo_.push_front(redo);
}

/*
** Pop (remove and free) the current (front) undo record from the undo list
*/
void DocumentWidget::removeUndoItem() {

    if (undo_.empty()) {
        return;
    }

    UndoInfo *undo = undo_.front();

    // Decrement the operation and memory counts
    undoMemUsed_ -= undo->oldLen;

    // Remove and free the item
    undo_.pop_front();
    delete undo;

    // if there are no more undo records left, dim the Undo menu item
    if (undo_.empty()) {
        if(auto win = toWindow()) {
            win->ui.action_Undo->setEnabled(false);
        }
    }
}

/*
** Pop (remove and free) the current (front) redo record from the redo list
*/
void DocumentWidget::removeRedoItem() {
    UndoInfo *redo = redo_.front();

    // Remove and free the item
    redo_.pop_front();
    delete redo;

    // if there are no more redo records left, dim the Redo menu item
    if (redo_.empty()) {
        if(auto win = toWindow()) {
            win->ui.action_Redo->setEnabled(false);
        }
    }
}


/*
** Trim records off of the END of the undo list to reduce it to length
** maxLength
*/
void DocumentWidget::trimUndoList(int maxLength) {

    if (undo_.empty()) {
        return;
    }

    auto it = undo_.begin();
    int i   = 1;

    // Find last item on the list to leave intact
    while(it != undo_.end() && i < maxLength) {
        ++it;
        ++i;
    }

    // Trim off all subsequent entries
    while(it != undo_.end()) {
        UndoInfo *u = *it;

        undoMemUsed_ -= u->oldLen;
        delete u;

        it = undo_.erase(it);
    }
}

void DocumentWidget::Undo() {

    if(auto win = toWindow()) {
        int restoredTextLength;

        // return if nothing to undo
        if (undo_.empty()) {
            return;
        }

        UndoInfo *undo = undo_.front();

        /* BufReplaceEx will eventually call SaveUndoInformation.  This is mostly
           good because it makes accumulating redo operations easier, however
           SaveUndoInformation needs to know that it is being called in the context
           of an undo.  The inUndo field in the undo record indicates that this
           record is in the process of being undone. */
        undo->inUndo = true;

        // use the saved undo information to reverse changes
        buffer_->BufReplaceEx(undo->startPos, undo->endPos, undo->oldText);

        restoredTextLength = undo->oldText.size();
        if (!buffer_->primary_.selected || GetPrefUndoModifiesSelection()) {
            /* position the cursor in the focus pane after the changed text
               to show the user where the undo was done */
            auto area = win->lastFocus_;
            area->TextSetCursorPos(undo->startPos + restoredTextLength);
        }

        if (GetPrefUndoModifiesSelection()) {
            if (restoredTextLength > 0) {
                buffer_->BufSelect(undo->startPos, undo->startPos + restoredTextLength);
            } else {
                buffer_->BufUnselect();
            }
        }
        MakeSelectionVisible(win->lastFocus_);

        /* restore the file's unmodified status if the file was unmodified
           when the change being undone was originally made.  Also, remove
           the backup file, since the text in the buffer is now identical to
           the original file */
        if (undo->restoresToSaved) {
            SetWindowModified(false);
            RemoveBackupFile();
        }

        // free the undo record and remove it from the chain
        removeUndoItem();
    }
}

void DocumentWidget::Redo() {

    if(auto win = toWindow()) {
        int restoredTextLength;

        // return if nothing to redo
        if (redo_.empty()) {
            return;
        }

        UndoInfo *redo = redo_.front();

        /* BufReplaceEx will eventually call SaveUndoInformation.  To indicate
           to SaveUndoInformation that this is the context of a redo operation,
           we set the inUndo indicator in the redo record */
        redo->inUndo = true;

        // use the saved redo information to reverse changes
        buffer_->BufReplaceEx(redo->startPos, redo->endPos, redo->oldText);

        restoredTextLength = redo->oldText.size();
        if (!buffer_->primary_.selected || GetPrefUndoModifiesSelection()) {
            /* position the cursor in the focus pane after the changed text
               to show the user where the undo was done */
            auto area = win->lastFocus_;
            area->TextSetCursorPos(redo->startPos + restoredTextLength);
        }
        if (GetPrefUndoModifiesSelection()) {

            if (restoredTextLength > 0) {
                buffer_->BufSelect(redo->startPos, redo->startPos + restoredTextLength);
            } else {
                buffer_->BufUnselect();
            }
        }
        MakeSelectionVisible(win->lastFocus_);

        /* restore the file's unmodified status if the file was unmodified
           when the change being redone was originally made. Also, remove
           the backup file, since the text in the buffer is now identical to
           the original file */
        if (redo->restoresToSaved) {
            SetWindowModified(false);
            RemoveBackupFile();
        }

        // remove the redo record from the chain and free it
        removeRedoItem();
    }
}

/*
** Check the read-only or locked status of the window and beep and return
** false if the window should not be written in.
*/
bool DocumentWidget::CheckReadOnly() const {
    if (lockReasons_.isAnyLocked()) {
        QApplication::beep();
        return true;
    }
    return false;
}

/*
** If the selection (or cursor position if there's no selection) is not
** fully shown, scroll to bring it in to view.  Note that as written,
** this won't work well with multi-line selections.  Modest re-write
** of the horizontal scrolling part would be quite easy to make it work
** well with rectangular selections.
*/
void DocumentWidget::MakeSelectionVisible(TextArea *area) {

    int width;
    bool isRect;
    int horizOffset;
    int lastLineNum;
    int left;
    int leftLineNum;
    int leftX;
    int linesToScroll;
    int margin;
    int rectEnd;
    int rectStart;
    int right;
    int rightLineNum;
    int rightX;
    int rows;
    int scrollOffset;
    int targetLineNum;
    int topLineNum;
    int y;

    int topChar  = area->TextFirstVisiblePos();
    int lastChar = area->TextLastVisiblePos();

    // find out where the selection is
    if (!buffer_->BufGetSelectionPos(&left, &right, &isRect, &rectStart, &rectEnd)) {
        left = right = area->TextGetCursorPos();
        isRect = false;
    }

    /* Check vertical positioning unless the selection is already shown or
       already covers the display.  If the end of the selection is below
       bottom, scroll it in to view until the end selection is scrollOffset
       lines from the bottom of the display or the start of the selection
       scrollOffset lines from the top.  Calculate a pleasing distance from the
       top or bottom of the window, to scroll the selection to (if scrolling is
       necessary), around 1/3 of the height of the window */
    if (!((left >= topChar && right <= lastChar) || (left <= topChar && right >= lastChar))) {

        rows = area->getRows();

        scrollOffset = rows / 3;
        area->TextDGetScroll(&topLineNum, &horizOffset);
        if (right > lastChar) {
            // End of sel. is below bottom of screen
            leftLineNum = topLineNum + area->TextDCountLines(topChar, left, false);
            targetLineNum = topLineNum + scrollOffset;
            if (leftLineNum >= targetLineNum) {
                // Start of sel. is not between top & target
                linesToScroll = area->TextDCountLines(lastChar, right, false) + scrollOffset;
                if (leftLineNum - linesToScroll < targetLineNum)
                    linesToScroll = leftLineNum - targetLineNum;
                // Scroll start of selection to the target line
                area->TextDSetScroll(topLineNum + linesToScroll, horizOffset);
            }
        } else if (left < topChar) {
            // Start of sel. is above top of screen
            lastLineNum = topLineNum + rows;
            rightLineNum = lastLineNum - area->TextDCountLines(right, lastChar, false);
            targetLineNum = lastLineNum - scrollOffset;
            if (rightLineNum <= targetLineNum) {
                // End of sel. is not between bottom & target
                linesToScroll = area->TextDCountLines(left, topChar, false) + scrollOffset;
                if (rightLineNum + linesToScroll > targetLineNum)
                    linesToScroll = targetLineNum - rightLineNum;
                // Scroll end of selection to the target line
                area->TextDSetScroll(topLineNum - linesToScroll, horizOffset);
            }
        }
    }

    /* If either end of the selection off screen horizontally, try to bring it
       in view, by making sure both end-points are visible.  Using only end
       points of a multi-line selection is not a great idea, and disaster for
       rectangular selections, so this part of the routine should be re-written
       if it is to be used much with either.  Note also that this is a second
       scrolling operation, causing the display to jump twice.  It's done after
       vertical scrolling to take advantage of TextDPosToXY which requires it's
       reqested position to be vertically on screen) */
    if (area->TextDPositionToXY(left, &leftX, &y) && area->TextDPositionToXY(right, &rightX, &y) && leftX <= rightX) {
        area->TextDGetScroll(&topLineNum, &horizOffset);

        margin = area->getMarginWidth();
        width  = area->width();

        if (leftX < margin + area->getLineNumLeft() + area->getLineNumWidth())
            horizOffset -= margin + area->getLineNumLeft() + area->getLineNumWidth() - leftX;
        else if (rightX > width - margin)
            horizOffset += rightX - (width - margin);

        area->TextDSetScroll(topLineNum, horizOffset);
    }

    // make sure that the statistics line is up to date
    UpdateStatsLine(area);
}



/*
** Remove the backup file associated with this window
*/
void DocumentWidget::RemoveBackupFile() {

    // Don't delete backup files when backups aren't activated.
    if (!autoSave_) {
        return;
    }

    QString name = backupFileNameEx();
    ::remove(name.toLatin1().data());
}

/*
** Generate the name of the backup file for this window from the filename
** and path in the window data structure & write into name
*/
QString DocumentWidget::backupFileNameEx() {

    if (filenameSet_) {
        return tr("%1~%2").arg(path_, filename_);
    } else {
        return PrependHomeEx(tr("~%1").arg(filename_));
    }
}

/*
** Check if the file in the window was changed by an external source.
** and put up a warning dialog if it has.
*/
void DocumentWidget::CheckForChangesToFileEx() {

    // TODO(eteran): this concept can be reworked in terms of QFileSystemWatcher
    //               but we'll leave that for 2.0

    static DocumentWidget *lastCheckWindow = nullptr;
    static qint64 lastCheckTime = 0;

    if (!filenameSet_) {
        return;
    }

    // If last check was very recent, don't impact performance
    const qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    if (this == lastCheckWindow && (timestamp - lastCheckTime) < MOD_CHECK_INTERVAL) {
        return;
    }

    lastCheckWindow = this;
    lastCheckTime   = timestamp;

    bool silent = false;

    /* Update the status, but don't pop up a dialog if we're called
       from a place where the window might be iconic (e.g., from the
       replace dialog) or on another desktop.

       This works, but I bet it costs a round-trip to the server.
       Might be better to capture MapNotify/Unmap events instead.

       For tabs that are not on top, we don't want the dialog either,
       and we don't even need to contact the server to find out. By
       performing this check first, we avoid a server round-trip for
       most files in practice. */
    if (!IsTopDocument()) {
        silent = true;
    } else {
        MainWindow *win = toWindow();
        if(!win->isVisible()) {
            silent = true;
        }
    }

    if(auto win = toWindow()) {
        // Get the file mode and modification time
        QString fullname = FullPath();

        struct stat statbuf;
        if (::stat(fullname.toLatin1().data(), &statbuf) != 0) {

            // Return if we've already warned the user or we can't warn him now
            if (fileMissing_ || silent) {
                return;
            }

            /* Can't stat the file -- maybe it's been deleted.
               The filename is now invalid */
            fileMissing_ = true;
            lastModTime_ = 1;
            device_      = 0;
            inode_       = 0;

            /* Warn the user, if they like to be warned (Maybe this should be its
                own preference setting: GetPrefWarnFileDeleted()) */
            if (GetPrefWarnFileMods()) {
                bool save = false;

                //  Set title, message body and button to match stat()'s error.
                switch (errno) {
                case ENOENT:
                    {
                        // A component of the path file_name does not exist.
                        int resp = QMessageBox::critical(this, tr("File not Found"), tr("File '%1' (or directory in its path)\nno longer exists.\nAnother program may have deleted or moved it.").arg(filename_), QMessageBox::Save | QMessageBox::Cancel);
                        save = (resp == QMessageBox::Save);
                    }
                    break;
                case EACCES:
                    {
                        // Search permission denied for a path component. We add one to the response because Re-Save wouldn't really make sense here.
                        int resp = QMessageBox::critical(this, tr("Permission Denied"), tr("You no longer have access to file '%1'.\nAnother program may have changed the permissions of one of its parent directories.").arg(filename_), QMessageBox::Save | QMessageBox::Cancel);
                        save = (resp == QMessageBox::Save);
                    }
                    break;
                default:
                    {
                        // Everything else. This hints at an internal error (eg. ENOTDIR) or at some bad state at the host.
                        int resp = QMessageBox::critical(this, tr("File not Accessible"), tr("Error while checking the status of file '%1':\n    '%2'\nPlease make sure that no data is lost before closing this window.").arg(filename_).arg(QString::fromLatin1(strerror(errno))), QMessageBox::Save | QMessageBox::Cancel);
                        save = (resp == QMessageBox::Save);
                    }
                    break;
                }

                if(save) {
                    SaveWindow();
                }
            }

            // A missing or (re-)saved file can't be read-only.
            // TODO: A document without a file can be locked though.
            // Make sure that the window was not destroyed behind our back!
            lockReasons_.setPermLocked(false);
            win->UpdateWindowTitle(this);
            win->UpdateWindowReadOnly(this);
            return;
        }

        /* Check that the file's read-only status is still correct (but
           only if the file can still be opened successfully in read mode) */
        if (fileMode_ != statbuf.st_mode || fileUid_ != statbuf.st_uid || fileGid_ != statbuf.st_gid) {

            fileMode_ = statbuf.st_mode;
            fileUid_  = statbuf.st_uid;
            fileGid_  = statbuf.st_gid;

            FILE *fp;
            if ((fp = fopen(fullname.toLatin1().data(), "r"))) {
                fclose(fp);

                bool readOnly = access(fullname.toLatin1().data(), W_OK) != 0;

                if (lockReasons_.isPermLocked() != readOnly) {
                    lockReasons_.setPermLocked(readOnly);
                    win->UpdateWindowTitle(this);
                    win->UpdateWindowReadOnly(this);
                }
            }
        }

        /* Warn the user if the file has been modified, unless checking is
           turned off or the user has already been warned.  Popping up a dialog
           from a focus callback (which is how this routine is usually called)
           seems to catch Motif off guard, and if the timing is just right, the
           dialog can be left with a still active pointer grab from a Motif menu
           which is still in the process of popping down.  The workaround, below,
           of calling XUngrabPointer is inelegant but seems to fix the problem. */
        if (!silent && ((lastModTime_ != 0 && lastModTime_ != statbuf.st_mtime) || fileMissing_)) {

            lastModTime_ = 0; // Inhibit further warnings
            fileMissing_ = false;
            if (!GetPrefWarnFileMods()) {
                return;
            }

            if (GetPrefWarnRealFileMods() && !cmpWinAgainstFile(fullname)) {
                // Contents hasn't changed. Update the modification time.
                lastModTime_ = statbuf.st_mtime;
                return;
            }

            QMessageBox messageBox(this);
            messageBox.setIcon(QMessageBox::Warning);
            messageBox.setWindowTitle(tr("File modified externally"));
            QPushButton *buttonReload = messageBox.addButton(tr("Reload"), QMessageBox::AcceptRole);
            QPushButton *buttonCancel = messageBox.addButton(QMessageBox::Cancel);

            Q_UNUSED(buttonCancel);

            if (fileChanged_) {
                messageBox.setText(tr("%1 has been modified by another program.  Reload?\n\nWARNING: Reloading will discard changes made in this editing session!").arg(filename_));
            } else {
                messageBox.setText(tr("%1 has been modified by another program.  Reload?").arg(filename_));
            }

            messageBox.exec();
            if(messageBox.clickedButton() == buttonReload) {
                RevertToSaved();
            }
        }
    }
}

QString DocumentWidget::FullPath() const {
    return tr("%1%2").arg(path_, filename_);
}

/*
 * Check if the contens of the TextBuffer *buf is equal
 * the contens of the file named fileName. The format of
 * the file (UNIX/DOS/MAC) is handled properly.
 *
 * Return values
 *   0: no difference found
 *  !0: difference found or could not compare contents.
 */
int DocumentWidget::cmpWinAgainstFile(const QString &fileName) {

    char fileString[PREFERRED_CMPBUF_LEN + 2];
    struct stat statbuf;
    int rv;
    int offset;
    char pendingCR         = 0;
    FileFormats fileFormat = fileFormat_;
    TextBuffer *buf        = buffer_;

    FILE *fp = fopen(fileName.toLatin1().data(), "r");
    if (!fp) {
        return 1;
    }

    if (fstat(fileno(fp), &statbuf) != 0) {
        fclose(fp);
        return 1;
    }

    int fileLen = statbuf.st_size;
    // For DOS files, we can't simply check the length
    if (fileFormat != DOS_FILE_FORMAT) {
        if (fileLen != buf->BufGetLength()) {
            fclose(fp);
            return 1;
        }
    } else {
        // If a DOS file is smaller on disk, it's certainly different
        if (fileLen < buf->BufGetLength()) {
            fclose(fp);
            return 1;
        }
    }

    /* For large files, the comparison can take a while. If it takes too long,
       the user should be given a clue about what is happening. */
    QString message = tr("Comparing externally modified 1s ...").arg(filename_);

    int restLen = std::min(PREFERRED_CMPBUF_LEN, fileLen);
    int bufPos  = 0;
    int filePos = 0;

    while (restLen > 0) {

        MainWindow::AllWindowsBusyEx(message);

        if (pendingCR) {
            fileString[0] = pendingCR;
            offset = 1;
        } else {
            offset = 0;
        }

        int nRead = fread(fileString + offset, sizeof(char), restLen, fp);
        if (nRead != restLen) {
            fclose(fp);
            MainWindow::AllWindowsUnbusyEx();
            return 1;
        }
        filePos += nRead;

        nRead += offset;

        // check for on-disk file format changes, but only for the first hunk
        if (bufPos == 0 && fileFormat != FormatOfFileEx(view::string_view(fileString, nRead))) {
            fclose(fp);
            MainWindow::AllWindowsUnbusyEx();
            return 1;
        }

        switch(fileFormat) {
        case MAC_FILE_FORMAT:
            ConvertFromMacFileString(fileString, nRead);
            break;
        case DOS_FILE_FORMAT:
            ConvertFromDosFileString(fileString, &nRead, &pendingCR);
            break;
        default:
            break;
        }

        // Beware of '\0' chars !
        buf->BufSubstituteNullChars(fileString, nRead);
        rv = buf->BufCmpEx(bufPos, nRead, fileString);
        if (rv) {
            fclose(fp);
            MainWindow::AllWindowsUnbusyEx();
            return rv;
        }
        bufPos += nRead;
        restLen = std::min(fileLen - filePos, PREFERRED_CMPBUF_LEN);
    }

    MainWindow::AllWindowsUnbusyEx();
    fclose(fp);
    if (pendingCR) {
        rv = buf->BufCmpEx(bufPos, 1, &pendingCR);
        if (rv) {
            return rv;
        }
        bufPos += 1;
    }

    if (bufPos != buf->BufGetLength()) {
        return 1;
    }
    return 0;
}

void DocumentWidget::RevertToSaved() {

    if(auto win = toWindow()) {
        int insertPositions[MAX_PANES];
        int topLines[MAX_PANES];
        int horizOffsets[MAX_PANES];
        int openFlags = 0;

        // Can't revert untitled windows
        if (!filenameSet_) {
            QMessageBox::warning(this, tr("Error"), tr("Window '%1' was never saved, can't re-read").arg(filename_));
            return;
        }

        const QList<TextArea *> textAreas = textPanes();
        const int panesCount = textAreas.size();

        // save insert & scroll positions of all of the panes to restore later
        for (int i = 0; i < panesCount; i++) {
            TextArea *area = textAreas[i];
            insertPositions[i] = area->TextGetCursorPos();
            area->TextDGetScroll(&topLines[i], &horizOffsets[i]);
        }

        // re-read the file, update the window title if new file is different
        QString name = filename_;
        QString path = path_;

        RemoveBackupFile();
        ClearUndoList();
        openFlags |= lockReasons_.isUserLocked() ? PREF_READ_ONLY : 0;

        if (!doOpen(name, path, openFlags)) {
            /* This is a bit sketchy.  The only error in doOpen that irreperably
               damages the window is "too much binary data".  It should be
               pretty rare to be reverting something that was fine only to find
               that now it has too much binary data. */
            if (!fileMissing_) {
                safeCloseEx();
            } else {
                // Treat it like an externally modified file
                lastModTime_ = 0;
                fileMissing_ = false;
            }
            return;
        }

        win->forceShowLineNumbers();
        win->UpdateWindowTitle(this);
        win->UpdateWindowReadOnly(this);

        // restore the insert and scroll positions of each pane
        for (int i = 0; i < panesCount; i++) {
            TextArea *area = textAreas[i];
            area->TextSetCursorPos(insertPositions[i]);
            area->TextDSetScroll(topLines[i], horizOffsets[i]);
        }
    }
}

/*
** Create a backup file for the current window.  The name for the backup file
** is generated using the name and path stored in the window and adding a
** tilde (~) on UNIX and underscore (_) on VMS to the beginning of the name.
*/
int DocumentWidget::WriteBackupFile() {
    FILE *fp;

    // Generate a name for the autoSave file
    QString name = backupFileNameEx();

    // remove the old backup file. Well, this might fail - we'll notice later however.
    ::remove(name.toLatin1().data());

    /* open the file, set more restrictive permissions (using default
       permissions was somewhat of a security hole, because permissions were
       independent of those of the original file being edited */
    int fd = ::open(name.toLatin1().data(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR);
    if (fd < 0 || (fp = fdopen(fd, "w")) == nullptr) {

        QMessageBox::warning(this, tr("Error writing Backup"), tr("Unable to save backup for %1:\n%2\nAutomatic backup is now off").arg(filename_).arg(QString::fromLatin1(strerror(errno))));
        autoSave_ = false;

        if(auto win = toWindow()) {
            no_signals(win->ui.action_Incremental_Backup)->setChecked(false);
        }
        return false;
    }

    // get the text buffer contents and its length
    std::string fileString = buffer_->BufGetAllEx();

    // If null characters are substituted for, put them back
    buffer_->BufUnsubstituteNullCharsEx(fileString);

    // add a terminating newline if the file doesn't already have one
    if (!fileString.empty() && fileString.back() != '\n') {
        fileString.append("\n"); // null terminator no longer needed
    }

    // write out the file
    ::fwrite(fileString.data(), sizeof(char), fileString.size(), fp);
    if (::ferror(fp)) {
        QMessageBox::critical(this, tr("Error saving Backup"), tr("Error while saving backup for %1:\n%2\nAutomatic backup is now off").arg(filename_).arg(QString::fromLatin1(strerror(errno))));
        ::fclose(fp);
        ::remove(name.toLatin1().data());
        autoSave_ = false;
        return false;
    }

    // close the backup file
    if (fclose(fp) != 0) {
        return false;
    }

    return true;
}

int DocumentWidget::SaveWindow() {

    // Try to ensure our information is up-to-date
    CheckForChangesToFileEx();

    /* Return success if the file is normal & unchanged or is a
        read-only file. */
    if ((!fileChanged_ && !fileMissing_ && lastModTime_ > 0) || lockReasons_.isAnyLockedIgnoringPerm()) {
        return true;
    }

    // Prompt for a filename if this is an Untitled window
    if (!filenameSet_) {
        return SaveWindowAs(QString(), false);
    }

    // Check for external modifications and warn the user
    if (GetPrefWarnFileMods() && fileWasModifiedExternally()) {

        QMessageBox messageBox(this);
        messageBox.setWindowTitle(tr("Save File"));
        messageBox.setIcon(QMessageBox::Warning);
        messageBox.setText(tr("%1 has been modified by another program.\n\n"
                              "Continuing this operation will overwrite any external\n"
                              "modifications to the file since it was opened in NEdit,\n"
                              "and your work or someone else's may potentially be lost.\n\n"
                              "To preserve the modified file, cancel this operation and\n"
                              "use Save As... to save this file under a different name,\n"
                              "or Revert to Saved to revert to the modified version.").arg(filename_));

        QPushButton *buttonContinue = messageBox.addButton(tr("Continue"), QMessageBox::AcceptRole);
        QPushButton *buttonCancel   = messageBox.addButton(QMessageBox::Cancel);
        Q_UNUSED(buttonContinue);

        messageBox.exec();
        if(messageBox.clickedButton() == buttonCancel) {
            // Cancel and mark file as externally modified
            lastModTime_ = 0;
            fileMissing_ = false;
            return false;
        }
    }

    if (writeBckVersion()) {
        return false;
    }

    bool stat = doSave();
    if (stat) {
        RemoveBackupFile();
    }

    return stat;
}

bool DocumentWidget::doSave() {
    struct stat statbuf;

    // Get the full name of the file

    QString fullname = FullPath();

    /*  Check for root and warn him if he wants to write to a file with
        none of the write bits set.  */
    if ((getuid() == 0) && (stat(fullname.toLatin1().data(), &statbuf) == 0) && !(statbuf.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) {

        int result = QMessageBox::warning(this, tr("Writing Read-only File"), tr("File '%1' is marked as read-only.\nDo you want to save anyway?").arg(filename_), QMessageBox::Save | QMessageBox::Cancel);
        if (result != QMessageBox::Save) {
            return true;
        }
    }

    /* add a terminating newline if the file doesn't already have one for
       Unix utilities which get confused otherwise
       NOTE: this must be done _before_ we create/open the file, because the
             (potential) buffer modification can trigger a check for file
             changes. If the file is created for the first time, it has
             zero size on disk, and the check would falsely conclude that the
             file has changed on disk, and would pop up a warning dialog */
    if (!buffer_->BufIsEmpty() && buffer_->BufGetCharacter(buffer_->BufGetLength() - 1) != '\n' && GetPrefAppendLF()) {
        buffer_->BufAppendEx("\n");
    }

    // open the file
    FILE *fp = ::fopen(fullname.toLatin1().data(), "wb");
    if(!fp) {

        QMessageBox messageBox(this);
        messageBox.setWindowTitle(tr("Error saving File"));
        messageBox.setIcon(QMessageBox::Warning);
        messageBox.setText(tr("Unable to save %1:\n%1\n\nSave as a new file?").arg(filename_).arg(QString::fromLatin1(strerror(errno))));

        QPushButton *buttonSaveAs = messageBox.addButton(tr("Save As..."), QMessageBox::AcceptRole);
        QPushButton *buttonCancel = messageBox.addButton(QMessageBox::Cancel);
        Q_UNUSED(buttonCancel);

        messageBox.exec();
        if(messageBox.clickedButton() == buttonSaveAs) {
            return SaveWindowAs(QString(), 0);
        }

        return false;
    }

    // get the text buffer contents and its length
    std::string fileString = buffer_->BufGetAllEx();

    // If null characters are substituted for, put them back
    buffer_->BufUnsubstituteNullCharsEx(fileString);

    // If the file is to be saved in DOS or Macintosh format, reconvert
    if (fileFormat_ == DOS_FILE_FORMAT) {
        if (!ConvertToDosFileStringEx(fileString)) {
            QMessageBox::critical(this, tr("Out of Memory"), tr("Out of memory!  Try\nsaving in Unix format"));

            // NOTE(eteran): fixes resource leak error
            fclose(fp);
            return false;
        }
    } else if (fileFormat_ == MAC_FILE_FORMAT) {
        ConvertToMacFileStringEx(fileString);
    }

    // write to the file
    fwrite(fileString.data(), sizeof(char), fileString.size(), fp);

    if (ferror(fp)) {
        QMessageBox::critical(this, tr("Error saving File"), tr("%2 not saved:\n%2").arg(filename_).arg(QString::fromLatin1(strerror(errno))));
        ::fclose(fp);
        ::remove(fullname.toLatin1().data());
        return false;
    }

    // close the file
    if (::fclose(fp) != 0) {
        QMessageBox::critical(this, tr("Error closing File"), tr("Error closing file:\n%1").arg(QString::fromLatin1(strerror(errno))));
        return false;
    }

    // success, file was written
    SetWindowModified(false);

    // update the modification time
    if (::stat(fullname.toLatin1().data(), &statbuf) == 0) {
        lastModTime_ = statbuf.st_mtime;
        fileMissing_ = false;
        device_      = statbuf.st_dev;
        inode_       = statbuf.st_ino;
    } else {
        // This needs to produce an error message -- the file can't be accessed!
        lastModTime_ = 0;
        fileMissing_ = true;
        device_      = 0;
        inode_       = 0;
    }

    return true;
}

int DocumentWidget::SaveWindowAs(const QString &newName, bool addWrap) {

    if(auto win = toWindow()) {

        int  retVal;
        QString fullname;
        char filename[MAXPATHLEN];
        char pathname[MAXPATHLEN];

        if(newName.isNull()) {
            QFileDialog dialog(this, tr("Save File As"));
            dialog.setFileMode(QFileDialog::AnyFile);
            dialog.setAcceptMode(QFileDialog::AcceptSave);
            dialog.setDirectory((!path_.isEmpty()) ? path_ : QString());
            dialog.setOptions(QFileDialog::DontUseNativeDialog);

            if(QGridLayout* const layout = qobject_cast<QGridLayout*>(dialog.layout())) {
                if(layout->rowCount() == 4 && layout->columnCount() == 3) {
                    auto boxLayout = new QBoxLayout(QBoxLayout::LeftToRight);

                    auto unixCheck = new QRadioButton(tr("&Unix"));
                    auto dosCheck  = new QRadioButton(tr("D&OS"));
                    auto macCheck  = new QRadioButton(tr("&Macintosh"));

                    switch(fileFormat_) {
                    case DOS_FILE_FORMAT:
                        dosCheck->setChecked(true);
                        break;
                    case MAC_FILE_FORMAT:
                        macCheck->setChecked(true);
                        break;
                    case UNIX_FILE_FORMAT:
                        unixCheck->setChecked(true);
                        break;
                    }

                    auto group = new QButtonGroup();
                    group->addButton(unixCheck);
                    group->addButton(dosCheck);
                    group->addButton(macCheck);

                    boxLayout->addWidget(unixCheck);
                    boxLayout->addWidget(dosCheck);
                    boxLayout->addWidget(macCheck);

                    int row = layout->rowCount();

                    layout->addWidget(new QLabel(tr("Format: ")), row, 0, 1, 1);
                    layout->addLayout(boxLayout, row, 1, 1, 1, Qt::AlignLeft);

                    ++row;

                    auto wrapCheck = new QCheckBox(tr("&Add line breaks where wrapped"));
                    if(addWrap) {
                        wrapCheck->setChecked(true);
                    }

                    // NOTE(eteran): this seems a bit redundant to other code...
                    connect(wrapCheck, &QCheckBox::toggled, [&wrapCheck, this](bool checked) {
                        if(checked) {
                            int ret = QMessageBox::information(this, tr("Add Wrap"),
                                tr("This operation adds permanent line breaks to\n"
                                "match the automatic wrapping done by the\n"
                                "Continuous Wrap mode Preferences Option.\n\n"
                                "*** This Option is Irreversable ***\n\n"
                                "Once newlines are inserted, continuous wrapping\n"
                                "will no longer work automatically on these lines"),
                                QMessageBox::Ok, QMessageBox::Cancel);

                            if(ret != QMessageBox::Ok) {
                                wrapCheck->setChecked(false);
                            }
                        }
                    });


                    if (wrapMode_ == CONTINUOUS_WRAP) {
                        layout->addWidget(wrapCheck, row, 1, 1, 1);
                    }

                    if(dialog.exec()) {
                        if(dosCheck->isChecked()) {
                            fileFormat_ = DOS_FILE_FORMAT;
                        } else if(macCheck->isChecked()) {
                            fileFormat_ = MAC_FILE_FORMAT;
                        } else if(unixCheck->isChecked()) {
                            fileFormat_ = UNIX_FILE_FORMAT;
                        }

                        addWrap = wrapCheck->isChecked();
                        fullname = dialog.selectedFiles()[0];
                    } else {
                        return false;
                    }

                }
            }
        } else {
            fullname = newName;
        }

        // Add newlines if requested
        if (addWrap) {
            addWrapNewlines();
        }

        if (ParseFilename(fullname.toLatin1().data(), filename, pathname) != 0) {
            return false;
        }

        // If the requested file is this file, just save it and return
        if (filename_ == QString::fromLatin1(filename) && path_ == QString::fromLatin1(pathname)) {
            if (writeBckVersion()) {
                return false;
            }

            return doSave();
        }

        /* If the file is open in another window, make user close it.  Note that
           it is possible for user to close the window by hand while the dialog
           is still up, because the dialog is not application modal, so after
           doing the dialog, check again whether the window still exists. */
        if (DocumentWidget *otherWindow = MainWindow::FindWindowWithFile(QString::fromLatin1(filename), QString::fromLatin1(pathname))) {

            QMessageBox messageBox(this);
            messageBox.setWindowTitle(tr("File open"));
            messageBox.setIcon(QMessageBox::Warning);
            messageBox.setText(tr("%1 is open in another NEdit window").arg(QString::fromLatin1(filename)));
            QPushButton *buttonCloseOther = messageBox.addButton(tr("Close Other Window"), QMessageBox::AcceptRole);
            QPushButton *buttonCancel     = messageBox.addButton(QMessageBox::Cancel);
            Q_UNUSED(buttonCloseOther);

            messageBox.exec();
            if(messageBox.clickedButton() == buttonCancel) {
                return false;
            }

            if (otherWindow == MainWindow::FindWindowWithFile(QString::fromLatin1(filename), QString::fromLatin1(pathname))) {
                if (!otherWindow->CloseFileAndWindow(PROMPT_SBC_DIALOG_RESPONSE)) {
                    return false;
                }
            }
        }

        // Change the name of the file and save it under the new name
        RemoveBackupFile();
        filename_ = QString::fromLatin1(filename);
        path_     = QString::fromLatin1(pathname);
        fileMode_ = 0;
        fileUid_  = 0;
        fileGid_  = 0;

        lockReasons_.clear();
        retVal = doSave();
        win->UpdateWindowReadOnly(this);
        RefreshTabState();

        // Add the name to the convenience menu of previously opened files
        win->AddToPrevOpenMenu(fullname);

        /*  If name has changed, language mode may have changed as well, unless
            it's an Untitled window for which the user already set a language
            mode; it's probably the right one.  */
        if (languageMode_ == PLAIN_LANGUAGE_MODE || filenameSet_) {
            DetermineLanguageMode(false);
        }
        filenameSet_ = true;

        // Update the stats line and window title with the new filename
        win->UpdateWindowTitle(this);
        UpdateStatsLine(nullptr);

        win->SortTabBar();
        return retVal;
    }
    return 0;

}

/*
** Change a window created in NEdit's continuous wrap mode to the more
** conventional Unix format of embedded newlines.  Indicate to the user
** by turning off Continuous Wrap mode.
*/
void DocumentWidget::addWrapNewlines() {

    int insertPositions[MAX_PANES];
    int topLines[MAX_PANES];
    int horizOffset;

    const QList<TextArea *> textAreas = textPanes();

    // save the insert and scroll positions of each pane
    for(int i = 0; i < textAreas.size(); ++i) {
        TextArea *area = textAreas[i];
        insertPositions[i] = area->TextGetCursorPos();
        area->TextDGetScroll(&topLines[i], &horizOffset);
    }

    // Modify the buffer to add wrapping
    TextArea *area = textAreas[0];
    std::string fileString = area->TextGetWrappedEx(0, buffer_->BufGetLength());

    buffer_->BufSetAllEx(fileString);

    // restore the insert and scroll positions of each pane
    for(int i = 0; i < textAreas.size(); ++i) {
        TextArea *area = textAreas[i];
        area->TextSetCursorPos(insertPositions[i]);
        area->TextDSetScroll(topLines[i], 0);
    }

    /* Show the user that something has happened by turning off
       Continuous Wrap mode */
    if(auto win = toWindow()) {
        no_signals(win->ui.action_Wrap_Continuous)->setChecked(false);
    }
}

/*
** If saveOldVersion is on, copies the existing version of the file to
** <filename>.bck in anticipation of a new version being saved.  Returns
** true if backup fails and user requests that the new file not be written.
*/
bool DocumentWidget::writeBckVersion() {

    char bckname[MAXPATHLEN];
    struct stat statbuf;

    static const size_t IO_BUFFER_SIZE = (1024 * 1024);

    // Do only if version backups are turned on
    if (!saveOldVersion_) {
        return false;
    }

    // Get the full name of the file
    QString fullname = FullPath();

    // Generate name for old version
    if (fullname.size() >= MAXPATHLEN) {
        return bckError(tr("file name too long"), filename_);
    }
    snprintf(bckname, sizeof(bckname), "%s.bck", fullname.toLatin1().data());

    // Delete the old backup file
    // Errors are ignored; we'll notice them later.
    ::remove(bckname);

    /* open the file being edited.  If there are problems with the
       old file, don't bother the user, just skip the backup */
    int in_fd = ::open(fullname.toLatin1().data(), O_RDONLY);
    if (in_fd < 0) {
        return false;
    }

    /* Get permissions of the file.
       We preserve the normal permissions but not ownership, extended
       attributes, et cetera. */
    if (::fstat(in_fd, &statbuf) != 0) {
        return false;
    }

    // open the destination file exclusive and with restrictive permissions.
    int out_fd = ::open(bckname, O_CREAT | O_EXCL | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
    if (out_fd < 0) {
        return bckError(tr("Error open backup file"), QString::fromLatin1(bckname));
    }

    // Set permissions on new file
    if (::fchmod(out_fd, statbuf.st_mode) != 0) {
        ::close(in_fd);
        ::close(out_fd);
        ::remove(bckname);
        return bckError(tr("fchmod() failed"), QString::fromLatin1(bckname));
    }

    // Allocate I/O buffer
    auto io_buffer = std::make_unique<char[]>(IO_BUFFER_SIZE);;

    // copy loop
    for (;;) {
        ssize_t bytes_read = ::read(in_fd, &io_buffer[0], IO_BUFFER_SIZE);

        if (bytes_read < 0) {
            ::close(in_fd);
            ::close(out_fd);
            ::remove(bckname);
            return bckError(tr("read() error"), filename_);
        }

        if (bytes_read == 0) {
            break; // EOF
        }

        // write to the file
        ssize_t bytes_written = ::write(out_fd, &io_buffer[0], (size_t)bytes_read);
        if (bytes_written != bytes_read) {
            ::close(in_fd);
            ::close(out_fd);
            ::remove(bckname);
            return bckError(QString::fromLatin1(strerror(errno)), QString::fromLatin1(bckname));
        }
    }

    // close the input and output files
    ::close(in_fd);
    ::close(out_fd);
    return false;
}

/*
** Error processing for writeBckVersion, gives the user option to cancel
** the subsequent save, or continue and optionally turn off versioning
*/
bool DocumentWidget::bckError(const QString &errString, const QString &file) {

    QMessageBox messageBox(this);
    messageBox.setWindowTitle(tr("Error writing Backup"));
    messageBox.setIcon(QMessageBox::Critical);
    messageBox.setText(tr("Couldn't write .bck (last version) file.\n%1: %2").arg(file).arg(errString));

    QPushButton *buttonCancelSave = messageBox.addButton(tr("Cancel Save"),      QMessageBox::RejectRole);
    QPushButton *buttonTurnOff    = messageBox.addButton(tr("Turn off Backups"), QMessageBox::AcceptRole);
    QPushButton *buttonContinue   = messageBox.addButton(tr("Continue"),         QMessageBox::AcceptRole);

    Q_UNUSED(buttonContinue);

    messageBox.exec();
    if(messageBox.clickedButton() == buttonCancelSave) {
        return true;
    }

    if(messageBox.clickedButton() == buttonTurnOff) {
        saveOldVersion_ = false;

        if(auto win = toWindow()) {
            no_signals(win->ui.action_Make_Backup_Copy)->setChecked(false);
        }
    }

    return false;
}

/*
** Return true if the file displayed in window has been modified externally
** to nedit.  This should return false if the file has been deleted or is
** unavailable.
*/
int DocumentWidget::fileWasModifiedExternally() {
    struct stat statbuf;

    if (!filenameSet_) {
        return false;
    }

    QString fullname = FullPath();

    if (::stat(fullname.toLatin1().data(), &statbuf) != 0) {
        return false;
    }

    if (lastModTime_ == statbuf.st_mtime) {
        return false;
    }

    if (GetPrefWarnRealFileMods() && !cmpWinAgainstFile(fullname)) {
        return false;
    }

    return true;
}

int DocumentWidget::CloseFileAndWindow(int preResponse) {
    int response;
    int stat;

    // Make sure that the window is not in iconified state
    if (fileChanged_) {
        RaiseDocumentWindow();
    }

    /* If the window is a normal & unmodified file or an empty new file,
       or if the user wants to ignore external modifications then
       just close it.  Otherwise ask for confirmation first. */
    if (!fileChanged_ &&
        // Normal File
        ((!fileMissing_ && lastModTime_ > 0) ||
         // New File
         (fileMissing_ && lastModTime_ == 0) ||
         // File deleted/modified externally, ignored by user.
         !GetPrefWarnFileMods())) {
        CloseWindow();
        // up-to-date windows don't have outstanding backup files to close
    } else {
        if (preResponse == PROMPT_SBC_DIALOG_RESPONSE) {

            int resp = QMessageBox::warning(this, tr("Save File"), tr("Save %1 before closing?").arg(filename_), QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);

            // TODO(eteran): factor out the need for this mapping...
            switch(resp) {
            case QMessageBox::Yes:
                response = 1;
                break;
            case QMessageBox::No:
                response = 2;
                break;
            case QMessageBox::Cancel:
            default:
                response = 3;
                break;
            }

        } else {
            response = preResponse;
        }

        switch(response) {
        case YES_SBC_DIALOG_RESPONSE:
            // Save
            stat = SaveWindow();
            if (stat) {
                CloseWindow();
            } else {
                return false;
            }
            break;

        case NO_SBC_DIALOG_RESPONSE:
            // Don't Save
            RemoveBackupFile();
            CloseWindow();
            break;
        default:
            return false;
        }
    }
    return true;
}

/*
** Close a document, or an editor window
*/
void DocumentWidget::CloseWindow() {

    MainWindow *window = toWindow();
    if(!window) {
        return;
    }

    // Free smart indent macro programs
    EndSmartIndentEx(this);

    /* Clean up macro references to the doomed window.  If a macro is
       executing, stop it.  If macro is calling this (closing its own
       window), leave the window alive until the macro completes */
    bool keepWindow = !MacroWindowCloseActionsEx(this);

    // Kill shell sub-process and free related memory
    AbortShellCommandEx();

    // Unload the default tips files for this language mode if necessary
    UnloadLanguageModeTipsFileEx();

    /* if this is the last window, or must be kept alive temporarily because
       it's running the macro calling us, don't close it, make it Untitled */
    const int windowCount   = MainWindow::allWindows().size();
    const int documentCount = DocumentWidget::allDocuments().size();

    if (keepWindow || (windowCount == 1 && documentCount == 1)) {
        filename_ = QLatin1String("");

        QString name = MainWindow::UniqueUntitledNameEx();
        lockReasons_.clear();

        fileMode_     = 0;
        fileUid_      = 0;
        fileGid_      = 0;
        filename_     = name;
        path_         = QLatin1String("");
        ignoreModify_ = true;

        buffer_->BufSetAllEx("");

        ignoreModify_ = false;
        nMarks_       = 0;
        filenameSet_  = false;
        fileMissing_  = true;
        fileChanged_  = false;
        fileFormat_   = UNIX_FILE_FORMAT;
        lastModTime_  = 0;
        device_       = 0;
        inode_        = 0;

        StopHighlightingEx();
        EndSmartIndentEx(this);
        window->UpdateWindowTitle(this);
        window->UpdateWindowReadOnly(this);
        window->ui.action_Close->setEnabled(false);
        window->ui.action_Read_Only->setEnabled(true);
        window->ui.action_Read_Only->setChecked(false);
        ClearUndoList();
        ClearRedoList();
        UpdateStatsLine(nullptr);
        DetermineLanguageMode(true);
        RefreshTabState();
        window->updateLineNumDisp();
        return;
    }

    // NOTE(eteran): there used to be some logic about syncronizing the multi-file
    //               replace dialog. It was complex and error prone. Simpler to
    //               just make the multi-file replace dialog modal and avoid the
    //               issue all together

    // TODO(eteran): just put this into the destructor and make things simple...

    // Free syntax highlighting patterns, if any. w/o redisplaying
    FreeHighlightingDataEx(this);

    /* remove the buffer modification callbacks so the buffer will be
       deallocated when the last text widget is destroyed */
    buffer_->BufRemoveModifyCB(modifiedCB, this);
    buffer_->BufRemoveModifyCB(SyntaxHighlightModifyCBEx, this);

    // free the undo and redo lists
    ClearUndoList();
    ClearRedoList();

    // remove the window from the global window list, update window menus
    window->InvalidateWindowMenus();
    MainWindow::CheckCloseDimEx(); // Close of window running a macro may have been disabled.

    // NOTE(eteran): No need to explicitly sync this with the tab context menu
    //               because they are set to be in sync when the context menu is shown

    // TODO(eteran): I'd like to move this to an event on the tab widget itself,
    // so it's more automatic
    window->ui.action_Detach_Tab->setEnabled(window->TabCount() > 1);
    window->ui.action_Move_Tab_To->setEnabled(MainWindow::allWindows().size() > 1);

    // deallocate the window data structure
    delete this;

    if(window->TabCount() == 0) {
        delete window;
    }
}

DocumentWidget *DocumentWidget::documentFrom(TextArea *area) {

    if(!area) {
        return nullptr;
    }

    QObject *p = area->parent();
    while(p) {
        if(auto document = qobject_cast<DocumentWidget *>(p)) {
            return document;
        }

        p = p->parent();
    }

    return nullptr;
}

void DocumentWidget::open(const char *fullpath) {

    char filename[MAXPATHLEN];
    char pathname[MAXPATHLEN];

    if (ParseFilename(fullpath, filename, pathname) != 0 || strlen(filename) + strlen(pathname) > MAXPATHLEN - 1) {
        fprintf(stderr, "nedit: invalid file name for open action: %s\n", fullpath);
        return;
    }

    EditExistingFileEx(
                this,
                QString::fromLatin1(filename),
                QString::fromLatin1(pathname),
                0,
                QString(),
                false,
                QString(),
                GetPrefOpenInTab(),
                false);

    if(auto win = toWindow()) {
        win->CheckCloseDimEx();
    }
}

bool DocumentWidget::doOpen(const QString &name, const QString &path, int flags) {

    MainWindow *window = toWindow();
    if(!window) {
        return false;
    }

    // initialize lock reasons
    lockReasons_.clear();

    // Update the window data structure
    filename_    = name;
    path_        = path;
    filenameSet_ = true;
    fileMissing_ = true;

    struct stat statbuf;
    FILE *fp = nullptr;

    // Get the full name of the file
    const QString fullname = FullPath();

    // Open the file
    /* The only advantage of this is if you use clearcase,
       which messes up the mtime of files opened with r+,
       even if they're never actually written.
       To avoid requiring special builds for clearcase users,
       this is now the default.
    */
    {
        if ((fp = ::fopen(fullname.toLatin1().data(), "r"))) {
            if (::access(fullname.toLatin1().data(), W_OK) != 0) {
                lockReasons_.setPermLocked(true);
            }

        } else if (flags & CREATE && errno == ENOENT) {
            // Give option to create (or to exit if this is the only window)
            if (!(flags & SUPPRESS_CREATE_WARN)) {


                QMessageBox msgbox(this);
                QAbstractButton  *exitButton;

                // ask user for next action if file not found

                QList<DocumentWidget *> documents = DocumentWidget::allDocuments();
                int resp;
                if (this == documents.front() && documents.size() == 1) {

                    msgbox.setIcon(QMessageBox::Warning);
                    msgbox.setWindowTitle(tr("New File"));
                    msgbox.setText(tr("Can't open %1:\n%2").arg(fullname, QString::fromLatin1(strerror(errno))));
                    msgbox.addButton(tr("New File"), QMessageBox::AcceptRole);
                    msgbox.addButton(QMessageBox::Cancel);
                    exitButton = msgbox.addButton(tr("Exit NEdit"), QMessageBox::RejectRole);
                    resp = msgbox.exec();

                } else {

                    msgbox.setIcon(QMessageBox::Warning);
                    msgbox.setWindowTitle(tr("New File"));
                    msgbox.setText(tr("Can't open %1:\n%2").arg(fullname, QString::fromLatin1(strerror(errno))));
                    msgbox.addButton(tr("New File"), QMessageBox::AcceptRole);
                    msgbox.addButton(QMessageBox::Cancel);
                    exitButton = nullptr;
                    resp = msgbox.exec();
                }

                if (resp == QMessageBox::Cancel) {
                    return false;
                } else if (msgbox.clickedButton() == exitButton) {
                    exit(EXIT_SUCCESS);
                }
            }

            // Test if new file can be created
            int fd = ::creat(fullname.toLatin1().data(), 0666);
            if (fd == -1) {
                QMessageBox::critical(this, tr("Error creating File"), tr("Can't create %1:\n%2").arg(fullname, QString::fromLatin1(strerror(errno))));
                return false;
            } else {
                ::close(fd);
                ::remove(fullname.toLatin1().data());
            }

            SetWindowModified(false);
            if ((flags & PREF_READ_ONLY) != 0) {
                lockReasons_.setUserLocked(true);
            }

            window->UpdateWindowReadOnly(this);
            return true;
        } else {
            // A true error
            QMessageBox::critical(this, tr("Error opening File"), tr("Could not open %1%2:\n%3").arg(path, name, QString::fromLatin1(strerror(errno))));
            return false;
        }
    }

    /* Get the length of the file, the protection mode, and the time of the
       last modification to the file */
    if (::fstat(fileno(fp), &statbuf) != 0) {
        ::fclose(fp);
        filenameSet_ = false; // Temp. prevent check for changes.
        QMessageBox::critical(this, tr("Error opening File"), tr("Error opening %1").arg(name));
        filenameSet_ = true;
        return false;
    }

    if (S_ISDIR(statbuf.st_mode)) {
        ::fclose(fp);
        filenameSet_ = false; // Temp. prevent check for changes.
        QMessageBox::critical(this, tr("Error opening File"), tr("Can't open directory %1").arg(name));
        filenameSet_ = true;
        return false;
    }

#ifdef S_ISBLK
    if (S_ISBLK(statbuf.st_mode)) {
        ::fclose(fp);
        filenameSet_ = false; // Temp. prevent check for changes.
        QMessageBox::critical(this, tr("Error opening File"), tr("Can't open block device %1").arg(name));
        filenameSet_ = true;
        return false;
    }
#endif

    const auto fileLen = static_cast<int>(statbuf.st_size);

    // Allocate space for the whole contents of the file (unfortunately)
    auto fileString = std::make_unique<char[]>(fileLen + 1); // +1 = space for null

    if(!fileString) {
        ::fclose(fp);
        filenameSet_ = false; // Temp. prevent check for changes.
        QMessageBox::critical(this, tr("Error while opening File"), tr("File is too large to edit"));
        filenameSet_ = true;
        return false;
    }

    // Read the file into fileString and terminate with a null
    int readLen = ::fread(&fileString[0], sizeof(char), fileLen, fp);
    if (ferror(fp)) {
        ::fclose(fp);
        filenameSet_ = false; // Temp. prevent check for changes.
        QMessageBox::critical(this, tr("Error while opening File"), tr("Error reading %1:\n%2").arg(name, QString::fromLatin1(strerror(errno))));
        filenameSet_ = true;
        return false;
    }
    fileString[readLen] = '\0';

    // Close the file
    if (::fclose(fp) != 0) {
        // unlikely error
        QMessageBox::warning(this, tr("Error while opening File"), tr("Unable to close file"));
        // we read it successfully, so continue
    }

    /* Any errors that happen after this point leave the window in a
       "broken" state, and thus RevertToSaved will abandon the window if
       window->fileMissing_ is false and doOpen fails. */
    fileMode_    = statbuf.st_mode;
    fileUid_     = statbuf.st_uid;
    fileGid_     = statbuf.st_gid;
    lastModTime_ = statbuf.st_mtime;
    device_      = statbuf.st_dev;
    inode_       = statbuf.st_ino;
    fileMissing_ = false;

    // Detect and convert DOS and Macintosh format files
    if (GetPrefForceOSConversion()) {
        fileFormat_ = FormatOfFileEx(view::string_view(&fileString[0], readLen));
        if (fileFormat_ == DOS_FILE_FORMAT) {
            ConvertFromDosFileString(&fileString[0], &readLen, nullptr);
        } else if (fileFormat_ == MAC_FILE_FORMAT) {
            ConvertFromMacFileString(&fileString[0], readLen);
        }
    }

    // Display the file contents in the text widget
    ignoreModify_ = true;
    buffer_->BufSetAllEx(view::string_view(&fileString[0], readLen));
    ignoreModify_ = false;

    /* Check that the length that the buffer thinks it has is the same
       as what we gave it.  If not, there were probably nuls in the file.
       Substitute them with another character.  If that is impossible, warn
       the user, make the file read-only, and force a substitution */
    if (buffer_->BufGetLength() != readLen) {
        if (!buffer_->BufSubstituteNullChars(&fileString[0], readLen)) {

            QMessageBox msgbox(this);
            msgbox.setIcon(QMessageBox::Critical);
            msgbox.setWindowTitle(tr("Error while opening File"));
            msgbox.setText(tr("Too much binary data in file.  You may view it, but not modify or re-save its contents."));
            msgbox.addButton(tr("View"), QMessageBox::AcceptRole);
            msgbox.addButton(QMessageBox::Cancel);
            int resp = msgbox.exec();

            if (resp == QMessageBox::Cancel) {
                return false;
            }

            lockReasons_.setTMBDLocked(true);
            for (char *c = &fileString[0]; c < &fileString[readLen]; c++) {
                if (*c == '\0') {
                    *c = (char)0xfe;
                }
            }
            buffer_->nullSubsChar_ = (char)0xfe;
        }
        ignoreModify_ = true;

        // NOTE(eteran): this looks correct, but hasn't been tested, so
        // I'm putting an assert here. I think this code may possibly be
        // technically unreachable due to proper NUL handling at a different
        // layer
        Q_ASSERT(strlen(&fileString[0]) == static_cast<size_t>(readLen));
        buffer_->BufSetAllEx(view::string_view(&fileString[0], readLen));
        ignoreModify_ = false;
    }

    // Set window title and file changed flag
    if ((flags & PREF_READ_ONLY) != 0) {
        lockReasons_.setUserLocked(true);
    }

    if (lockReasons_.isPermLocked()) {
        fileChanged_ = false;
        window->UpdateWindowTitle(this);
    } else {
        SetWindowModified(false);
        if (lockReasons_.isAnyLocked()) {
            window->UpdateWindowTitle(this);
        }
    }

    window->UpdateWindowReadOnly(this);
    return true;
}

/*
** refresh window state for this document
*/
void DocumentWidget::RefreshWindowStates() {

    if (!IsTopDocument()) {
        return;
    }

    if(auto win = toWindow()) {

        if (modeMessageDisplayed_) {
            ui.labelFileAndSize->setText(modeMessage_);
        } else {
            UpdateStatsLine(nullptr);
        }

        win->UpdateWindowReadOnly(this);
        win->UpdateWindowTitle(this);

        // show/hide statsline as needed
        if (modeMessageDisplayed_ && !ui.statusFrame->isVisible()) {
            // turn on statline to display mode message
            ShowStatsLine(true);
        } else if (showStats_ && !ui.statusFrame->isVisible()) {
            // turn on statsline since it is enabled
            ShowStatsLine(true);
        } else if (!showStats_ && !modeMessageDisplayed_ && ui.statusFrame->isVisible()) {
            // turn off statsline since there's nothing to show
            ShowStatsLine(false);
        }

        // signal if macro/shell is running
        if (shellCmdData_ || macroCmdData_) {
            setCursor(Qt::WaitCursor);
        } else {
            setCursor(Qt::ArrowCursor);
        }

        refreshMenuBar();
        win->updateLineNumDisp();
    }
}

/*
** Refresh the various settings/state of the shell window per the
** settings of the top document.
*/
void DocumentWidget::refreshMenuBar() {
    if(auto win = toWindow()) {
        RefreshMenuToggleStates();

        // Add/remove language specific menu items
        win->UpdateUserMenus(this);

        // refresh selection-sensitive menus
        DimSelectionDepUserMenuItems(win->wasSelected_);
    }
}

/*
** Refresh the menu entries per the settings of the
** top document.
*/
void DocumentWidget::RefreshMenuToggleStates() {

    if (!IsTopDocument()) {
        return;
    }

    if(auto win = toWindow()) {
        // File menu
        win->ui.action_Print_Selection->setEnabled(win->wasSelected_);

        // Edit menu
        win->ui.action_Undo->setEnabled(!undo_.empty());
        win->ui.action_Redo->setEnabled(!redo_.empty());
        win->ui.action_Cut->setEnabled(win->wasSelected_);
        win->ui.action_Copy->setEnabled(win->wasSelected_);
        win->ui.action_Delete->setEnabled(win->wasSelected_);

        // Preferences menu
        no_signals(win->ui.action_Statistics_Line)->setChecked(showStats_);
        no_signals(win->ui.action_Incremental_Search_Line)->setChecked(win->showISearchLine_);
        no_signals(win->ui.action_Show_Line_Numbers)->setChecked(win->showLineNumbers_);
        no_signals(win->ui.action_Highlight_Syntax)->setChecked(highlightSyntax_);
        no_signals(win->ui.action_Highlight_Syntax)->setEnabled(languageMode_ != PLAIN_LANGUAGE_MODE);
        no_signals(win->ui.action_Apply_Backlighting)->setChecked(backlightChars_);
        no_signals(win->ui.action_Make_Backup_Copy)->setChecked(saveOldVersion_);
        no_signals(win->ui.action_Incremental_Backup)->setChecked(autoSave_);
        no_signals(win->ui.action_Overtype)->setChecked(overstrike_);
        no_signals(win->ui.action_Matching_Syntax)->setChecked(matchSyntaxBased_);
        no_signals(win->ui.action_Read_Only)->setChecked(lockReasons_.isUserLocked());
        no_signals(win->ui.action_Indent_Smart)->setEnabled(SmartIndentMacrosAvailable(LanguageModeName(languageMode_)));

        SetAutoIndent(indentStyle_);
        SetAutoWrap(wrapMode_);
        SetShowMatching(showMatchingStyle_);
        SetLanguageMode(languageMode_, false);

        // Windows Menu
        no_signals(win->ui.action_Split_Pane)->setEnabled(textPanesCount() < MAX_PANES);
        no_signals(win->ui.action_Close_Pane)->setEnabled(textPanesCount() > 1);
        no_signals(win->ui.action_Detach_Tab)->setEnabled(win->ui.tabWidget->count() > 1);

        QList<MainWindow *> windows = MainWindow::allWindows();
        no_signals(win->ui.action_Move_Tab_To)->setEnabled(windows.size() > 1);
    }
}

/*
** Run the newline macro with information from the smart-indent callback
** structure passed by the widget
*/
void DocumentWidget::executeNewlineMacroEx(smartIndentCBStruct *cbInfo) {
    auto winData = static_cast<SmartIndentData *>(smartIndentData_);
    // posValue probably shouldn't be static due to re-entrance issues <slobasso>
    static DataValue posValue = {INT_TAG, {0}};
    DataValue result;
    RestartData *continuation;
    const char *errMsg;
    int stat;

    /* Beware of recursion: the newline macro may insert a string which
       triggers the newline macro to be called again and so on. Newline
       macros shouldn't insert strings, but nedit must not crash either if
       they do. */
    if (winData->inNewLineMacro) {
        return;
    }

    // Call newline macro with the position at which to add newline/indent
    posValue.val.n = cbInfo->pos;
    ++(winData->inNewLineMacro);
    stat = ExecuteMacroEx(this, winData->newlineMacro, 1, &posValue, &result, &continuation, &errMsg);

    // Don't allow preemption or time limit.  Must get return value
    while (stat == MACRO_TIME_LIMIT) {
        stat = ContinueMacroEx(continuation, &result, &errMsg);
    }

    --(winData->inNewLineMacro);
    /* Collect Garbage.  Note that the mod macro does not collect garbage,
       (because collecting per-line is more efficient than per-character)
       but GC now depends on the newline macro being mandatory */
    SafeGC();

    // Process errors in macro execution
    if (stat == MACRO_PREEMPT || stat == MACRO_ERROR) {
        QMessageBox::critical(this, tr("Smart Indent"), tr("Error in smart indent macro:\n%1").arg(stat == MACRO_ERROR ? QString::fromLatin1(errMsg) : tr("dialogs and shell commands not permitted")));
        EndSmartIndentEx(this);
        return;
    }

    // Validate and return the result
    if (result.tag != INT_TAG || result.val.n < -1 || result.val.n > 1000) {
        QMessageBox::critical(this, tr("Smart Indent"), tr("Smart indent macros must return\ninteger indent distance"));
        EndSmartIndentEx(this);
        return;
    }

    cbInfo->indentRequest = result.val.n;
}

/*
** Set showMatching state to one of NO_FLASH, FLASH_DELIMIT or FLASH_RANGE.
** Update the menu to reflect the change of state.
*/
void DocumentWidget::SetShowMatching(ShowMatchingStyle state) {
    showMatchingStyle_ = state;
    if (IsTopDocument()) {
        if(auto win = toWindow()) {
            switch(state) {
            case NO_FLASH:
                no_signals(win->ui.action_Matching_Off)->setChecked(true);
                break;
            case FLASH_DELIMIT:
                no_signals(win->ui.action_Matching_Delimiter)->setChecked(true);
                break;
            case FLASH_RANGE:
                no_signals(win->ui.action_Matching_Range)->setChecked(true);
                break;
            }
        }
    }
}

/*
** Run the modification macro with information from the smart-indent callback
** structure passed by the widget
*/
void DocumentWidget::executeModMacroEx(smartIndentCBStruct *cbInfo) {

    auto winData = static_cast<SmartIndentData *>(smartIndentData_);

    // args probably shouldn't be static due to future re-entrance issues <slobasso>
    static DataValue args[2] = {{INT_TAG, {0}}, {STRING_TAG, {0}}};

    // after 5.2 release remove inModCB and use new winData->inModMacro value
    static bool inModCB = false;

    DataValue result;
    RestartData *continuation;
    const char *errMsg;

    /* Check for inappropriate calls and prevent re-entering if the macro
       makes a buffer modification */
    if (winData == nullptr || winData->modMacro == nullptr || inModCB) {
        return;
    }

    /* Call modification macro with the position of the modification,
       and the character(s) inserted.  Don't allow
       preemption or time limit.  Execution must not overlap or re-enter */
    args[0].val.n = cbInfo->pos;
    AllocNStringCpy(&args[1].val.str, cbInfo->charsTyped);

    inModCB = true;
    ++(winData->inModMacro);

    int stat = ExecuteMacroEx(this, winData->modMacro, 2, args, &result, &continuation, &errMsg);

    while (stat == MACRO_TIME_LIMIT) {
        stat = ContinueMacroEx(continuation, &result, &errMsg);
    }

    --(winData->inModMacro);
    inModCB = false;

    // Process errors in macro execution
    if (stat == MACRO_PREEMPT || stat == MACRO_ERROR) {
        QMessageBox::critical(this, tr("Smart Indent"), tr("Error in smart indent modification macro:\n%1").arg(stat == MACRO_ERROR ? QString::fromLatin1(errMsg) : tr("dialogs and shell commands not permitted")));
        EndSmartIndentEx(this);
        return;
    }
}

void DocumentWidget::bannerTimeoutProc() {

    MainWindow *window = toWindow();
    if(!window) {
        return;
    }

    QString message;
    auto cmdData = static_cast<macroCmdInfoEx *>(macroCmdData_);

    cmdData->bannerIsUp = true;

    // Extract accelerator text from menu PushButtons
    QString cCancel = window->ui.action_Cancel_Learn->shortcut().toString();

    // Create message
    if (cCancel.isEmpty()) {
        message = tr("Macro Command in Progress");
    } else {
        message = tr("Macro Command in Progress -- Press %1 to Cancel").arg(cCancel);
    }

    SetModeMessageEx(message);
}

void DocumentWidget::actionClose(CloseMode mode) {

    int preResponse;

    switch(mode) {
    case CloseMode::Close_Prompt:
        preResponse = PROMPT_SBC_DIALOG_RESPONSE;
        break;
    case CloseMode::Close_Save:
        preResponse = YES_SBC_DIALOG_RESPONSE;
        break;
    case CloseMode::Close_NoSave:
        preResponse = NO_SBC_DIALOG_RESPONSE;
        break;
    }

    if(auto win = toWindow()) {
        CloseFileAndWindow(preResponse);
        win->CheckCloseDimEx();
    }
}

bool DocumentWidget::includeFile(const QString &name) {

    if (CheckReadOnly()) {
        return false;
    }

    // Open the file
    FILE *fp = ::fopen(name.toLatin1().data(), "rb");
    if(!fp) {
        QMessageBox::critical(this, tr("Error opening File"), tr("Could not open %1:\n%2").arg(name, QString::fromLatin1(strerror(errno))));
        return false;
    }

    struct stat statbuf;

    // Get the length of the file
    if (::fstat(fileno(fp), &statbuf) != 0) {
        QMessageBox::critical(this, tr("Error opening File"), tr("Error opening %1").arg(name));
        fclose(fp);
        return false;
    }

    if (S_ISDIR(statbuf.st_mode)) {
        QMessageBox::critical(this, tr("Error opening File"), tr("Can't open directory %1").arg(name));
        fclose(fp);
        return false;
    }

    int fileLen = statbuf.st_size;

    // allocate space for the whole contents of the file
    try {
        auto fileString = std::make_unique<char[]>(fileLen + 1); // +1 = space for null

        // read the file into fileString and terminate with a null
        int readLen = ::fread(&fileString[0], sizeof(char), fileLen, fp);
        if (::ferror(fp)) {
            QMessageBox::critical(this, tr("Error opening File"), tr("Error reading %1:\n%2").arg(name, QString::fromLatin1(strerror(errno))));
            ::fclose(fp);
            return false;
        }
        fileString[readLen] = '\0';

        // Detect and convert DOS and Macintosh format files
        switch (FormatOfFileEx(view::string_view(&fileString[0], readLen))) {
        case DOS_FILE_FORMAT:
            ConvertFromDosFileString(&fileString[0], &readLen, nullptr);
            break;
        case MAC_FILE_FORMAT:
            ConvertFromMacFileString(&fileString[0], readLen);
            break;
        case UNIX_FILE_FORMAT:
            //  Default is Unix, no conversion necessary.
            break;
        }

        // If the file contained ascii nulls, re-map them
        if (!buffer_->BufSubstituteNullChars(&fileString[0], readLen)) {
            QMessageBox::critical(this, tr("Error opening File"), tr("Too much binary data in file"));
        }

        // close the file
        if (::fclose(fp) != 0) {
            // unlikely error
            QMessageBox::warning(this, tr("Error opening File"), tr("Unable to close file"));
            // we read it successfully, so continue
        }

        /* insert the contents of the file in the selection or at the insert
           position in the window if no selection exists */
        if (buffer_->primary_.selected) {
            buffer_->BufReplaceSelectedEx(view::string_view(&fileString[0], readLen));
        } else {
            if(auto win = toWindow()) {
                auto textD = win->lastFocus_;
                buffer_->BufInsertEx(textD->TextGetCursorPos(), view::string_view(&fileString[0], readLen));
            }
        }

    } catch(const std::bad_alloc &) {
        QMessageBox::critical(this, tr("Error opening File"), tr("File is too large to include"));
        fclose(fp);
        return false;
    }

    return true;
}

void DocumentWidget::replaceAllAP(const QString &searchString, const QString &replaceString, SearchType searchType) {

    if (CheckReadOnly()) {
        return;
    }

    ReplaceAllEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                searchString,
                replaceString,
                searchType);
}

void DocumentWidget::replaceInSelAP(const QString &searchString, const QString &replaceString, SearchType searchType) {

    if (CheckReadOnly()) {
        return;
    }

    ReplaceInSelectionEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                searchString,
                replaceString,
                searchType);
}

void DocumentWidget::replaceFindAP(const QString &searchString, const QString &replaceString, SearchDirection direction, SearchType searchType, bool searchWraps) {

    if (CheckReadOnly()) {
        return;
    }

    ReplaceAndSearchEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                direction,
                searchString,
                replaceString,
                searchType,
                searchWraps);
}

void DocumentWidget::findAP(const QString &searchString, SearchDirection direction, SearchType searchType, bool searchWraps) {

    SearchAndSelectEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                direction,
                searchString,
                searchType,
                searchWraps);
}

void DocumentWidget::findIncrAP(const QString &searchString, SearchDirection direction, SearchType searchType, bool searchWraps, bool continued) {

    SearchAndSelectIncrementalEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                direction,
                searchString,
                searchType,
                searchWraps,
                continued);

}

void DocumentWidget::replaceAP(const QString &searchString, const QString &replaceString, SearchDirection direction, SearchType searchType, bool searchWraps) {

    if (CheckReadOnly()) {
        return;
    }

    SearchAndReplaceEx(
                toWindow(),
                this,
                toWindow()->lastFocus_,
                direction,
                searchString,
                replaceString,
                searchType,
                searchWraps);
}

void DocumentWidget::markAP(QChar ch) {

    if (!ch.isLetterOrNumber()) {
        qDebug("nedit: mark action requires a single-letter label");
        return;
    }

    AddMarkEx(
            toWindow(),
            this,
            toWindow()->lastFocus_,
            ch);
}

void DocumentWidget::gotoMarkAP(QChar label, bool extendSel) {

    if(MainWindow *win = toWindow()) {

        if(TextArea *area = win->lastFocus_) {

            int index;

            // look up the mark in the mark table
            label = label.toUpper();
            for (index = 0; index < nMarks_; index++) {
                if (markTable_[index].label == label.toLatin1())
                    break;
            }

            if (index == nMarks_) {
                QApplication::beep();
                return;
            }

            // reselect marked the selection, and move the cursor to the marked pos
            TextSelection *sel    = &markTable_[index].sel;
            TextSelection *oldSel = &buffer_->primary_;

            int cursorPos = markTable_[index].cursorPos;
            if (extendSel) {

                int oldStart = oldSel->selected ? oldSel->start : area->TextGetCursorPos();
                int oldEnd   = oldSel->selected ? oldSel->end   : area->TextGetCursorPos();
                int newStart = sel->selected    ? sel->start    : cursorPos;
                int newEnd   = sel->selected    ? sel->end      : cursorPos;

                buffer_->BufSelect(oldStart < newStart ? oldStart : newStart, oldEnd > newEnd ? oldEnd : newEnd);
            } else {
                if (sel->selected) {
                    if (sel->rectangular) {
                        buffer_->BufRectSelect(sel->start, sel->end, sel->rectStart, sel->rectEnd);
                    } else {
                        buffer_->BufSelect(sel->start, sel->end);
                    }
                } else {
                    buffer_->BufUnselect();
                }
            }

            /* Move the window into a pleasing position relative to the selection
               or cursor.   MakeSelectionVisible is not great with multi-line
               selections, and here we will sometimes give it one.  And to set the
               cursor position without first using the less pleasing capability
               of the widget itself for bringing the cursor in to view, you have to
               first turn it off, set the position, then turn it back on. */
            area->setAutoShowInsertPos(false);
            area->TextSetCursorPos(cursorPos);
            MakeSelectionVisible(area);
            area->setAutoShowInsertPos(true);
        }
    }
}

void DocumentWidget::GotoMatchingCharacter(TextArea *area) {
    int selStart;
    int selEnd;
    int matchPos;
    TextBuffer *buf = buffer_;

    /* get the character to match and its position from the selection, or
       the character before the insert point if nothing is selected.
       Give up if too many characters are selected */
    if (!buf->GetSimpleSelection(&selStart, &selEnd)) {

        selEnd = area->TextGetCursorPos();

        if (overstrike_) {
            selEnd += 1;
        }

        selStart = selEnd - 1;
        if (selStart < 0) {
            QApplication::beep();
            return;
        }
    }
    if ((selEnd - selStart) != 1) {
        QApplication::beep();
        return;
    }

    // Search for it in the buffer
    if (!findMatchingCharEx(buf->BufGetCharacter(selStart), GetHighlightInfoEx(this, selStart), selStart, 0, buf->BufGetLength(), &matchPos)) {
        QApplication::beep();
        return;
    }

    /* temporarily shut off autoShowInsertPos before setting the cursor
       position so MakeSelectionVisible gets a chance to place the cursor
       string at a pleasing position on the screen (otherwise, the cursor would
       be automatically scrolled on screen and MakeSelectionVisible would do
       nothing) */
    area->setAutoShowInsertPos(false);
    area->TextSetCursorPos(matchPos + 1);
    MakeSelectionVisible(area);
    area->setAutoShowInsertPos(true);
}

bool DocumentWidget::findMatchingCharEx(char toMatch, void *styleToMatch, int charPos, int startLimit, int endLimit, int *matchPos) {
    int nestDepth;
    int matchIndex;
    int beginPos;
    int pos;
    char c;
    void *style = nullptr;
    TextBuffer *buf = buffer_;
    bool matchSyntaxBased = matchSyntaxBased_;

    // If we don't match syntax based, fake a matching style.
    if (!matchSyntaxBased) {
        style = styleToMatch;
    }

    // Look up the matching character and match direction
    for (matchIndex = 0; matchIndex < N_MATCH_CHARS; matchIndex++) {
        if (MatchingChars[matchIndex].c == toMatch) {
            break;
        }
    }

    if (matchIndex == N_MATCH_CHARS) {
        return false;
    }

    char matchChar = MatchingChars[matchIndex].match;
    int direction  = MatchingChars[matchIndex].direction;

    // find it in the buffer
    beginPos = (direction == SEARCH_FORWARD) ? charPos + 1 : charPos - 1;
    nestDepth = 1;
    if (direction == SEARCH_FORWARD) {
        for (pos = beginPos; pos < endLimit; pos++) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased) {
                    style = GetHighlightInfoEx(this, pos);
                }

                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(this, pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    } else { // SEARCH_BACKWARD
        for (pos = beginPos; pos >= startLimit; pos--) {
            c = buf->BufGetCharacter(pos);
            if (c == matchChar) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(this, pos);
                if (style == styleToMatch) {
                    nestDepth--;
                    if (nestDepth == 0) {
                        *matchPos = pos;
                        return true;
                    }
                }
            } else if (c == toMatch) {
                if (matchSyntaxBased)
                    style = GetHighlightInfoEx(this, pos);
                if (style == styleToMatch)
                    nestDepth++;
            }
        }
    }
    return false;
}

void DocumentWidget::SelectToMatchingCharacter(TextArea *area) {
    int selStart;
    int selEnd;
    int startPos;
    int endPos;
    int matchPos;
    TextBuffer *buf = buffer_;

    /* get the character to match and its position from the selection, or
       the character before the insert point if nothing is selected.
       Give up if too many characters are selected */
    if (!buf->GetSimpleSelection(&selStart, &selEnd)) {

        selEnd = area->TextGetCursorPos();
        if (overstrike_)
            selEnd += 1;
        selStart = selEnd - 1;
        if (selStart < 0) {
            QApplication::beep();
            return;
        }
    }
    if ((selEnd - selStart) != 1) {
        QApplication::beep();
        return;
    }

    // Search for it in the buffer
    if (!findMatchingCharEx(buf->BufGetCharacter(selStart), GetHighlightInfoEx(this, selStart), selStart, 0, buf->BufGetLength(), &matchPos)) {
        QApplication::beep();
        return;
    }

    startPos = (matchPos > selStart) ? selStart : matchPos;
    endPos   = (matchPos > selStart) ? matchPos : selStart;

    /* temporarily shut off autoShowInsertPos before setting the cursor
       position so MakeSelectionVisible gets a chance to place the cursor
       string at a pleasing position on the screen (otherwise, the cursor would
       be automatically scrolled on screen and MakeSelectionVisible would do
       nothing) */
    area->setAutoShowInsertPos(false);
    // select the text between the matching characters
    buf->BufSelect(startPos, endPos + 1);
    MakeSelectionVisible(area);
    area->setAutoShowInsertPos(true);
}

/*
** See findDefHelper
*/
void DocumentWidget::FindDefCalltip(TextArea *area, const char *arg) {
    // Reset calltip parameters to reasonable defaults
    globAnchored  = false;
    globPos       = -1;
    globHAlign    = TIP_LEFT;
    globVAlign    = TIP_BELOW;
    globAlignMode = TIP_SLOPPY;

    findDefinitionHelper(area, arg, TIP);
}

/*
** Lookup the definition for the current primary selection the currently
** loaded tags file and bring up the file and line that the tags file
** indicates.
*/
void DocumentWidget::findDefinitionHelper(TextArea *area, const char *arg, Mode search_type) {
    if (arg) {
        findDef(area, arg, search_type);
    } else {
        searchMode = search_type;

        const QMimeData *mimeData = QApplication::clipboard()->mimeData(QClipboard::Selection);
        if(!mimeData->hasText()) {
            return;
        }

        QString string = mimeData->text();
        findDef(area, string.toLatin1().data(), search_type);
    }
}

/*
** This code path is followed if the request came from either
** FindDefinition or FindDefCalltip.  This should probably be refactored.
*/
int DocumentWidget::findDef(TextArea *area, const char *value, Mode search_type) {
    int status = 0;

    static char tagText[MAX_TAG_LEN + 1];
    const char *p;

    searchMode = search_type;
    int l = strlen(value);
    if (l <= MAX_TAG_LEN) {

        // should be of type text???
        for (p = value; *p && isascii(*p); p++) {
        }

        if (!*p) {

            int ml = ((l < MAX_TAG_LEN) ? (l) : (MAX_TAG_LEN));
            strncpy(tagText, value, ml);
            tagText[ml] = '\0';

            // See if we can find the tip/tag
            status = findAllMatchesEx(this, area, tagText);

            // If we didn't find a requested calltip, see if we can use a tag
            if (status == 0 && search_type == TIP && TagsFileList) {
                searchMode = TIP_FROM_TAG;
                status = findAllMatchesEx(this, area, tagText);
            }

            if (status == 0) {
                // Didn't find any matches
                if (searchMode == TIP_FROM_TAG || searchMode == TIP) {
                    tagsShowCalltipEx(area, tr("No match for \"%1\" in calltips or tags.").arg(QString::fromLatin1(tagName)));
                } else {
                    QMessageBox::warning(this, tr("Tags"), tr("\"%1\" not found in tags %2").arg(QString::fromLatin1(tagName), (TagsFileList && TagsFileList->next) ? tr("files") : tr("file")));
                }
            }

        } else {
            fprintf(stderr, "NEdit: Can't handle non 8-bit text\n");
            QApplication::beep();
        }

    } else {
        fprintf(stderr, "NEdit: Tag Length too long.\n");
        QApplication::beep();
    }

    return status;
}

void DocumentWidget::FindDefinition(TextArea *area, const char *arg) {
    findDefinitionHelper(area, arg, TAG);
}

void DocumentWidget::execAP(TextArea *area, const QString &command) {

    if (CheckReadOnly()) {
        return;
    }

    ExecShellCommandEx(area, command, false);
}

/*
** Execute shell command "command", depositing the result at the current
** insert position or in the current selection if the window has a
** selection.
*/
void DocumentWidget::ExecShellCommandEx(TextArea *area, const QString &command, bool fromMacro) {
    if(auto win = toWindow()) {
        int flags = 0;

        // Can't do two shell commands at once in the same window
        if (shellCmdData_) {
            QApplication::beep();
            return;
        }

        // get the selection or the insert position
        const int pos = area->TextGetCursorPos();

        int left;
        int right;
        if (buffer_->GetSimpleSelection(&left, &right)) {
            flags = ACCUMULATE | REPLACE_SELECTION;
        } else {
            left = right = pos;
        }

        /* Substitute the current file name for % and the current line number
           for # in the shell command */
        QString fullName = FullPath();

        int line;
        int column;
        area->TextDPosToLineAndCol(pos, &line, &column);

        QString substitutedCommand = command;
        substitutedCommand.replace(QLatin1Char('%'), fullName);
        substitutedCommand.replace(QLatin1Char('#'), tr("%1").arg(line));


        // NOTE(eteran): this used to be a nullptr check because the old code would
        // produce a null string pointer if it failed to allocate.
        if(substitutedCommand.isNull()) {
            QMessageBox::critical(this, tr("Shell Command"), tr("Shell command is too long due to\n"
                                                               "filename substitutions with '%%' or\n"
                                                               "line number substitutions with '#'"));
            return;
        }

        // issue the command
        issueCommandEx(
                    win,
                    area,
                    substitutedCommand,
                    QString(),
                    flags,
                    left,
                    right,
                    fromMacro);

    }

}

void DocumentWidget::PrintWindow(TextArea *area, bool selectedOnly) {

    TextBuffer *buf = buffer_;
    TextSelection *sel = &buf->primary_;
    std::string fileString;

    /* get the contents of the text buffer from the text area widget.  Add
       wrapping newlines if necessary to make it match the displayed text */
    if (selectedOnly) {
        if (!sel->selected) {
            QApplication::beep();
            return;
        }
        if (sel->rectangular) {
            fileString = buf->BufGetSelectionTextEx();
        } else {
            fileString = area->TextGetWrappedEx(sel->start, sel->end);
        }
    } else {
        fileString = area->TextGetWrappedEx(0, buf->BufGetLength());
    }

    // If null characters are substituted for, put them back
    buf->BufUnsubstituteNullCharsEx(fileString);

    // add a terminating newline if the file doesn't already have one
    if (!fileString.empty() && fileString.back() != '\n') {
        fileString.push_back('\n'); // null terminator no longer needed
    }

    // Print the string
    PrintStringEx(fileString, filename_);
}

/*
** Print a string (length is required).  parent is the dialog parent, for
** error dialogs, and jobName is the print title.
*/
void DocumentWidget::PrintStringEx(const std::string &string, const QString &jobName) {

    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);

    // L_tmpnam defined in stdio.h
    char tmpFileName[L_tmpnam];
    snprintf(tmpFileName, sizeof(tmpFileName), "%s/nedit-XXXXXX", qPrintable(tempDir));

    int fd = mkstemp(tmpFileName);
    if (fd < 0) {
        QMessageBox::warning(this, tr("Error while Printing"), tr("Unable to write file for printing:\n%1").arg(QString::fromLatin1(strerror(errno))));
        return;
    }

    FILE *const fp = fdopen(fd, "w");

    // open the temporary file
    if (fp == nullptr) {
        QMessageBox::warning(this, tr("Error while Printing"), tr("Unable to write file for printing:\n%1").arg(QString::fromLatin1(strerror(errno))));
        return;
    }

    // write to the file
    fwrite(string.data(), sizeof(char), string.size(), fp);
    if (ferror(fp)) {
        QMessageBox::critical(this, tr("Error while Printing"), tr("%1 not printed:\n%2").arg(jobName).arg(QString::fromLatin1(strerror(errno))));
        fclose(fp); // should call close(fd) in turn!
        remove(tmpFileName);
        return;
    }

    // close the temporary file
    if (fclose(fp) != 0) {
        QMessageBox::warning(this, tr("Error while Printing"), tr("Error closing temp. print file:\n%1").arg(QString::fromLatin1(strerror(errno))));
        remove(tmpFileName);
        return;
    }

    // Print the temporary file, then delete it and return success

    auto dialog = new DialogPrint(QString::fromLatin1(tmpFileName), jobName, this);
    dialog->exec();
    delete dialog;

    remove(tmpFileName);
}

void DocumentWidget::splitPane() {

    // TODO(eteran): copy common state from an existing text area!
    //               this includes the BGMenu, and the syntax highlighting
    //               and probably a few other things too!
    auto area = createTextArea(buffer_);
    splitter_->addWidget(area);
    area->setFocus();
}

/*
** Close the window pane that last had the keyboard focus.
*/
void DocumentWidget::closePane() {

    if(splitter_->count() > 1) {
        TextArea *lastFocus = toWindow()->lastFocus_;
        for(int i = 0; i < splitter_->count(); ++i) {
            if(auto area = qobject_cast<TextArea *>(splitter_->widget(i))) {
                if(area == lastFocus) {
                    delete area;
                    return;
                }
            }
        }

        // if we got here, that means that the last focus isn't even in this
        // document, so we'll just nominate the last one
        QList<TextArea *> panes = textPanes();
        delete panes.back();
    }
}

/*
** Turn on smart-indent (well almost).  Unfortunately, this doesn't do
** everything.  It requires that the smart indent callback (SmartIndentCB)
** is already attached to all of the text widgets in the window, and that the
** smartIndent resource must be turned on in the widget.  These are done
** separately, because they are required per-text widget, and therefore must
** be repeated whenever a new text widget is created within this window
** (a split-window command).
*/
void DocumentWidget::BeginSmartIndentEx(int warn) {

    SmartIndent *indentMacros;
    int stoppedAt;
    QString errMsg;
    static bool initialized = false;

    // Find the window's language mode.  If none is set, warn the user
    QString modeName = LanguageModeName(languageMode_);
    if(modeName.isNull()) {
        if (warn) {
            QMessageBox::warning(this, tr("Smart Indent"), tr("No language-specific mode has been set for this file.\n\nTo use smart indent in this window, please select a\nlanguage from the Preferences -> Language Modes menu."));
        }
        return;
    }

    // Look up the appropriate smart-indent macros for the language
    indentMacros = findIndentSpec(modeName);
    if(!indentMacros) {
        if (warn) {
            QMessageBox::warning(this, tr("Smart Indent"), tr("Smart indent is not available in languagemode\n%1.\n\nYou can create new smart indent macros in the\nPreferences -> Default Settings -> Smart Indent\ndialog, or choose a different language mode from:\nPreferences -> Language Mode.").arg(modeName));
        }
        return;
    }


    /* Make sure that the initial macro file is loaded before we execute
       any of the smart-indent macros. Smart-indent macros may reference
       routines defined in that file. */
    ReadMacroInitFileEx(this);

    /* Compile and run the common and language-specific initialization macros
       (Note that when these return, the immediate commands in the file have not
       necessarily been executed yet.  They are only SCHEDULED for execution) */
    if (!initialized) {
        if (!ReadMacroStringEx(this, CommonMacros, "smart indent common initialization macros")) {
            return;
        }

        initialized = true;
    }

    if (!indentMacros->initMacro.isNull()) {
        if (!ReadMacroStringEx(this, indentMacros->initMacro, "smart indent initialization macro")) {
            return;
        }
    }

    // Compile the newline and modify macros and attach them to the window
    auto winData = new SmartIndentData;
    winData->inNewLineMacro = false;
    winData->inModMacro     = false;
    winData->newlineMacro   = ParseMacroEx(indentMacros->newlineMacro, &errMsg, &stoppedAt);


    if (!winData->newlineMacro) {
        delete winData;
        ParseErrorEx(this, indentMacros->newlineMacro, stoppedAt, tr("newline macro"), errMsg);
        return;
    }

    if (indentMacros->modMacro.isNull()) {
        winData->modMacro = nullptr;
    } else {

        winData->modMacro = ParseMacroEx(indentMacros->modMacro, &errMsg, &stoppedAt);
        if (!winData->modMacro) {

            FreeProgram(winData->newlineMacro);
            delete winData;
            ParseErrorEx(this, indentMacros->modMacro, stoppedAt, tr("smart indent modify macro"), errMsg);
            return;
        }
    }

    smartIndentData_ = winData;
}

/*
** present dialog for selecting a target window to move this document
** into. Do nothing if there is only one shell window opened.
*/
void DocumentWidget::moveDocument(MainWindow *fromWindow) {
    auto dialog = new DialogMoveDocument(this);

    // all windows, except for the source window
    QList<MainWindow *> allWindows = MainWindow::allWindows();
    allWindows.removeAll(fromWindow);

    // stop here if there's no other window to move to
    if (allWindows.empty()) {
        delete dialog;
        return;
    }

    // load them into the dialog
    for(MainWindow *win : allWindows) {
        dialog->addItem(win);
    }

    // reset the dialog and display it
    dialog->resetSelection();
    dialog->setLabel(filename_);
    dialog->setMultipleDocuments(fromWindow->TabCount() > 1);
    int r = dialog->exec();

    if(r == QDialog::Accepted) {

        int selection = dialog->selectionIndex();

        // get the this to move document into
        MainWindow *targetWin = allWindows[selection];

        // move top document
        if (dialog->moveAllSelected()) {
            // move all documents
            for(DocumentWidget *document : fromWindow->openDocuments()) {
                targetWin->ui.tabWidget->addTab(document, document->filename_);
                targetWin->show();
            }
        } else {
            targetWin->ui.tabWidget->addTab(this, filename_);
            targetWin->show();
        }

        // if we just emptied the window, then delete it
        if(fromWindow->TabCount() == 0) {
            delete fromWindow;
        }
    }

    delete dialog;
}

/*
** Turn on and off the display of the statistics line
*/
void DocumentWidget::ShowStatsLine(bool state) {

    /* In continuous wrap mode, text widgets must be told to keep track of
       the top line number in absolute (non-wrapped) lines, because it can
       be a costly calculation, and is only needed for displaying line
       numbers, either in the stats line, or along the left margin */
    for(TextArea *area : textPanes()) {
        area->TextDMaintainAbsLineNum(state);
    }

    showStats_ = state;
    ui.statusFrame->setVisible(state);
}

void DocumentWidget::setWrapMargin(int margin) {
    for(TextArea *area : textPanes()) {
        area->setWrapMargin(margin);
    }
}


/*
** Set the fonts for "window" from a font name, and updates the display.
** Also updates window->fontList_ which is used for statistics line.
**
** Note that this leaks memory and server resources.  In previous NEdit
** versions, fontLists were carefully tracked and freed, but X and Motif
** have some kind of timing problem when widgets are distroyed, such that
** fonts may not be freed immediately after widget destruction with 100%
** safety.  Rather than kludge around this with timerProcs, I have chosen
** to create new fontLists only when the user explicitly changes the font
** (which shouldn't happen much in normal NEdit operation), and skip the
** futile effort of freeing them.
*/
void DocumentWidget::SetFonts(const QString &fontName, const QString &italicName, const QString &boldName, const QString &boldItalicName) {


    auto textD = firstPane();

    // Check which fonts have changed
    bool primaryChanged = fontName != fontName_;

    bool highlightChanged = false;
    if (italicName != italicFontName_) {
        highlightChanged = true;
    }

    if (boldName != boldFontName_) {
        highlightChanged = true;
    }

    if (boldItalicName != boldItalicFontName_) {
        highlightChanged = true;
    }

    if (!primaryChanged && !highlightChanged)
        return;

    /* Get information about the current this sizing, to be used to
       determine the correct this size after the font is changed */

    // TODO(eteran): do we want the WINDOW width/height? or the widget's?
    //               I suspect we want the widget
#if 0
    int oldWindowWidth  = textD->width();
    int oldWindowHeight = textD->height();
#endif
    int marginHeight     = textD->getMarginHeight();
#if 0
    int marginWidth      = textD->getMarginWidth();
    QFont oldFont = textD->getFont();
#endif
    int textHeight    = textD->height();

#if 0
    int oldTextWidth = textD->getRect().width + textD->getLineNumWidth();
#endif
    int oldTextHeight = 0;

    for(TextArea *area : textPanes()) {
        textHeight = area->height();
        oldTextHeight += textHeight - 2 * marginHeight;
    }

#if 0
    QFontMetricsF fm(oldFont);
    int borderWidth   = oldWindowWidth - oldTextWidth;
    int borderHeight  = oldWindowHeight - oldTextHeight;
    int oldFontWidth  = fm.maxWidth();
    int oldFontHeight = textD->fontAscent() + textD->fontDescent();
#endif
    if (primaryChanged) {
        fontName_ = fontName;

        fontStruct_.fromString(fontName_);
        fontStruct_.setStyleStrategy(QFont::ForceIntegerMetrics);
    }

    if (highlightChanged) {
        italicFontName_     = italicName;
        italicFontStruct_.fromString(italicName);

        boldFontName_       = boldName;
        boldFontStruct_.fromString(boldName);

        boldItalicFontName_ = boldItalicName;
        boldItalicFontStruct_.fromString(boldItalicName);

        italicFontStruct_.setStyleStrategy(QFont::ForceIntegerMetrics);
        boldFontStruct_.setStyleStrategy(QFont::ForceIntegerMetrics);
        boldItalicFontStruct_.setStyleStrategy(QFont::ForceIntegerMetrics);
    }

    // Change the primary font in all the widgets
    if (primaryChanged) {
        for(TextArea *area : textPanes()) {
            area->setFont(fontStruct_);
        }
    }

    /* Change the highlight fonts, even if they didn't change, because
       primary font is read through the style table for syntax highlighting */
    if (highlightData_) {
        UpdateHighlightStylesEx(this);
    }

    /* Change the this manager size hints.
       Note: this has to be done _before_ we set the new sizes. ICCCM2
       compliant this managers (such as fvwm2) would otherwise resize
       the this twice: once because of the new sizes requested, and once
       because of the new size increments, resulting in an overshoot. */
#if 0
    UpdateWMSizeHints();
#endif


#if 0 // TODO(eteran): do we need to worry about these things explicitly anymore ?
    /* Use the information from the old this to re-size the this to a
       size appropriate for the new font, but only do so if there's only
       _one_ document in the this, in order to avoid growing-this bug */
    if (TabCount() == 1) {
        fontWidth = GetDefaultFontStruct(fontList_)->max_bounds.width;
        fontHeight = textD->fontAscent() + textD->fontDescent();
        newWindowWidth = (oldTextWidth * fontWidth) / oldFontWidth + borderWidth;
        newWindowHeight = (oldTextHeight * fontHeight) / oldFontHeight + borderHeight;
        XtVaSetValues(shell_, XmNwidth, newWindowWidth, XmNheight, newWindowHeight, nullptr);
    }

    // Change the minimum pane height
    UpdateMinPaneHeights();
#endif
}

/*
** Set the backlight character class string
*/
void DocumentWidget::SetBacklightChars(const QString &applyBacklightTypes) {

    const bool do_apply = !applyBacklightTypes.isNull() ? true : false;

    backlightChars_ = do_apply;

    if(backlightChars_) {
        backlightCharTypes_ = applyBacklightTypes;
    } else {
        backlightCharTypes_ = QString();
    }

    for (TextArea *area : textPanes()) {
        area->setBacklightCharTypes(backlightCharTypes_);
    }
}

/*
** Set insert/overstrike mode
*/
void DocumentWidget::SetOverstrike(bool overstrike) {

    for (TextArea *area : textPanes()) {
        area->setOverstrike(overstrike);
    }

    overstrike_ = overstrike;
}

void DocumentWidget::gotoAP(TextArea *area, QStringList args) {

    int lineNum;
    int column;
    int position;
    int curCol;


    /* Accept various formats:
          [line]:[column]   (menu action)
          line              (macro call)
          line, column      (macro call) */
    if (    args.size() == 0 ||
            args.size() > 2 ||
            (args.size() == 1 && StringToLineAndCol(args[0].toLatin1().data(), &lineNum, &column) == -1) ||
            (args.size() == 2 && (!StringToNum(args[0], &lineNum) || !StringToNum(args[1], &column)))) {
        fprintf(stderr, "nedit: goto_line_number action requires line and/or column number\n");
        return;
    }

    // User specified column, but not line number
    if (lineNum == -1) {
        position = area->TextGetCursorPos();
        if (!area->TextDPosToLineAndCol(position, &lineNum, &curCol)) {
            return;
        }
    } else if (column == -1) {
        // User didn't specify a column
        SelectNumberedLineEx(this, area, lineNum);
        return;
    }

    position = area->TextDLineAndColToPos(lineNum, column);
    if (position == -1) {
        return;
    }

    area->TextSetCursorPos(position);
    return;
}

/*
**
*/
void DocumentWidget::SetColors(const QString &textFg, const QString &textBg, const QString &selectFg, const QString &selectBg, const QString &hiliteFg, const QString &hiliteBg, const QString &lineNoFg, const QString &cursorFg) {

    QColor textFgPix   = AllocColor(textFg);
    QColor textBgPix   = AllocColor(textBg);
    QColor selectFgPix = AllocColor(selectFg);
    QColor selectBgPix = AllocColor(selectBg);
    QColor hiliteFgPix = AllocColor(hiliteFg);
    QColor hiliteBgPix = AllocColor(hiliteBg);
    QColor lineNoFgPix = AllocColor(lineNoFg);
    QColor cursorFgPix = AllocColor(cursorFg);


    // Update all panes
    for(TextArea *area : textPanes()) {
        area->setForegroundPixel(textFgPix); // NOTE(eteran): seems redundant
        area->setBackgroundPixel(textBgPix); // NOTE(eteran): seems redundant
        area->TextDSetColors(textFgPix, textBgPix, selectFgPix, selectBgPix, hiliteFgPix, hiliteBgPix, lineNoFgPix, cursorFgPix);
    }

    // Redo any syntax highlighting
    if (highlightData_) {
        UpdateHighlightStylesEx(this);
    }
}

/*
** Display a special message in the stats line (show the stats line if it
** is not currently shown).
*/
void DocumentWidget::SetModeMessageEx(const QString &message) {
    /* this document may be hidden (not on top) or later made hidden,
       so we save a copy of the mode message, so we can restore the
       statsline when the document is raised to top again */
    modeMessageDisplayed_ = true;
    modeMessage_          = message;

    ui.labelFileAndSize->setText(message);

    /*
     * Don't invoke the stats line again, if stats line is already displayed.
     */
    if (!showStats_) {
        ShowStatsLine(true);
    }
}

/*
** Clear special statistics line message set in SetModeMessage, returns
** the statistics line to its original state as set in window->showStats_
*/
void DocumentWidget::ClearModeMessageEx() {

    if (!modeMessageDisplayed_) {
        return;
    }

    modeMessageDisplayed_ = false;
    modeMessage_          = QString();

    /*
     * Remove the stats line only if indicated by it's window state.
     */
    if (!showStats_) {
        ShowStatsLine(false);
    }

    UpdateStatsLine(nullptr);
}

void DocumentWidget::customContextMenuRequested(const QPoint &pos) {
    if(contextMenu_) {
        contextMenu_->exec(pos);
    }
}

/*
** Checks whether a window is still alive, and closes it only if so.
** Intended to be used when the file could not be opened for some reason.
** Normally the window is still alive, but the user may have closed the
** window instead of the error dialog. In that case, we shouldn't close the
** window a second time.
*/
void DocumentWidget::safeCloseEx() {

    QList<DocumentWidget *> documents = DocumentWidget::allDocuments();

    auto it = std::find_if(documents.begin(), documents.end(), [this](DocumentWidget *p) {
        return p == this;
    });

    if(it != documents.end()) {
        (*it)->CloseWindow();
    }
}

// Decref the default calltips file(s) for this window
void DocumentWidget::UnloadLanguageModeTipsFileEx() {

    int mode = languageMode_;
    if (mode != PLAIN_LANGUAGE_MODE && !LanguageModes[mode].defTipsFile.isNull()) {
        DeleteTagsFileEx(LanguageModes[mode].defTipsFile, TIP, false);
    }
}

/*
** Issue a shell command and feed it the string "input".  Output can be
** directed either to text widget "textW" where it replaces the text between
** the positions "replaceLeft" and "replaceRight", to a separate pop-up dialog
** (OUTPUT_TO_DIALOG), or to a macro-language string (OUTPUT_TO_STRING).  If
** "input" is nullptr, no input is fed to the process.  If an input string is
** provided, it is freed when the command completes.  Flags:
**
**   ACCUMULATE     	Causes output from the command to be saved up until
**  	    	    	the command completes.
**   ERROR_DIALOGS  	Presents stderr output separately in popup a dialog,
**  	    	    	and also reports failed exit status as a popup dialog
**  	    	    	including the command output.
**   REPLACE_SELECTION  Causes output to replace the selection in textW.
**   RELOAD_FILE_AFTER  Causes the file to be completely reloaded after the
**  	    	    	command completes.
**   OUTPUT_TO_DIALOG   Send output to a pop-up dialog instead of textW
**   OUTPUT_TO_STRING   Output to a macro-language string instead of a text
**  	    	    	widget or dialog.
**
** REPLACE_SELECTION, ERROR_DIALOGS, and OUTPUT_TO_STRING can only be used
** along with ACCUMULATE (these operations can't be done incrementally).
*/
void DocumentWidget::issueCommandEx(MainWindow *window, TextArea *area, const QString &command, const QString &input, int flags, int replaceLeft, int replaceRight, bool fromMacro) {

    // verify consistency of input parameters
    if ((flags & ERROR_DIALOGS || flags & REPLACE_SELECTION || flags & OUTPUT_TO_STRING) && !(flags & ACCUMULATE)) {
        return;
    }

    DocumentWidget *document = this;

    /* a shell command called from a macro must be executed in the same
       window as the macro, regardless of where the output is directed,
       so the user can cancel them as a unit */
    if (fromMacro) {
        document = MacroRunWindowEx();
    }

    // put up a watch cursor over the waiting window
    if (!fromMacro) {
        setCursor(Qt::WaitCursor);
    }

    // enable the cancel menu item
    if (!fromMacro) {
        window->ui.action_Cancel_Shell_Command->setEnabled(true);
    }

    // create the process and connect the output streams to the readyRead events
    auto process = new QProcess(this);

    connect(process, SIGNAL(finished(int, QProcess::ExitStatus)), document, SLOT(processFinished(int, QProcess::ExitStatus)));

    // support for merged output if we are not using ERROR_DIALOGS
    if (flags & ERROR_DIALOGS) {
        connect(process, SIGNAL(readyReadStandardError()),  document, SLOT(stderrReadProc()));
        connect(process, SIGNAL(readyReadStandardOutput()), document, SLOT(stdoutReadProc()));
    } else {
        process->setProcessChannelMode(QProcess::MergedChannels);
        connect(process, SIGNAL(readyRead()), document, SLOT(mergedReadProc()));
    }

    // start it off!
    QStringList args;
    args << QLatin1String("-c");
    args << command;
    process->start(GetPrefShell(), args);

    // if there's nothing to write to the process' stdin, close it now, otherwise
    // write it to the process
    if(!input.isEmpty()) {
        process->write(input.toLatin1());
    }

    process->closeWriteChannel();

    /* Create a data structure for passing process information around
       amongst the callback routines which will process i/o and completion */
    auto cmdData = new shellCmdInfoEx;
    document->shellCmdData_ = cmdData;

    cmdData->process     = process;
    cmdData->flags       = flags;
    cmdData->area        = area;
    cmdData->bannerIsUp  = false;
    cmdData->fromMacro   = fromMacro;
    cmdData->leftPos     = replaceLeft;
    cmdData->rightPos    = replaceRight;

    // Set up timer proc for putting up banner when process takes too long
    if (fromMacro) {
        cmdData->bannerTimer = nullptr;
    } else {
        cmdData->bannerTimer = new QTimer(document);
        QObject::connect(cmdData->bannerTimer, SIGNAL(timeout()), document, SLOT(bannerTimeoutProc()));
        cmdData->bannerTimer->setSingleShot(true);
        cmdData->bannerTimer->start(BANNER_WAIT_TIME);
    }

    /* If this was called from a macro, preempt the macro until shell
       command completes */
    if (fromMacro) {
        PreemptMacro();
    }
}

/*
** Called when the shell sub-process stream has data.
*/
void DocumentWidget::mergedReadProc() {
    if(auto cmdData = static_cast<shellCmdInfoEx *>(shellCmdData_)) {
        QByteArray data = cmdData->process->readAll();
        cmdData->standardOutput.append(data);
    }
}

/*
** Called when the shell sub-process stdout stream has data.
*/
void DocumentWidget::stdoutReadProc() {
    if(auto cmdData = static_cast<shellCmdInfoEx *>(shellCmdData_)) {
        QByteArray data = cmdData->process->readAllStandardOutput();
        cmdData->standardOutput.append(data);
    }
}

/*
** Called when the shell sub-process stderr stream has data.
*/
void DocumentWidget::stderrReadProc() {
    if(auto cmdData = static_cast<shellCmdInfoEx *>(shellCmdData_)) {
        QByteArray data = cmdData->process->readAllStandardOutput();
        cmdData->standardError.append(data);
    }
}

/*
** Clean up after the execution of a shell command sub-process and present
** the output/errors to the user as requested in the initial issueCommand
** call.  If "terminatedOnError" is true, don't bother trying to read the
** output, just close the i/o descriptors, free the memory, and restore the
** user interface state.
*/
void DocumentWidget::processFinished(int exitCode, QProcess::ExitStatus exitStatus) {

    MainWindow *window = toWindow();
    if(!window) {
        return;
    }

    auto cmdData = static_cast<shellCmdInfoEx *>(shellCmdData_);
    TextBuffer *buf;
    int reselectStart;
    bool cancel = false;
    bool fromMacro = cmdData->fromMacro;

    // Cancel pending timeouts
    if (cmdData->bannerTimer) {
        cmdData->bannerTimer->stop();
        delete cmdData->bannerTimer;
    }

    // Clean up waiting-for-shell-command-to-complete mode
    if (!cmdData->fromMacro) {
        setCursor(Qt::ArrowCursor);
        window->ui.action_Cancel_Shell_Command->setEnabled(false);
        if (cmdData->bannerIsUp) {
            ClearModeMessageEx();
        }
    }

    QString errText;
    QString outText;

    // If the process was killed or became inaccessable, give up
    if (exitStatus != QProcess::NormalExit) {
        goto cmdDone;
    }

    // if we have terminated the process, let's be 100% sure we've gotten all input
    cmdData->process->waitForReadyRead();

    /* Assemble the output from the process' stderr and stdout streams into
       strings */
    if (cmdData->flags & ERROR_DIALOGS) {
        // make sure we got the rest and convert it to a string
        QByteArray dataErr = cmdData->process->readAllStandardError();
        cmdData->standardError.append(dataErr);
        errText = QString::fromLocal8Bit(cmdData->standardError);

        QByteArray dataOut = cmdData->process->readAllStandardOutput();
        cmdData->standardOutput.append(dataOut);
        outText = QString::fromLocal8Bit(cmdData->standardOutput);
    } else {

        QByteArray data = cmdData->process->readAll();
        cmdData->standardOutput.append(data);
        outText = QString::fromLocal8Bit(cmdData->standardOutput);

    }

    /* Present error and stderr-information dialogs.  If a command returned
       error output, or if the process' exit status indicated failure,
       present the information to the user. */
    if (cmdData->flags & ERROR_DIALOGS) {
        bool failure = exitStatus != QProcess::NormalExit;
        bool errorReport = !errText.isEmpty();

        static const int DF_MAX_MSG_LENGTH = 4096;

        if (failure && errorReport) {
            errText.remove(QRegExp(QLatin1String("\n+$"))); // remove trailing newlines
            errText.truncate(DF_MAX_MSG_LENGTH);            // truncate to DF_MAX_MSG_LENGTH characters

            QMessageBox msgBox;
            msgBox.setWindowTitle(tr("Warning"));
            msgBox.setText(errText);
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Cancel);
            auto accept = new QPushButton(tr("Proceed"));
            msgBox.addButton(accept, QMessageBox::AcceptRole);
            msgBox.setDefaultButton(accept);
            cancel = (msgBox.exec() == QMessageBox::Cancel);

        } else if (failure) {
            outText.truncate(DF_MAX_MSG_LENGTH - 70); // truncate to ~DF_MAX_MSG_LENGTH characters

            QMessageBox msgBox;
            msgBox.setWindowTitle(tr("Command Failure"));
            msgBox.setText(tr("Command reported failed exit status.\nOutput from command:\n%1").arg(outText));
            msgBox.setIcon(QMessageBox::Warning);
            msgBox.setStandardButtons(QMessageBox::Cancel);
            auto accept = new QPushButton(tr("Proceed"));
            msgBox.addButton(accept, QMessageBox::AcceptRole);
            msgBox.setDefaultButton(accept);
            cancel = (msgBox.exec() == QMessageBox::Cancel);

        } else if (errorReport) {

            errText.remove(QRegExp(QLatin1String("\n+$"))); // remove trailing newlines
            errText.truncate(DF_MAX_MSG_LENGTH);            // truncate to DF_MAX_MSG_LENGTH characters

            QMessageBox msgBox;
            msgBox.setWindowTitle(tr("Information"));
            msgBox.setText(errText);
            msgBox.setIcon(QMessageBox::Information);
            msgBox.setStandardButtons(QMessageBox::Cancel);
            auto accept = new QPushButton(tr("Proceed"));
            msgBox.addButton(accept, QMessageBox::AcceptRole);
            msgBox.setDefaultButton(accept);
            cancel = (msgBox.exec() == QMessageBox::Cancel);
        }

        if (cancel) {
            goto cmdDone;
        }
    }

    {

        /* If output is to a dialog, present the dialog.  Otherwise insert the
           (remaining) output in the text widget as requested, and move the
           insert point to the end */
        if (cmdData->flags & OUTPUT_TO_DIALOG) {
            outText.remove(QRegExp(QLatin1String("\n+$"))); // remove trailing newlines

            if (!outText.isEmpty()) {
                auto dialog = new DialogOutput(this);
                dialog->setText(outText);
                dialog->show();
            }
        } else if (cmdData->flags & OUTPUT_TO_STRING) {
            std::string output_string = outText.toStdString();
            ReturnShellCommandOutputEx(this, output_string, exitCode);
        } else {

            std::string output_string = outText.toStdString();

            auto area = cmdData->area;
            buf = area->TextGetBuffer();
            if (!buf->BufSubstituteNullCharsEx(output_string)) {
                fprintf(stderr, "nedit: Too much binary data in shell cmd output\n");
                output_string.clear();
            }

            if (cmdData->flags & REPLACE_SELECTION) {
                reselectStart = buf->primary_.rectangular ? -1 : buf->primary_.start;
                buf->BufReplaceSelectedEx(output_string);

                area->TextSetCursorPos(buf->cursorPosHint_);
                if (reselectStart != -1) {
                    buf->BufSelect(reselectStart, reselectStart + output_string.size());
                }
            } else {
                safeBufReplace(buf, &cmdData->leftPos, &cmdData->rightPos, output_string);
                area->TextSetCursorPos(cmdData->leftPos + outText.size());
            }
        }

        // If the command requires the file to be reloaded afterward, reload it
        if (cmdData->flags & RELOAD_FILE_AFTER) {
            RevertToSaved();
        }
    }

cmdDone:
    delete cmdData->process;
    delete cmdData;
    shellCmdData_ = nullptr;
    if (fromMacro) {
        ResumeMacroExecutionEx(this);
    }
}

/*
** Cancel the shell command in progress
*/
void DocumentWidget::AbortShellCommandEx() {
    if(auto cmdData = static_cast<shellCmdInfoEx *>(shellCmdData_)) {
        if(QProcess *process = cmdData->process) {
            process->kill();
        }
    }
}

/*
** Execute the line of text where the the insertion cursor is positioned
** as a shell command.
*/
void DocumentWidget::ExecCursorLineEx(TextArea *area, bool fromMacro) {

    MainWindow *window = toWindow();
    if(!window) {
        return;
    }

    int left;
    int right;
    int insertPos;
    int line;
    int column;

    // Can't do two shell commands at once in the same window
    if (shellCmdData_) {
        QApplication::beep();
        return;
    }

    // get all of the text on the line with the insert position
    int pos = area->TextGetCursorPos();

    if (!buffer_->GetSimpleSelection(&left, &right)) {
        left = right = pos;
        left = buffer_->BufStartOfLine(left);
        right = buffer_->BufEndOfLine( right);
        insertPos = right;
    } else {
        insertPos = buffer_->BufEndOfLine( right);
    }

    std::string cmdText = buffer_->BufGetRangeEx(left, right);
    buffer_->BufUnsubstituteNullCharsEx(cmdText);

    // insert a newline after the entire line
    buffer_->BufInsertEx(insertPos, "\n");

    /* Substitute the current file name for % and the current line number
       for # in the shell command */
    QString fullName = tr("%1%2").arg(path_).arg(filename_);
    area->TextDPosToLineAndCol(pos, &line, &column);

    QString substitutedCommand = QString::fromStdString(cmdText);
    substitutedCommand.replace(QLatin1Char('%'), fullName);
    substitutedCommand.replace(QLatin1Char('#'), tr("%1").arg(line));

    if(substitutedCommand.isNull()) {
        QMessageBox::critical(this, tr("Shell Command"), tr("Shell command is too long due to\n"
                                                           "filename substitutions with '%%' or\n"
                                                           "line number substitutions with '#'"));
        return;
    }

    // issue the command
    issueCommandEx(
                window,
                area,
                substitutedCommand,
                QString(),
                0,
                insertPos + 1,
                insertPos + 1,
                fromMacro);

}

void DocumentWidget::filterSelection(const QString &filterText) {

    if (CheckReadOnly()) {
        return;
    }

    FilterSelection(filterText, false);
}

/*
** Filter the current selection through shell command "command".  The selection
** is removed, and replaced by the output from the command execution.  Failed
** command status and output to stderr are presented in dialog form.
*/
void DocumentWidget::FilterSelection(const QString &command, bool fromMacro) {

    MainWindow *window = toWindow();
    if(!window) {
        return;
    }

    // Can't do two shell commands at once in the same window
    if (shellCmdData_) {
        QApplication::beep();
        return;
    }

    /* Get the selection and the range in character positions that it
       occupies.  Beep and return if no selection */
    std::string text = buffer_->BufGetSelectionTextEx();
    if (text.empty()) {
        QApplication::beep();
        return;
    }
    buffer_->BufUnsubstituteNullCharsEx(text);
    int left  = buffer_->primary_.start;
    int right = buffer_->primary_.end;

    // Issue the command and collect its output
    issueCommandEx(
                window,
                window->lastFocus_,
                command,
                QString::fromStdString(text),
                ACCUMULATE | ERROR_DIALOGS | REPLACE_SELECTION,
                left,
                right,
                fromMacro);
}

/*
** Search through the shell menu and execute the first command with menu item
** name "itemName".  Returns True on successs and False on failure.
*/
bool DocumentWidget::DoNamedShellMenuCmd(TextArea *area, const QString &name, bool fromMacro) {

    MainWindow *window = toWindow();
    if(!window) {
        return false;
    }

    for(MenuData &data: ShellMenuData) {
        if (data.item->name == name) {
            if (data.item->output == TO_SAME_WINDOW && CheckReadOnly()) {
                return false;
            }

            DoShellMenuCmd(
                window,
                area,
                data.item->cmd,
                data.item->input,
                data.item->output,
                data.item->repInput,
                data.item->saveFirst,
                data.item->loadAfter,
                fromMacro);

            return true;
        }
    }
    return false;
}

/*
** Do a shell command, with the options allowed to users (input source,
** output destination, save first and load after) in the shell commands
** menu.
*/
void DocumentWidget::DoShellMenuCmd(MainWindow *inWindow, TextArea *area, const QString &command, InSrcs input, OutDests output, bool outputReplacesInput, bool saveFirst, bool loadAfter, bool fromMacro) {

    int flags = 0;
    int left  = 0;
    int right = 0;
    int line;
    int column;

    // Can't do two shell commands at once in the same window
    if (shellCmdData_) {
        QApplication::beep();
        return;
    }

    /* Substitute the current file name for % and the current line number
       for # in the shell command */
    QString fullName = tr("%1%2").arg(path_).arg(filename_);
    int pos = area->TextGetCursorPos();
    area->TextDPosToLineAndCol(pos, &line, &column);

    QString substitutedCommand = command;
    substitutedCommand.replace(QLatin1Char('%'), fullName);
    substitutedCommand.replace(QLatin1Char('#'), tr("%1").arg(line));

    if(substitutedCommand.isNull()) {
        QMessageBox::critical(this,
                              tr("Shell Command"),
                              tr("Shell command is too long due to filename substitutions with '%%' or line number substitutions with '#'"));
        return;
    }

    /* Get the command input as a text string.  If there is input, errors
      shouldn't be mixed in with output, so set flags to ERROR_DIALOGS */
    std::string text;
    switch(input) {
    case FROM_SELECTION:
        text = buffer_->BufGetSelectionTextEx();
        if (text.empty()) {
            QApplication::beep();
            return;
        }
        flags |= ACCUMULATE | ERROR_DIALOGS;
        break;
    case FROM_WINDOW:
        text = buffer_->BufGetAllEx();
        flags |= ACCUMULATE | ERROR_DIALOGS;
        break;
    case FROM_EITHER:
        text = buffer_->BufGetSelectionTextEx();
        if (text.empty()) {
            text = buffer_->BufGetAllEx();
        }
        flags |= ACCUMULATE | ERROR_DIALOGS;
        break;
    case FROM_NONE:
    default:
        text = std::string();
        break;
    }

    /* If the buffer was substituting another character for ascii-nuls,
       put the nuls back in before exporting the text */
    buffer_->BufUnsubstituteNullCharsEx(text);

    /* Assign the output destination.  If output is to a new window,
       create it, and run the command from it instead of the current
       one, to free the current one from waiting for lengthy execution */
    TextArea *outWidget = nullptr;
    switch(output) {
    case TO_DIALOG:
        outWidget = nullptr;
        flags |= OUTPUT_TO_DIALOG;
        left  = 0;
        right = 0;
        break;
    case TO_NEW_WINDOW:
        if(DocumentWidget *document = MainWindow::EditNewFileEx(
                    GetPrefOpenInTab() ? inWindow : nullptr,
                    QString(),
                    false,
                    QString(),
                    path_)) {

            inWindow  = document->toWindow();
            outWidget = document->firstPane();
            left      = 0;
            right     = 0;
            inWindow->CheckCloseDimEx();
        }
        break;
    case TO_SAME_WINDOW:
        outWidget = area;
        if (outputReplacesInput && input != FROM_NONE) {
            if (input == FROM_WINDOW) {
                left = 0;
                right = buffer_->BufGetLength();
            } else if (input == FROM_SELECTION) {
                buffer_->GetSimpleSelection(&left, &right);
                flags |= ACCUMULATE | REPLACE_SELECTION;
            } else if (input == FROM_EITHER) {
                if (buffer_->GetSimpleSelection(&left, &right)) {
                    flags |= ACCUMULATE | REPLACE_SELECTION;
                } else {
                    left = 0;
                    right = buffer_->BufGetLength();
                }
            }
        } else {
            if (buffer_->GetSimpleSelection(&left, &right)) {
                flags |= ACCUMULATE | REPLACE_SELECTION;
            } else {
                left = right = area->TextGetCursorPos();
            }
        }
        break;
    default:
        Q_ASSERT(0);
        break;
    }

    // If the command requires the file be saved first, save it
    if (saveFirst) {
        if (!SaveWindow()) {
            if (input != FROM_NONE)
            return;
        }
    }

    /* If the command requires the file to be reloaded after execution, set
       a flag for issueCommand to deal with it when execution is complete */
    if (loadAfter) {
        flags |= RELOAD_FILE_AFTER;
    }

    // issue the command
    issueCommandEx(
                inWindow,
                outWidget,
                substitutedCommand,
                QString::fromStdString(text),
                flags,
                left,
                right,
                fromMacro);
}

/*
** Search through the Macro or background menu and execute the first command
** with menu item name "itemName".  Returns True on successs and False on
** failure.
*/
bool DocumentWidget::DoNamedMacroMenuCmd(TextArea *area, const QString &name, bool fromMacro) {

    Q_UNUSED(fromMacro);
    Q_UNUSED(area);

    for(MenuData &data: MacroMenuData) {
        if (data.item->name == name) {

            DoMacroEx(
                this,
                data.item->cmd,
                "macro menu command");

            return true;
        }
    }
    return false;
}

bool DocumentWidget::DoNamedBGMenuCmd(TextArea *area, const QString &name, bool fromMacro) {
    Q_UNUSED(fromMacro);
    Q_UNUSED(area);

    for(MenuData &data: BGMenuData) {
        if (data.item->name == name) {
            DoMacroEx(
                this,
                data.item->cmd,
                "background menu macro");

            return true;
        }
    }
    return false;
}

int DocumentWidget::WidgetToPaneIndex(TextArea *area) const {
    return splitter_->indexOf(area);
}

/*
** Execute shell command "command", on input string "input", depositing the
** in a macro string (via a call back to ReturnShellCommandOutput).
*/
void DocumentWidget::ShellCmdToMacroStringEx(const std::string &command, const std::string &input) {

    // fork the command and begin processing input/output
    issueCommandEx(
                toWindow(),
                nullptr,
                QString::fromStdString(command),
                QString::fromStdString(input),
                ACCUMULATE | OUTPUT_TO_STRING,
                0,
                0,
                true);
}

/*
** Set the auto-scroll margin
*/
void DocumentWidget::SetAutoScroll(int margin) {

    for(TextArea *area : textPanes()) {
        area->setCursorVPadding(margin);
    }
}

void DocumentWidget::repeatMacro(const QString &macro, int how) {
    RepeatMacroEx(this, macro.toLatin1().data(), how);
}

QList<DocumentWidget *> DocumentWidget::allDocuments() {
    QList<MainWindow *> windows = MainWindow::allWindows();
    QList<DocumentWidget *> documents;

    for(MainWindow *window : windows) {
        documents.append(window->openDocuments());
    }
    return documents;

}