
#include "Settings.h"
#include "utils.h"

#include <QtDebug>
#include <QSettings>
#include <QStandardPaths>

// Some default colors
namespace {

auto NEDIT_DEFAULT_FG         = QLatin1String("#221f1e");
auto NEDIT_DEFAULT_TEXT_BG    = QLatin1String("#d6d2d0");
auto NEDIT_DEFAULT_SEL_FG     = QLatin1String("#ffffff");
auto NEDIT_DEFAULT_SEL_BG     = QLatin1String("#43ace8");
auto NEDIT_DEFAULT_HI_FG      = QLatin1String("white");        /* These are colors for flashing */
auto NEDIT_DEFAULT_HI_BG      = QLatin1String("red");          /* matching parens. */
auto NEDIT_DEFAULT_LINENO_FG  = QLatin1String("black");
auto NEDIT_DEFAULT_CURSOR_FG  = QLatin1String("black");
auto DEFAULT_DELIMETERS       = QLatin1String(".,/\\`'!|@#%^&*()-=+{}[]\":;<>?");
auto DEFAULT_BACKLIGHT_CHARS  = QLatin1String("0-8,10-31,127:red;9:#dedede;32,160-255:#f0f0f0;128-159:orange");
auto DEFAULT_TOOLTIP_COLOR    = QLatin1String("LemonChiffon1");
auto DEFAULT_TEXT_FONT        = QLatin1String("Courier New,10,-1,5,50,0,0,0,0,0");
auto DEFAULT_BOLD_FONT        = QLatin1String("Courier New,10,-1,5,75,0,0,0,0,0");
auto DEFAULT_ITALIC_FONT      = QLatin1String("Courier New,10,-1,5,50,1,0,0,0,0");
auto DEFAULT_BOLD_ITALIC_FONT = QLatin1String("Courier New,10,-1,5,75,1,0,0,0,0");

template <class T>
using IsEnum = typename std::enable_if<std::is_enum<T>::value>::type;

template <class T, class = IsEnum<T>>
T readEnum(QSettings &settings, const QString &key, const T &defaultValue = T()) {
    return from_integer<T>(settings.value(key, static_cast<int>(defaultValue)).toInt());
}

template <class T, class = IsEnum<T>>
void writeEnum(QSettings &settings, const QString &key, const T &value) {
    settings.setValue(key, static_cast<int>(value));
}

}

/**
 * @brief Settings::configFile
 * @return
 */
QString Settings::configFile() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    auto configFile   = tr("%1/%2/%3").arg(configDir, tr("nedit-ng"), tr("config.ini"));
    return configFile;
}

/**
 * @brief Settings::historyFile
 * @return
 */
QString Settings::historyFile() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    auto configFile   = tr("%1/%2/%3").arg(configDir, tr("nedit-ng"), tr("history"));
    return configFile;
}

/**
 * @brief Settings::autoLoadMacroFile
 * @return
 */
QString Settings::autoLoadMacroFile() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    auto configFile   = tr("%1/%2/%3").arg(configDir, tr("nedit-ng"), tr("autoload.nm"));
    return configFile;
}

/**
 * @brief Settings::styleFile
 * @return
 */
QString Settings::styleFile() {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    auto configFile   = tr("%1/%2/%3").arg(configDir, tr("nedit-ng"), tr("style.qss"));
    return configFile;
}

/**
 * @brief Settings::loadPreferences
 */
