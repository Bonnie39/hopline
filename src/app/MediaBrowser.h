#pragma once

#include <QStringList>
#include <QWidget>

#include "model/Media.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class QToolButton;
class QPoint;

namespace hopline {

class Project;
class PreviewCache;

// MIME type carrying a MediaId when a bin item is dragged (to the timeline, or
// onto a folder to reorganize).
inline constexpr char kMediaMimeType[] = "application/x-hopline-media";

// The bin: a navigable, Finder-style view over the project's media pool. The
// panel itself is the root folder — you enter subfolders and a breadcrumb tracks
// the path. Icon or list display. A view over the model: it emits intents and
// never mutates the project.
class MediaBrowser : public QWidget {
    Q_OBJECT

public:
    explicit MediaBrowser(QWidget* parent = nullptr);

    void setProject(const Project* project);
    void setPreviewCache(const PreviewCache* previews) { m_previews = previews; }
    void refresh();

    FolderId currentFolder() const { return m_current; }

    // Called by the internal view on drops.
    void moveMediaToFolder(MediaId media, FolderId folder);
    void importFiles(const QStringList& paths, FolderId folder);

signals:
    void newFolderRequested(FolderId parent);
    void importRequested(FolderId folder);
    void deleteFolderRequested(FolderId folder);
    void mediaMovedToFolder(MediaId media, FolderId folder);
    void filesImported(const QStringList& paths, FolderId folder);
    void mediaSelected(MediaId media);  // kInvalidMedia when a folder or nothing is selected

private:
    void enterFolder(FolderId folder);
    void goUp();
    void toggleViewMode();
    void onItemActivated(QListWidgetItem* item);
    void showContextMenu(const QPoint& pos);
    void updateBreadcrumb();

    class BinView;
    BinView* m_view = nullptr;
    QLabel* m_breadcrumb = nullptr;
    QToolButton* m_upButton = nullptr;
    QToolButton* m_viewButton = nullptr;

    const Project* m_project = nullptr;
    const PreviewCache* m_previews = nullptr;
    FolderId m_current = kRootFolder;
    bool m_iconMode = true;
};

}  // namespace hopline
