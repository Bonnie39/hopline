#pragma once

#include <QColor>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "model/Media.h"

class QAbstractItemView;
class QLabel;
class QLineEdit;
class QPixmap;
class QPoint;
class QSlider;
class QStackedWidget;
class QToolButton;

namespace hopline {

class Project;
class PreviewCache;
class Sequence;

// MIME carrying the single primary MediaId (kept single so TimelineWidget's drop
// path is unchanged), plus a comma-joined list of every selected id for
// bin-internal bulk reparenting.
inline constexpr char kMediaMimeType[] = "application/x-hopline-media";
inline constexpr char kMediaListMimeType[] = "application/x-hopline-media-list";

// Fixed cosmetic label palette. Index 0 is "none"; 1..labelCount() map into it.
int labelCount();
QColor labelColor(int label);
QString labelName(int label);

// The bin: a navigable, Finder-style view over the project's media pool. Icon
// view shows custom tiles with hover-scrub thumbnails; list view shows sortable
// columns. Multi-select + bulk ops. A view over the model: it emits intents and
// never mutates the project.
class MediaBrowser : public QWidget {
    Q_OBJECT

public:
    explicit MediaBrowser(QWidget* parent = nullptr);
    ~MediaBrowser() override;

    void setProject(const Project* project);
    void setPreviewCache(const PreviewCache* previews) { m_previews = previews; }
    void refresh();

    FolderId currentFolder() const { return m_current; }

    // Selects the folder in the active view and starts an inline rename edit.
    void beginRenameFolder(FolderId folder);

signals:
    void newFolderRequested(FolderId parent);
    void importRequested(FolderId folder);
    void deleteFolderRequested(FolderId folder);
    void mediaMovedToFolder(QList<MediaId> media, FolderId folder);
    void filesImported(QStringList paths, FolderId folder);
    void mediaSelected(MediaId media);  // kInvalidMedia when nothing / a folder is current
    void deleteMediaRequested(QList<MediaId> media);
    void mediaLabelChanged(QList<MediaId> media, int label);
    void folderLabelChanged(FolderId folder, int label);
    void folderRenamed(FolderId folder, QString name);
    void newSequenceRequested(FolderId folder);
    void sequenceActivated(SequenceId sequence);
    void sequenceRenamed(SequenceId sequence, QString name);
    void deleteSequenceRequested(SequenceId sequence);

private:
    class IconView;
    class ListView;
    class TileDelegate;

    // Called back by the internal views.
    void enterFolder(FolderId folder);
    void goUp();
    void reportSelection(MediaId primary);
    void startMediaDrag(QAbstractItemView* view, const QList<MediaId>& ids, MediaId primary,
                        const QPixmap& pixmap);
    void dropOnFolder(const QList<MediaId>& ids, FolderId folder);
    void dropFiles(const QStringList& paths, FolderId folder);
    void showMenu(const QPoint& globalPos, const QList<MediaId>& media, FolderId folderUnderCursor,
                  SequenceId sequenceUnderCursor);
    void commitRename(FolderId folder, const QString& name);
    void beginRenameSequence(SequenceId sequence);
    void openSequence(SequenceId sequence) { emit sequenceActivated(sequence); }

    void setIconMode(bool icon);
    void setThumbSize(int width);
    void setSortKey(int key);
    void populate();
    void addFolderRow(const BinFolder& folder);
    void addMediaRow(const MediaSource& media);
    void addSequenceRow(const Sequence& sequence);
    void commitSequenceRename(SequenceId sequence, const QString& name);
    void updateBreadcrumb();
    const PreviewCache* previews() const { return m_previews; }
    const Project* project() const { return m_project; }

    IconView* m_iconView = nullptr;
    ListView* m_listView = nullptr;
    TileDelegate* m_delegate = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_breadcrumb = nullptr;
    QToolButton* m_upButton = nullptr;
    QToolButton* m_viewButton = nullptr;
    QToolButton* m_sortButton = nullptr;
    QLineEdit* m_filter = nullptr;
    QSlider* m_sizeSlider = nullptr;

    const Project* m_project = nullptr;
    const PreviewCache* m_previews = nullptr;
    FolderId m_current = kRootFolder;
    bool m_iconMode = true;
    int m_thumbWidth = 150;
    int m_sortKey = 0;  // 0 = name, 1 = duration, 2 = type
    QString m_filterText;
};

}  // namespace hopline