void Settings::loadPreferences() {

    QString filename = configFile();
    QSettings settings(filename, QSettings::IniFormat);

    fileVersion   = settings.value(tr("nedit.fileVersion"),   1).toInt();

#if defined(Q_OS_LINUX)
    shellCommands = settings.value(tr("nedit.shellCommands"), loadResource(QLatin1String("res/DefaultShellCommandsLinux.txt"))).toString();
#elif defined(Q_OS_FREEBSD)
    shellCommands = settings.value(tr("nedit.shellCommands"), loadResource(QLatin1String("res/DefaultShellCommandsFreeBSD.txt"))).toString();
#else
    shellCommands = settings.value(tr("nedit.shellCommands"), loadResource(QLatin1String("res/DefaultShellCommands.txt"))).toString();
#endif
    macroCommands         = settings.value(tr("nedit.macroCommands"),         loadResource(QLatin1String("res/DefaultMacroCommands.txt"))).toString();
    bgMenuCommands        = settings.value(tr("nedit.bgMenuCommands"),        loadResource(QLatin1String("res/DefaultBackgroundMenuCommands.txt"))).toString();
    highlightPatterns     = settings.value(tr("nedit.highlightPatterns"),     loadResource(QLatin1String("res/DefaultHighlightPatterns.txt"))).toString();
    languageModes         = settings.value(tr("nedit.languageModes"),         loadResource(QLatin1String("res/DefaultLanguageModes.txt"))).toString();
    styles                = settings.value(tr("nedit.styles"),                loadResource(QLatin1String("res/DefaultStyles.txt"))).toString();
    smartIndentInit       = settings.value(tr("nedit.smartIndentInit"),       loadResource(QLatin1String("res/DefaultSmartIndentInit.txt"))).toString();
    smartIndentInitCommon = settings.value(tr("nedit.smartIndentInitCommon"), loadResource(QLatin1String("res/DefaultSmartIndentInitCommon.txt"))).toString();

    autoWrap                          = readEnum(settings, tr("nedit.autoWrap"),                          WrapStyle::Continuous);
    autoIndent                        = readEnum(settings, tr("nedit.autoIndent"),                        IndentStyle::Auto);
    showMatching                      = readEnum(settings, tr("nedit.showMatching"),                      ShowMatchingStyle::Delimeter);
    searchMethod                      = readEnum(settings, tr("nedit.searchMethod"),                      SearchType::Literal);
    overrideDefaultVirtualKeyBindings = readEnum(settings, tr("nedit.overrideDefaultVirtualKeyBindings"), VirtKeyOverride::VIRT_KEY_OVERRIDE_AUTO);
    truncSubstitution                 = readEnum(settings, tr("nedit.truncSubstitution"),                 TruncSubstitution::Fail);

    wrapMargin            = settings.value(tr("nedit.wrapMargin"),            0).toInt();
    autoSave              = settings.value(tr("nedit.autoSave"),              true).toBool();
    openInTab             = settings.value(tr("nedit.openInTab"),             true).toBool();
    saveOldVersion        = settings.value(tr("nedit.saveOldVersion"),        false).toBool();
    matchSyntaxBased      = settings.value(tr("nedit.matchSyntaxBased"),      true).toBool();
    highlightSyntax       = settings.value(tr("nedit.highlightSyntax"),       true).toBool();
    backlightChars        = settings.value(tr("nedit.backlightChars"),        false).toBool();
    backlightCharTypes    = settings.value(tr("nedit.backlightCharTypes"),    DEFAULT_BACKLIGHT_CHARS).toString();
    searchDialogs         = settings.value(tr("nedit.searchDialogs"),         false).toBool();
    beepOnSearchWrap      = settings.value(tr("nedit.beepOnSearchWrap"),      false).toBool();
    retainSearchDialogs   = settings.value(tr("nedit.retainSearchDialogs"),   false).toBool();
    searchWraps           = settings.value(tr("nedit.searchWraps"),           true).toBool();
    stickyCaseSenseButton = settings.value(tr("nedit.stickyCaseSenseButton"), true).toBool();
    repositionDialogs     = settings.value(tr("nedit.repositionDialogs"),     false).toBool();
    autoScroll            = settings.value(tr("nedit.autoScroll"),            false).toBool();
    autoScrollVPadding    = settings.value(tr("nedit.autoScrollVPadding"),    4).toInt();
    appendLF              = settings.value(tr("nedit.appendLF"),              true).toBool();
    sortOpenPrevMenu      = settings.value(tr("nedit.sortOpenPrevMenu"),      true).toBool();
    statisticsLine        = settings.value(tr("nedit.statisticsLine"),        false).toBool();
    iSearchLine           = settings.value(tr("nedit.iSearchLine"),           false).toBool();
    sortTabs              = settings.value(tr("nedit.sortTabs"),              false).toBool();
    tabBar                = settings.value(tr("nedit.tabBar"),                true).toBool();
    tabBarHideOne         = settings.value(tr("nedit.tabBarHideOne"),         true).toBool();
    toolTips              = settings.value(tr("nedit.toolTips"),              true).toBool();
    globalTabNavigate     = settings.value(tr("nedit.globalTabNavigate"),     false).toBool();
    lineNumbers           = settings.value(tr("nedit.lineNumbers"),           false).toBool();
    pathInWindowsMenu     = settings.value(tr("nedit.pathInWindowsMenu"),     true).toBool();
    warnFileMods          = settings.value(tr("nedit.warnFileMods"),          true).toBool();
    warnRealFileMods      = settings.value(tr("nedit.warnRealFileMods"),      true).toBool();
    warnExit              = settings.value(tr("nedit.warnExit"),              true).toBool();

#if defined(REPLACE_SCOPE)
    replaceDefaultScope   = settings.value(tr("nedit.replaceDefaultScope"),   REPL_DEF_SCOPE_SMART).toInt();
#endif

    textRows                          = settings.value(tr("nedit.textRows"),					      24).toInt();
    textCols                          = settings.value(tr("nedit.textCols"),					      80).toInt();
    tabDistance                       = settings.value(tr("nedit.tabDistance"), 				      8).toInt();
    emulateTabs                       = settings.value(tr("nedit.emulateTabs"), 				      0).toInt();
    insertTabs                        = settings.value(tr("nedit.insertTabs"),  				      true).toBool();
    textFont                          = settings.value(tr("nedit.textFont"),					      DEFAULT_TEXT_FONT).toString();
    boldHighlightFont                 = settings.value(tr("nedit.boldHighlightFont"),			      DEFAULT_BOLD_FONT).toString();
    italicHighlightFont               = settings.value(tr("nedit.italicHighlightFont"), 		      DEFAULT_ITALIC_FONT).toString();
    boldItalicHighlightFont           = settings.value(tr("nedit.boldItalicHighlightFont"), 	      DEFAULT_BOLD_ITALIC_FONT).toString();

    colors[TEXT_FG_COLOR]             = settings.value(tr("nedit.textFgColor"), 				      NEDIT_DEFAULT_FG).toString();
    colors[TEXT_BG_COLOR]             = settings.value(tr("nedit.textBgColor"), 				      NEDIT_DEFAULT_TEXT_BG).toString();
    colors[SELECT_FG_COLOR]           = settings.value(tr("nedit.selectFgColor"),				      NEDIT_DEFAULT_SEL_FG).toString();
    colors[SELECT_BG_COLOR]           = settings.value(tr("nedit.selectBgColor"),				      NEDIT_DEFAULT_SEL_BG).toString();
    colors[HILITE_FG_COLOR]           = settings.value(tr("nedit.hiliteFgColor"),				      NEDIT_DEFAULT_HI_FG).toString();
    colors[HILITE_BG_COLOR]           = settings.value(tr("nedit.hiliteBgColor"),				      NEDIT_DEFAULT_HI_BG).toString();
    colors[LINENO_FG_COLOR]           = settings.value(tr("nedit.lineNoFgColor"),				      NEDIT_DEFAULT_LINENO_FG).toString();
    colors[CURSOR_FG_COLOR]           = settings.value(tr("nedit.cursorFgColor"),				      NEDIT_DEFAULT_CURSOR_FG).toString();

    tooltipBgColor                    = settings.value(tr("nedit.tooltipBgColor"),  			      DEFAULT_TOOLTIP_COLOR).toString();
    shell                             = settings.value(tr("nedit.shell"),						      QLatin1String("DEFAULT")).toString();
    geometry                          = settings.value(tr("nedit.geometry"),					      QLatin1String("")).toString();
    remapDeleteKey                    = settings.value(tr("nedit.remapDeleteKey"),  			      false).toBool();
    stdOpenDialog                     = settings.value(tr("nedit.stdOpenDialog"),				      false).toBool();
    tagFile                           = settings.value(tr("nedit.tagFile"), 					      QLatin1String("")).toString();
    wordDelimiters                    = settings.value(tr("nedit.wordDelimiters"),  			      DEFAULT_DELIMETERS).toString();
    serverName                        = settings.value(tr("nedit.serverName"),  				      QLatin1String("")).toString();
    maxPrevOpenFiles                  = settings.value(tr("nedit.maxPrevOpenFiles"),			      30).toInt();
    smartTags                         = settings.value(tr("nedit.smartTags"),					      true).toBool();
    typingHidesPointer                = settings.value(tr("nedit.typingHidesPointer"),  		      false).toBool();
    alwaysCheckRelativeTagsSpecs      = settings.value(tr("nedit.alwaysCheckRelativeTagsSpecs"),      true).toBool();
    prefFileRead                      = settings.value(tr("nedit.prefFileRead"),				      false).toBool();
    findReplaceUsesSelection          = settings.value(tr("nedit.findReplaceUsesSelection"),	      false).toBool();    
    titleFormat                       = settings.value(tr("nedit.titleFormat"),                       QLatin1String("{%c} [%s] %f (%S) - %d")).toString();
    undoModifiesSelection             = settings.value(tr("nedit.undoModifiesSelection"),             true).toBool();
    focusOnRaise                      = settings.value(tr("nedit.focusOnRaise"),                      false).toBool();
    forceOSConversion                 = settings.value(tr("nedit.forceOSConversion"),                 true).toBool();    
    honorSymlinks                     = settings.value(tr("nedit.honorSymlinks"),                     true).toBool();

    settingsLoaded_ = true;
}

