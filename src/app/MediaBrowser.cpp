#include "app/MediaBrowser.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "app/PreviewCache.h"
#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kTypeRole = Qt::UserRole;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kFolderType = 0;
constexpr int kMediaType = 1;

}  // namespace

// QListWidget with manual drag-out (the view's built-in icon-mode drag would
// not reliably initiate a custom-MIME drag) and drop handling for reparenting
// media onto folders and importing external files.
class MediaBrowser::BinView : public QListWidget {
public:
    explicit BinView(MediaBrowser* owner)
        : QListWidget(owner)
        , m_owner(owner)
    {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);  // drags are started by hand below
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setUniformItemSizes(true);
        setWordWrap(true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPos = event->pos();
        }
        QListWidget::mousePressEvent(event);  // keep selection behavior
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton)
            && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            if (QListWidgetItem* item = itemAt(m_pressPos);
                item && item->data(kTypeRole).toInt() == kMediaType) {
                auto* mime = new QMimeData;
                mime->setData(kMediaMimeType,
                              QByteArray::number(static_cast<qulonglong>(item->data(kIdRole).toULongLong())));
                auto* drag = new QDrag(this);
                drag->setMimeData(mime);
                const QPixmap pixmap = item->icon().pixmap(96, 54);
                if (!pixmap.isNull()) {
                    drag->setPixmap(pixmap);
                    drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
                }
                drag->exec(Qt::CopyAction);
                return;
            }
        }
        QListWidget::mouseMoveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasFormat(kMediaMimeType) || event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        // Accept anywhere in the panel so the cursor never shows "forbidden";
        // dropEvent decides what actually happens (reparent vs. no-op vs. import).
        if (event->mimeData()->hasFormat(kMediaMimeType) || event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        } else {
            event->ignore();
        }
    }

    void dropEvent(QDropEvent* event) override
    {
        const QListWidgetItem* item = itemAt(event->position().toPoint());
        const bool onFolder = item && item->data(kTypeRole).toInt() == kFolderType;

        if (event->mimeData()->hasFormat(kMediaMimeType)) {
            if (onFolder) {
                m_owner->moveMediaToFolder(event->mimeData()->data(kMediaMimeType).toULongLong(),
                                           static_cast<FolderId>(item->data(kIdRole).toULongLong()));
                event->acceptProposedAction();
            }
        } else if (event->mimeData()->hasUrls()) {
            const FolderId target = onFolder ? static_cast<FolderId>(item->data(kIdRole).toULongLong())
                                             : m_owner->currentFolder();
            QStringList paths;
            for (const QUrl& url : event->mimeData()->urls()) {
                if (url.isLocalFile()) {
                    paths << url.toLocalFile();
                }
            }
            if (!paths.isEmpty()) {
                m_owner->importFiles(paths, target);
            }
            event->acceptProposedAction();
        }
    }

private:
    MediaBrowser* m_owner;
    QPoint m_pressPos;
};

MediaBrowser::MediaBrowser(QWidget* parent)
    : QWidget(parent)
{
    m_upButton = new QToolButton(this);
    m_upButton->setText("↑");
    m_upButton->setToolTip("Up one folder");
    m_upButton->setAutoRaise(true);
    connect(m_upButton, &QToolButton::clicked, this, &MediaBrowser::goUp);

    m_breadcrumb = new QLabel("Media", this);
    m_breadcrumb->setStyleSheet("color: #b0b0b0;");

    m_viewButton = new QToolButton(this);
    m_viewButton->setText("List");
    m_viewButton->setToolTip("Toggle icon / list view");
    m_viewButton->setAutoRaise(true);
    connect(m_viewButton, &QToolButton::clicked, this, &MediaBrowser::toggleViewMode);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(m_upButton);
    bar->addWidget(m_breadcrumb, 1);
    bar->addWidget(m_viewButton);

    m_view = new BinView(this);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &QListWidget::itemActivated, this, &MediaBrowser::onItemActivated);
    connect(m_view, &QListWidget::itemDoubleClicked, this, &MediaBrowser::onItemActivated);
    connect(m_view, &QWidget::customContextMenuRequested, this, &MediaBrowser::showContextMenu);
    connect(m_view, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current, QListWidgetItem*) {
        if (current && current->data(kTypeRole).toInt() == kMediaType) {
            emit mediaSelected(static_cast<MediaId>(current->data(kIdRole).toULongLong()));
        } else {
            emit mediaSelected(kInvalidMedia);
        }
    });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(m_view);

    // Default to icon view.
    m_view->setViewMode(QListView::IconMode);
    m_view->setIconSize(QSize(96, 54));
    m_view->setGridSize(QSize(120, 86));
    m_viewButton->setText("List");
}

