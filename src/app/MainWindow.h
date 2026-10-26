#pragma once

#include "app/Updates.h"
#include "core/PageText.h"
#include "ui/ClassicWindow.h"
#include "core/Document.h"
#include "view/PageView.h"

#include <QColor>
#include <QMap>

#include <QHash>
#include <QTemporaryDir>

#include <atomic>
#include <memory>
#include <vector>

class ChooserButton;
class DocTabs;
class OrganizeView;
class HomePage;
struct Annot;
class QHBoxLayout;
class QTreeWidget;
class TrackBar;
class EngravedLabel;
class PageView;
class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QMenuBar;
class QSplitter;
class QStackedWidget;
class QTimer;
class SearchResults;
class StartPanel;
class StatusPanel;
class ThumbnailBar;
class ToolButton;

// The main window, laid out like the Python app's (and Acrobat's): the menu bar; the
// toolbar (open, save, undo, the page you're on, zoom - and Find at the right end); the
// tools; the bar under them saying what the tool does; the page thumbnails at the left;
// the pages; the status bar.
class MainWindow : public ClassicWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    bool openFile(const QString &path);  // (in a tab of its own - or its tab, if it's open already)
    void showDocument(std::shared_ptr<Document> doc, const QString &name);  // (a PDF opened or made: a new tab)
    // the tabs: Home first, then the PDFs open, one each
    DocTabs *tabs() const { return tabs_; }
    HomePage *home() const { return home_; }
    int tabCount() const { return int(docs_.size()); }
    void showTab(int doc);    // -1: Home
    // Read mode (View, the status bar's Read, Ctrl+H; Esc leaves it): the pages alone - no toolbars, no side
    // panel, no tabs - a little bar over them to turn the pages, zoom, and leave
    void setReadMode(bool on);
    bool readMode() const { return readMode_; }
    // Organize Pages (the toolbar's, by Find): every page in a grid, in place of the pages - picked, dragged,
    // turned, deleted, taken out, put in, replaced
    void showOrganize(bool on);
    bool organizing() const;
    OrganizeView *organizer() const { return organize_; }
    void closeTab(int doc);   // (asks first, if it has unsaved changes)
    // the File tools (step 7)
    void createFromFiles(QStringList paths);  // (none: asks which)
    void scanFile();  // (Home's Scan: a PDF or a picture from the computer, opened and scanned straight away)
    void exportDialog();
    void splitDialog();
    void compressDialog();
    static bool isWord(const QString &path);
    QString wordToPdf(const QString &src, const QString &folder, QString *error);
    QStringList fromWord(const QStringList &paths, QStringList *problems);

    // (for the self-test)
    PageView *view() const { return view_; }
    ThumbnailBar *thumbnails() const { return thumbs_; }
    QLineEdit *findBox() const { return findBox_; }
    QLineEdit *pageBox() const { return pageBox_; }
    QComboBox *zoomBox() const { return zoomBox_; }
    QMenuBar *menus() const { return menuBar_; }
    int findCount() const { return int(hits_.size()); }
    int findPos() const { return findPos_; }
    void search(const QString &text, bool jump = true);  // now (not a moment after typing)
    void pickTheme(const QString &choice, bool save = true);
    bool save();
    bool saveAs();
    bool saveTo(const QString &path);  // (Save As, the place chosen)
    void undo();
    void redo();
    void rotatePages(const std::vector<int> &pages, int degrees);
    void headerFooterDialog();  // (Pages > Header & Footer: found, changed, put on, taken off)
    void deletePages(const std::vector<int> &pages);  // (asks first)
    void movePages(const std::vector<int> &pages, int to);
    void insertBlank(int at);      // -1: after the page you're on
    void insertFromFile(int at);   // (asks which files)
    void insertPaths(const QStringList &paths, int at);
    void extractPages(const std::vector<int> &pages);  // (asks where)

    // the tools (step 4: annotations)
    void setTool(const QString &tool, bool toggle = true);
    QString currentTool() const { return tool_; }
    void setVariant(const QString &tool, const QString &kind) { variant_[tool] = kind; tintTools(); }
    void markText(const QString &kind);  // the selected text: Highlight, StrikeOut, Underline, Squiggly
    void deletePicked();
    void showAnnotations();
    void addPicture(int page, QPointF at, const QRectF &box);  // (asks which, unless it's the one copied)
    void deletePicture();
    void convertDrawing(int page, const Drawing &d);  // (step 7: the text reader)
    void addLayoutItems(class QMenu *menu);  // (the picture picked up's Layout Options)
    void retypeBlock(int page, int index, const QString &text, const Document::LineLook &look);
    void lineMovedBy(int page, int index, QPointF by);  // (the Edit tool's line - or block - dragged)
    // (things picked together - blocks of text by a line of each, pictures - dragged: all moved, one Undo step)
    void groupMovedBy(int page, const std::vector<int> &lines, const std::vector<int> &pictures, QPointF by);
    void groupDeleted(int page, const std::vector<int> &lines, const std::vector<int> &pictures);  // (... Delete: all of them)
    // a block's paragraphs from .. to (not including to) written again - in styled's runs, or as they read
    // with look over them - those after them moved up or down as much as they grew or shrank
    bool retypeSome(int page, const pagetext::TextFrame &frame, int from, int to,
                    const std::vector<std::vector<Document::StyledRun>> *styled, const Document::LineLook &look, QString *error);
    void setLayout(const QString &mode);
    // the Sign and Link tools
    void connectSignLink();
    QImage signatureDialog(QString *saved = nullptr);
    bool newSignature();
    void placeSignature(int page, QPointF at);
    void followLink(int page, int index);
    void newLink(int page, const QRectF &rect);
    void editLink(int page, int index);
    void deleteLink(int page, int index);

    static QString signaturePath();
    static void setSignaturePath(const QString &path);  // (the self-tests: not the user's signature)

    void layerPicture(bool front);
    // Acrobat's Objects panel: flipped, turned a quarter, lined up on the page, a step forward /
    // back, cropped, edited in another program (Paint...) and put back each time it's saved
    void transformPicked(const QTransform &move, const char *what);
    void flipPicture(bool across);
    void turnPicture(double degrees);
    void alignPicture(const QString &where);
    void stepPicture(bool forward);
    void cropPicture();
    void editPictureWith(const QString &program);
    void editImage();  // (the picture picked up, in the picture editor)
    void convertPictureText();
    void convertDrawnText(const Picture &pic, int page);  // (letters drawn as shapes, read and made text)  // (its text read - OCR - and made real text)
    // (the Format controls: on the letters selected while typing, else on the line / block picked up)
    void formatText(const Document::LineLook &look);
    void pictureMenu(const QString &which);
    QTreeWidget *annotationList() const { return annotList_; }
    QTreeWidget *bookmarkList() const { return bookmarkList_; }
    void showSideView(int which);  // 0: Pages, 1: Search results, 2: Annotations, 3: Bookmarks
    void checkForUpdates();  // (Help menu: asks GitHub for the newest version)
    // updates (UpdateTools.cpp, as the Python app's): just updated - what's new, once; then a newer version looked for
    void startupUpdateTasks();
    void showUpdateNotes();
    void offerUpdate(const updates::Release &r);       // (Update now / Not now / Skip this version)
    void showUpdateButton(const updates::Release &r);  // (the title bar's update button, for later)
    void installUpdate(const updates::Release &r);     // (downloaded and installed - Windows; the browser elsewhere)
    void toggleStyle(const QString &key);  // (the text bar: bold, italic, underline, strike)

protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *watched, QEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

private:
    void buildMenus();
    void buildToolbars(class QVBoxLayout *box);
    void buildStatusBar(class QVBoxLayout *box);
    void fillFileMenu(QMenu *m);
    void fillEditMenu(QMenu *m);
    void fillViewMenu(QMenu *m);
    void fillPagesMenu(QMenu *m);
    void fillThemeMenu(QMenu *m);
    void fillHelpMenu(QMenu *m);

    void openDialog();
    void closeFile();
    // the tabs: each PDF open, and where it was shown when another tab was chosen
    struct DocTab {
        std::shared_ptr<Document> doc;
        QString name;
        bool placed = false;  // (shown before: put back where it was)
        int page = 0;
        double zoom = 1;
        PageView::Fit fit = PageView::Fit::None;
        int scrollX = 0, scrollY = 0;
    };
    void tabChosen(int index);
    void keepTabPlace();
    void loadIntoView(std::shared_ptr<Document> doc, const QString &name);
    DocTabs *tabs_ = nullptr;
    HomePage *home_ = nullptr;
    QStackedWidget *pages_ = nullptr;  // (Home, or the PDF's tools and pages)
    QWidget *docPage_ = nullptr;
    std::vector<DocTab> docs_;
    int current_ = -1;  // (the PDF shown: docs_' index; -1 Home)
    std::weak_ptr<Document> editDoc_;
    void replacePages(const std::vector<int> &pages);  // (Organize's Replace: by another file's pages)
    // (Create PDF: the new PDF being put in order in Organize Pages - till it's named and created, or cancelled)
    std::weak_ptr<Document> creating_;
    bool isCreating(const std::shared_ptr<Document> &doc) const;
    void finishCreating(const QString &name);
    void cancelCreating();
    void startScan(const std::vector<int> &pages, const QRectF &area = QRectF());  // (ScanTools.cpp: the scan of the PDF shown, these pages - area: just that part of the page, its points)
    void scanPages();  // (Scan > Pages...: which, typed - "1-3, 6")
    void scanDialog();  // (Scan - ScanTools.cpp: a scanned page's text read, written over it unseen)
    // (Convert - ConvertTools.cpp: the PDF shown - or path, or one picked (Home) - as Word, Excel, pictures, web pages...)
    void convertDialog(const QString &path = QString());
    // (the scan read on a thread of its own - the app still used meanwhile, the scanned PDF's tab alone locked;
    // finished here, on the window's thread)
    std::shared_ptr<struct ScanJob> scan_;
    void scanFinished();
    void stopScan(const std::shared_ptr<Document> &doc);  // (its scan called off - its tab closed; nullptr: any)
    friend class ScanLock;
    void placeReadBar();
    QWidget *toolsBox_ = nullptr;          // (the toolbars: hidden in Read mode)
    class QStackedWidget *middleStack_ = nullptr;  // (the pages, or Organize Pages)
    OrganizeView *organize_ = nullptr;
    bool readMode_ = false;
    QString toolBeforeRead_;  // (the tool in use when Read mode came on: back when it goes)
    QWidget *readBar_ = nullptr;           // (Read mode's bar over the pages)
    class QLabel *readPage_ = nullptr;
    ToolButton *readButton_ = nullptr;     // (the status bar's Read)  // (the PDF a picture's being edited for, in another program)
    void dropped(const QStringList &paths);
    static bool isPdf(const QString &path);
    static bool isPicture(const QString &path);

    // saving, Undo, and the pages
    bool maybeSave();  // unsaved changes: saved first? false: don't go on
    bool write(const QString &path);
    void afterChange(int go, bool keep = false);
    // the tools
    PageView::ToolLook toolLook() const;
    void toolClicked(const QString &key);
    void toolMenu(const QString &key);
    void tintTools();
    void buildProps(QHBoxLayout *row, QWidget *props);
    void refreshProps();
    void pickColor();
    void connectTools();
    void annotChanged(bool ok, const QString &what, const QString &error);
    void editAnnot(int page, int id);
    bool marksOver(int page, const std::vector<QRectF> &boxes);
    void showTextBar();
    void showMarkBar(int page, int id);
    QString markedText(int page, const Annot &mark);
    void buildAnnotationList();
    void buildBookmarkList();
    void fillBookmarkList(bool fresh);  // (fresh: a new PDF - closed again, only the top level)
    bool annotationsShowing() const;
    void fillAnnotationList();
    void selectInList(int page, int id);
    // the text tools (step 5)
    void buildTextProps(QHBoxLayout *row, QWidget *props);
    void refreshTextProps(bool line);
    // (a box's value typed in, Enter: its line's returnPressed - and Qt's activated too, when it's one of the
    // list's values: put in once, not twice - the second time over text just retyped, its look read anew)
    bool oncePerKey();
    bool chosenNow_ = false;
    void restyleLine(const Document::LineLook &look);
    void lineChanged(bool ok, int page, const QRectF &near, const QString &error);
    void connectTextTools();
    void addRealText(int page, QPointF top, const QString &text);
    // pictures (step 6)
    void buildPictureProps(QHBoxLayout *row, QWidget *props);
    const Picture *pickedPicture();
    void pictureChangedDone(bool ok, int page, const QRectF &near, const QString &error);
    void connectPictureTools();
    void replacePicture();
    void savePicture();
    // Settings (step 7)
    void openSettings();
    QImage appIcon() const;
    void applyIcon();
    void fontsChanged();
    void failed(const QString &what, const QString &error);
    static bool pageStep(const QString &what);
    void updateUi();
    void say(const QString &text);
    void stepPage(int by);
    void goToTyped();
    void zoomStep(int d);
    void zoomTyped(const QString &text);
    void toggleThumbs();
    void openChooser();
    void notYet(const QString &what);
    void addRecent(const QString &path);
    void placeStartPanel();
    void themeChanged();

    // Find
    void focusFind();
    void findSoon();
    void findNext(int step);
    void goResult(int k, bool scroll = true);
    void searchDone(const QString &text, std::vector<FindHit> hits, bool jump);

    PageView *view_ = nullptr;
    QWidget *viewBox_ = nullptr;
    QSplitter *split_ = nullptr;
    QWidget *side_ = nullptr;
    ChooserButton *chooser_ = nullptr;
    QStackedWidget *sideStack_ = nullptr;
    ThumbnailBar *thumbs_ = nullptr;
    SearchResults *results_ = nullptr;
    StartPanel *start_ = nullptr;
    QMenuBar *menuBar_ = nullptr;
    QHash<QString, ToolButton *> buttons_;
    QLineEdit *pageBox_ = nullptr;
    QLabel *pageCount_ = nullptr;
    QComboBox *zoomBox_ = nullptr;
    QLineEdit *findBox_ = nullptr;
    QLabel *propsTitle_ = nullptr;
    EngravedLabel *propsHint_ = nullptr;
    StatusPanel *msg_ = nullptr;
    StatusPanel *pageLabel_ = nullptr;
    StatusPanel *sizeLabel_ = nullptr;
    StatusPanel *zoomLabel_ = nullptr;
    QString name_;  // the open file's name
    bool showThumbs_ = true;
    // the tools: which, and how each draws
    QString tool_ = QStringLiteral("select");
    QMap<QString, QString> variant_{{"eraser", "whiteout"}, {"image", "file"}, {"shape", "rect"}, {"sign", "place"}};
    QMap<QString, QColor> colors_{{"text", QColor("#000000")}, {"highlight", QColor("#FFFF00")}, {"pen", QColor("#FF0000")},
                                  {"eraser", QColor("#FFFFFF")}, {"shape", QColor("#FF0000")}};
    QMap<QString, double> sizes_{{"text", 12}, {"pen", 2}, {"shape", 2}, {"highlight", 12}, {"eraser", 16}};
    QMap<QString, int> opacities_{{"highlight", 100}, {"marker", 60}, {"pen", 100}, {"shape", 100}};
    bool propsLoading_ = false;
    QWidget *colorGroup_ = nullptr, *widthGroup_ = nullptr, *opacityGroup_ = nullptr;
    ToolButton *colorButton_ = nullptr;
    QLabel *widthTitle_ = nullptr, *widthLabel_ = nullptr, *opacityLabel_ = nullptr;
    TrackBar *widthBar_ = nullptr, *opacityBar_ = nullptr;
    QTreeWidget *annotList_ = nullptr;
    // Add text's look, and the text bar's parts
    QString textFamily_ = QStringLiteral("Arial");
    bool textBold_ = false, textItalic_ = false, textUnderline_ = false, textStrike_ = false;
    QString textAlign_ = QStringLiteral("left"), textDir_;
    QWidget *textGroup_ = nullptr;
    QComboBox *fontBox_ = nullptr, *sizeBox_ = nullptr;
    // (Acrobat's Format panel: line / paragraph spacing, the letters' width and spacing)
    QComboBox *lineSpacingBox_ = nullptr, *paraSpacingBox_ = nullptr, *scaleBox_ = nullptr, *trackBox_ = nullptr;
    std::vector<QWidget *> formatOnly_;  // (shown for text picked up: the spacing boxes and their pictures)
    QWidget *formatSep_ = nullptr, *dirSep_ = nullptr;
    QString listBullet_ = QStringLiteral("\u2022"), listNumber_ = QStringLiteral("1.");
    void listMenu(bool numbers);
    QWidget *pictureGroup_ = nullptr;
    QWidget *boxesGroup_ = nullptr;  // (Show bounding boxes: the Edit tool's)
    class QCheckBox *boxesCheck_ = nullptr;
    class QFileSystemWatcher *editWatch_ = nullptr;  // (a picture being edited in another program)
    QString editFile_;
    class QTimer *editSettle_ = nullptr;  // (the program's saves settled: put back once)
    int editPage_ = -1;
    QRectF editBox_;
    QString jobFolder() const;
    std::unique_ptr<QTemporaryDir> convertDir_;  // (Word documents made PDFs, while the app runs)
    EngravedLabel *annotNote_ = nullptr;
    QTreeWidget *bookmarkList_ = nullptr;  // (the side panel's Bookmarks: index 3)
    EngravedLabel *bookmarkNote_ = nullptr;

    QTimer *findTimer_ = nullptr;
    QString findText_;  // what was found (hits_ are its places)
    std::vector<FindHit> hits_;
    int findPos_ = -1;
    int viewBeforeFind_ = -1;  // what the side panel showed before the results
    std::shared_ptr<std::atomic<int>> findSerial_ = std::make_shared<std::atomic<int>>(0);
};