/**
 * @brief Settings::importSettings
 * @param filename
 */
void Settings::importSettings(const QString &filename) {
    if(!settingsLoaded_) {
        qWarning("NEdit: Warning, importing while no previous settings loaded!");
    }

    QSettings settings(filename, QSettings::IniFormat);

    fileVersion           = settings.value(tr("nedit.fileVersion"),           fileVersion).toInt();
    shellCommands         = settings.value(tr("nedit.shellCommands"),         shellCommands).toString();
    macroCommands         = settings.value(tr("nedit.macroCommands"),         macroCommands).toString();
    bgMenuCommands        = settings.value(tr("nedit.bgMenuCommands"),        bgMenuCommands).toString();
    highlightPatterns     = settings.value(tr("nedit.highlightPatterns"),     highlightPatterns).toString();
    languageModes         = settings.value(tr("nedit.languageModes"),         languageModes).toString();
    styles                = settings.value(tr("nedit.styles"),                styles).toString();
    smartIndentInit       = settings.value(tr("nedit.smartIndentInit"),       smartIndentInit).toString();
    smartIndentInitCommon = settings.value(tr("nedit.smartIndentInitCommon"), smartIndentInitCommon).toString();

    autoWrap                          = readEnum(settings, tr("nedit.autoWrap"),                          autoWrap);
    autoIndent                        = readEnum(settings, tr("nedit.autoIndent"),                        autoIndent);
    showMatching                      = readEnum(settings, tr("nedit.showMatching"),                      showMatching);
    searchMethod                      = readEnum(settings, tr("nedit.searchMethod"),                      searchMethod);
    overrideDefaultVirtualKeyBindings = readEnum(settings, tr("nedit.overrideDefaultVirtualKeyBindings"), overrideDefaultVirtualKeyBindings);
    truncSubstitution                 = readEnum(settings, tr("nedit.truncSubstitution"),                 truncSubstitution);

    wrapMargin            = settings.value(tr("nedit.wrapMargin"),            wrapMargin).toInt();
    autoSave              = settings.value(tr("nedit.autoSave"),              autoSave).toBool();
    openInTab             = settings.value(tr("nedit.openInTab"),             openInTab).toBool();
    saveOldVersion        = settings.value(tr("nedit.saveOldVersion"),        saveOldVersion).toBool();
    matchSyntaxBased      = settings.value(tr("nedit.matchSyntaxBased"),      matchSyntaxBased).toBool();
    highlightSyntax       = settings.value(tr("nedit.highlightSyntax"),       highlightSyntax).toBool();
    backlightChars        = settings.value(tr("nedit.backlightChars"),        backlightChars).toBool();
    backlightCharTypes    = settings.value(tr("nedit.backlightCharTypes"),    backlightCharTypes).toString();
    searchDialogs         = settings.value(tr("nedit.searchDialogs"),         searchDialogs).toBool();
    beepOnSearchWrap      = settings.value(tr("nedit.beepOnSearchWrap"),      beepOnSearchWrap).toBool();
    retainSearchDialogs   = settings.value(tr("nedit.retainSearchDialogs"),   retainSearchDialogs).toBool();
    searchWraps           = settings.value(tr("nedit.searchWraps"),           searchWraps).toBool();
    stickyCaseSenseButton = settings.value(tr("nedit.stickyCaseSenseButton"), stickyCaseSenseButton).toBool();
    repositionDialogs     = settings.value(tr("nedit.repositionDialogs"),     repositionDialogs).toBool();
    autoScroll            = settings.value(tr("nedit.autoScroll"),            autoScroll).toBool();
    autoScrollVPadding    = settings.value(tr("nedit.autoScrollVPadding"),    autoScrollVPadding).toInt();
    appendLF              = settings.value(tr("nedit.appendLF"),              appendLF).toBool();
    sortOpenPrevMenu      = settings.value(tr("nedit.sortOpenPrevMenu"),      sortOpenPrevMenu).toBool();
    statisticsLine        = settings.value(tr("nedit.statisticsLine"),        statisticsLine).toBool();
    iSearchLine           = settings.value(tr("nedit.iSearchLine"),           iSearchLine).toBool();
    sortTabs              = settings.value(tr("nedit.sortTabs"),              sortTabs).toBool();
    tabBar                = settings.value(tr("nedit.tabBar"),                tabBar).toBool();
    tabBarHideOne         = settings.value(tr("nedit.tabBarHideOne"),         tabBarHideOne).toBool();
    toolTips              = settings.value(tr("nedit.toolTips"),              toolTips).toBool();
    globalTabNavigate     = settings.value(tr("nedit.globalTabNavigate"),     globalTabNavigate).toBool();
    lineNumbers           = settings.value(tr("nedit.lineNumbers"),           lineNumbers).toBool();
    pathInWindowsMenu     = settings.value(tr("nedit.pathInWindowsMenu"),     pathInWindowsMenu).toBool();
    warnFileMods          = settings.value(tr("nedit.warnFileMods"),          warnFileMods).toBool();
    warnRealFileMods      = settings.value(tr("nedit.warnRealFileMods"),      warnRealFileMods).toBool();
    warnExit              = settings.value(tr("nedit.warnExit"),              warnExit).toBool();

#if defined(REPLACE_SCOPE)
    replaceDefaultScope   = settings.value(tr("nedit.replaceDefaultScope"),   replaceDefaultScope).toInt();
#endif

    textRows                          = settings.value(tr("nedit.textRows"),					      textRows).toInt();
    textCols                          = settings.value(tr("nedit.textCols"),					      textCols).toInt();
    tabDistance                       = settings.value(tr("nedit.tabDistance"), 				      tabDistance).toInt();
    emulateTabs                       = settings.value(tr("nedit.emulateTabs"), 				      emulateTabs).toInt();
    insertTabs                        = settings.value(tr("nedit.insertTabs"),  				      insertTabs).toBool();
    textFont                          = settings.value(tr("nedit.textFont"),					      textFont).toString();
    boldHighlightFont                 = settings.value(tr("nedit.boldHighlightFont"),			      boldHighlightFont).toString();
    italicHighlightFont               = settings.value(tr("nedit.italicHighlightFont"), 		      italicHighlightFont).toString();
    boldItalicHighlightFont           = settings.value(tr("nedit.boldItalicHighlightFont"), 	      boldItalicHighlightFont).toString();

    colors[TEXT_FG_COLOR]             = settings.value(tr("nedit.textFgColor"), 				      colors[TEXT_FG_COLOR]).toString();
    colors[TEXT_BG_COLOR]             = settings.value(tr("nedit.textBgColor"), 				      colors[TEXT_BG_COLOR]).toString();
    colors[SELECT_FG_COLOR]           = settings.value(tr("nedit.selectFgColor"),				      colors[SELECT_FG_COLOR]).toString();
    colors[SELECT_BG_COLOR]           = settings.value(tr("nedit.selectBgColor"),				      colors[SELECT_BG_COLOR]).toString();
    colors[HILITE_FG_COLOR]           = settings.value(tr("nedit.hiliteFgColor"),				      colors[HILITE_FG_COLOR]).toString();
    colors[HILITE_BG_COLOR]           = settings.value(tr("nedit.hiliteBgColor"),				      colors[HILITE_BG_COLOR]).toString();
    colors[LINENO_FG_COLOR]           = settings.value(tr("nedit.lineNoFgColor"),				      colors[LINENO_FG_COLOR]).toString();
    colors[CURSOR_FG_COLOR]           = settings.value(tr("nedit.cursorFgColor"),				      colors[CURSOR_FG_COLOR]).toString();

    tooltipBgColor                    = settings.value(tr("nedit.tooltipBgColor"),  			      tooltipBgColor).toString();
    shell                             = settings.value(tr("nedit.shell"),						      shell).toString();
    geometry                          = settings.value(tr("nedit.geometry"),					      geometry).toString();
    remapDeleteKey                    = settings.value(tr("nedit.remapDeleteKey"),  			      remapDeleteKey).toBool();
    stdOpenDialog                     = settings.value(tr("nedit.stdOpenDialog"),				      stdOpenDialog).toBool();
    tagFile                           = settings.value(tr("nedit.tagFile"), 					      tagFile).toString();
    wordDelimiters                    = settings.value(tr("nedit.wordDelimiters"),  			      wordDelimiters).toString();
    serverName                        = settings.value(tr("nedit.serverName"),  				      serverName).toString();
    maxPrevOpenFiles                  = settings.value(tr("nedit.maxPrevOpenFiles"),			      maxPrevOpenFiles).toInt();
    smartTags                         = settings.value(tr("nedit.smartTags"),					      smartTags).toBool();
    typingHidesPointer                = settings.value(tr("nedit.typingHidesPointer"),  		      typingHidesPointer).toBool();
    alwaysCheckRelativeTagsSpecs      = settings.value(tr("nedit.alwaysCheckRelativeTagsSpecs"),      alwaysCheckRelativeTagsSpecs).toBool();
    prefFileRead                      = settings.value(tr("nedit.prefFileRead"),				      prefFileRead).toBool();
    findReplaceUsesSelection          = settings.value(tr("nedit.findReplaceUsesSelection"),	      findReplaceUsesSelection).toBool();    
    titleFormat                       = settings.value(tr("nedit.titleFormat"),                       titleFormat).toString();
    undoModifiesSelection             = settings.value(tr("nedit.undoModifiesSelection"),             undoModifiesSelection).toBool();
    focusOnRaise                      = settings.value(tr("nedit.focusOnRaise"),                      focusOnRaise).toBool();
    forceOSConversion                 = settings.value(tr("nedit.forceOSConversion"),                 forceOSConversion).toBool();    
    honorSymlinks                     = settings.value(tr("nedit.honorSymlinks"),                     honorSymlinks).toBool();
}