void MediaBrowser::setProject(const Project* project)
{
    m_project = project;
    m_current = kRootFolder;
    refresh();
}

void MediaBrowser::toggleViewMode()
{
    m_iconMode = !m_iconMode;
    if (m_iconMode) {
        m_view->setViewMode(QListView::IconMode);
        m_view->setIconSize(QSize(96, 54));
        m_view->setGridSize(QSize(120, 86));
        m_viewButton->setText("List");
    } else {
        m_view->setViewMode(QListView::ListMode);
        m_view->setIconSize(QSize(20, 20));
        m_view->setGridSize(QSize());
        m_viewButton->setText("Icons");
    }
    refresh();
}

void MediaBrowser::enterFolder(FolderId folder)
{
    m_current = folder;
    refresh();
}

void MediaBrowser::goUp()
{
    if (m_current == kRootFolder || !m_project) {
        return;
    }
    for (const BinFolder& folder : m_project->folders()) {
        if (folder.id == m_current) {
            m_current = folder.parent;
            break;
        }
    }
    refresh();
}

void MediaBrowser::onItemActivated(QListWidgetItem* item)
{
    if (item && item->data(kTypeRole).toInt() == kFolderType) {
        enterFolder(static_cast<FolderId>(item->data(kIdRole).toULongLong()));
    }
}

void MediaBrowser::moveMediaToFolder(MediaId media, FolderId folder)
{
    emit mediaMovedToFolder(media, folder);
}

void MediaBrowser::importFiles(const QStringList& paths, FolderId folder)
{
    emit filesImported(paths, folder);
}

void MediaBrowser::updateBreadcrumb()
{
    if (!m_project) {
        m_breadcrumb->setText("Media");
        return;
    }
    QStringList parts;
    FolderId walk = m_current;
    while (walk != 0) {
        if (walk == kRootFolder) {
            parts.prepend("Media");
            break;
        }
        for (const BinFolder& folder : m_project->folders()) {
            if (folder.id == walk) {
                parts.prepend(QString::fromStdString(folder.name));
                walk = folder.parent;
                break;
            }
        }
    }
    m_breadcrumb->setText(parts.join("  ›  "));
    m_upButton->setEnabled(m_current != kRootFolder);
}

void MediaBrowser::refresh()
{
    m_view->clear();
    if (!m_project) {
        return;
    }

    const QIcon folderIcon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon fileIcon = style()->standardIcon(QStyle::SP_FileIcon);

    for (const BinFolder& folder : m_project->folders()) {
        if (folder.parent != m_current || folder.id == kRootFolder) {
            continue;
        }
        auto* item = new QListWidgetItem(folderIcon, QString::fromStdString(folder.name), m_view);
        item->setData(kTypeRole, kFolderType);
        item->setData(kIdRole, static_cast<qulonglong>(folder.id));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled);
    }

    for (const MediaSource& media : m_project->mediaPool()) {
        if (media.folder != m_current) {
            continue;
        }
        QIcon icon = fileIcon;
        if (m_previews) {
            if (const PreviewCache::Thumbnails* thumbs = m_previews->thumbnails(media.id)) {
                if (!thumbs->images.empty()) {
                    icon = QIcon(QPixmap::fromImage(thumbs->images.front()));
                }
            }
        }
        auto* item = new QListWidgetItem(icon, QFileInfo(QString::fromStdString(media.path)).fileName(), m_view);
        item->setData(kTypeRole, kMediaType);
        item->setData(kIdRole, static_cast<qulonglong>(media.id));
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }

    updateBreadcrumb();
}

void MediaBrowser::showContextMenu(const QPoint& pos)
{
    const QListWidgetItem* item = m_view->itemAt(pos);
    const bool onFolder = item && item->data(kTypeRole).toInt() == kFolderType;

    QMenu menu(this);
    QAction* newFolder = menu.addAction("New Folder");
    QAction* import = menu.addAction("Import Media…");
    QAction* del = nullptr;
    if (onFolder) {
        menu.addSeparator();
        del = menu.addAction("Delete Folder");
    }

    QAction* chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == newFolder) {
        emit newFolderRequested(m_current);
    } else if (chosen == import) {
        emit importRequested(m_current);
    } else if (del && chosen == del) {
        emit deleteFolderRequested(static_cast<FolderId>(item->data(kIdRole).toULongLong()));
    }
}

}  // namespace hopline