/**
 * @brief Settings::savePreferences
 * @return
 */
bool Settings::savePreferences() {
    QString filename = configFile();
    QSettings settings(filename, QSettings::IniFormat);

    settings.setValue(tr("nedit.fileVersion"), fileVersion);
    settings.setValue(tr("nedit.shellCommands"), shellCommands);
    settings.setValue(tr("nedit.macroCommands"), macroCommands);
    settings.setValue(tr("nedit.bgMenuCommands"), bgMenuCommands);
    settings.setValue(tr("nedit.highlightPatterns"), highlightPatterns);
    settings.setValue(tr("nedit.languageModes"), languageModes);
    settings.setValue(tr("nedit.styles"), styles);
    settings.setValue(tr("nedit.smartIndentInit"), smartIndentInit);
    settings.setValue(tr("nedit.smartIndentInitCommon"), smartIndentInitCommon);

    writeEnum(settings, tr("nedit.autoWrap"),                          autoWrap);
    writeEnum(settings, tr("nedit.autoIndent"),                        autoIndent);
    writeEnum(settings, tr("nedit.showMatching"),                      showMatching);
    writeEnum(settings, tr("nedit.searchMethod"),                      searchMethod);
    writeEnum(settings, tr("nedit.overrideDefaultVirtualKeyBindings"), overrideDefaultVirtualKeyBindings);
    writeEnum(settings, tr("nedit.truncSubstitution"),                 truncSubstitution);

    settings.setValue(tr("nedit.wrapMargin"), wrapMargin);
    settings.setValue(tr("nedit.autoSave"), autoSave);
    settings.setValue(tr("nedit.openInTab"), openInTab);
    settings.setValue(tr("nedit.saveOldVersion"), saveOldVersion);
    settings.setValue(tr("nedit.matchSyntaxBased"), matchSyntaxBased);
    settings.setValue(tr("nedit.highlightSyntax"), highlightSyntax);
    settings.setValue(tr("nedit.backlightChars"), backlightChars);
    settings.setValue(tr("nedit.backlightCharTypes"), backlightCharTypes);
    settings.setValue(tr("nedit.searchDialogs"), searchDialogs);
    settings.setValue(tr("nedit.beepOnSearchWrap"), beepOnSearchWrap);
    settings.setValue(tr("nedit.retainSearchDialogs"), retainSearchDialogs);
    settings.setValue(tr("nedit.searchWraps"), searchWraps);
    settings.setValue(tr("nedit.stickyCaseSenseButton"), stickyCaseSenseButton);
    settings.setValue(tr("nedit.repositionDialogs"), repositionDialogs);
    settings.setValue(tr("nedit.autoScroll"), autoScroll);
    settings.setValue(tr("nedit.autoScrollVPadding"), autoScrollVPadding);
    settings.setValue(tr("nedit.appendLF"), appendLF);
    settings.setValue(tr("nedit.sortOpenPrevMenu"), sortOpenPrevMenu);
    settings.setValue(tr("nedit.statisticsLine"), statisticsLine);
    settings.setValue(tr("nedit.iSearchLine"), iSearchLine);
    settings.setValue(tr("nedit.sortTabs"), sortTabs);
    settings.setValue(tr("nedit.tabBar"), tabBar);
    settings.setValue(tr("nedit.tabBarHideOne"), tabBarHideOne);
    settings.setValue(tr("nedit.toolTips"), toolTips);
    settings.setValue(tr("nedit.globalTabNavigate"), globalTabNavigate);
    settings.setValue(tr("nedit.lineNumbers"), lineNumbers);
    settings.setValue(tr("nedit.pathInWindowsMenu"), pathInWindowsMenu);
    settings.setValue(tr("nedit.warnFileMods"), warnFileMods);
    settings.setValue(tr("nedit.warnRealFileMods"), warnRealFileMods);
    settings.setValue(tr("nedit.warnExit"), warnExit);    
#if defined(REPLACE_SCOPE)
	settings.setValue(tr("nedit.replaceDefaultScope"),     replaceDefaultScope);
#endif
    settings.setValue(tr("nedit.textRows"), textRows);
    settings.setValue(tr("nedit.textCols"), textCols);
    settings.setValue(tr("nedit.tabDistance"), tabDistance);
    settings.setValue(tr("nedit.emulateTabs"), emulateTabs);
    settings.setValue(tr("nedit.insertTabs"), insertTabs);
    settings.setValue(tr("nedit.textFont"), textFont);
    settings.setValue(tr("nedit.boldHighlightFont"), boldHighlightFont);
    settings.setValue(tr("nedit.italicHighlightFont"), italicHighlightFont);
    settings.setValue(tr("nedit.boldItalicHighlightFont"), boldItalicHighlightFont);
    settings.setValue(tr("nedit.textFgColor"), colors[TEXT_FG_COLOR]);
    settings.setValue(tr("nedit.textBgColor"), colors[TEXT_BG_COLOR]);
    settings.setValue(tr("nedit.selectFgColor"), colors[SELECT_FG_COLOR]);
    settings.setValue(tr("nedit.selectBgColor"), colors[SELECT_BG_COLOR]);
    settings.setValue(tr("nedit.hiliteFgColor"), colors[HILITE_FG_COLOR]);
    settings.setValue(tr("nedit.hiliteBgColor"), colors[HILITE_BG_COLOR]);
    settings.setValue(tr("nedit.lineNoFgColor"), colors[LINENO_FG_COLOR]);
    settings.setValue(tr("nedit.cursorFgColor"), colors[CURSOR_FG_COLOR]);
    settings.setValue(tr("nedit.tooltipBgColor"), tooltipBgColor);
    settings.setValue(tr("nedit.shell"), shell);
    settings.setValue(tr("nedit.geometry"), geometry);
    settings.setValue(tr("nedit.remapDeleteKey"), remapDeleteKey);
    settings.setValue(tr("nedit.stdOpenDialog"), stdOpenDialog);
    settings.setValue(tr("nedit.tagFile"), tagFile);
    settings.setValue(tr("nedit.wordDelimiters"), wordDelimiters);
    settings.setValue(tr("nedit.serverName"), serverName);
    settings.setValue(tr("nedit.maxPrevOpenFiles"), maxPrevOpenFiles);
    settings.setValue(tr("nedit.smartTags"), smartTags);
    settings.setValue(tr("nedit.typingHidesPointer"), typingHidesPointer);
    settings.setValue(tr("nedit.alwaysCheckRelativeTagsSpecs"), alwaysCheckRelativeTagsSpecs);
    settings.setValue(tr("nedit.prefFileRead"), prefFileRead);
    settings.setValue(tr("nedit.findReplaceUsesSelection"), findReplaceUsesSelection);
    settings.setValue(tr("nedit.titleFormat"), titleFormat);
    settings.setValue(tr("nedit.undoModifiesSelection"), undoModifiesSelection);
    settings.setValue(tr("nedit.focusOnRaise"), focusOnRaise);
    settings.setValue(tr("nedit.forceOSConversion"), forceOSConversion);    
    settings.setValue(tr("nedit.honorSymlinks"), honorSymlinks);

    settings.sync();
    return settings.status() == QSettings::NoError;
}