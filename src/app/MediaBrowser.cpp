#include "app/MediaBrowser.h"

#include <algorithm>
#include <cmath>

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "app/PreviewCache.h"
#include "model/Project.h"

namespace hopline {
namespace {

constexpr int kTypeRole = Qt::UserRole;
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kDurationRole = Qt::UserRole + 2;
constexpr int kVideoRole = Qt::UserRole + 3;
constexpr int kAudioRole = Qt::UserRole + 4;
constexpr int kLabelRole = Qt::UserRole + 5;
constexpr int kFpsRole = Qt::UserRole + 6;

constexpr int kFolderType = 0;
constexpr int kMediaType = 1;

constexpr int kColName = 0;
constexpr int kColType = 1;
constexpr int kColDuration = 2;
constexpr int kColFps = 3;
constexpr int kColPath = 4;
constexpr int kColCount = 5;

constexpr int kPad = 6;
constexpr int kNameH = 20;

constexpr int kMinThumb = 90;
constexpr int kMaxThumb = 260;

struct LabelDef {
    const char* name;
    QColor color;
};

const LabelDef kLabels[] = {
    { "Rose", QColor(0xE0, 0x6C, 0x75) },   { "Mango", QColor(0xE0, 0x9A, 0x4C) },
    { "Yellow", QColor(0xE5, 0xC0, 0x7B) }, { "Green", QColor(0x8C, 0xC2, 0x65) },
    { "Teal", QColor(0x56, 0xB6, 0xC2) },   { "Blue", QColor(0x5A, 0x9B, 0xE0) },
    { "Violet", QColor(0x9E, 0x7B, 0xD8) }, { "Gray", QColor(0x9A, 0x9A, 0x9A) },
};

QString shortDuration(Tick ticks)
{
    const int total = static_cast<int>(secondsFromTicks(ticks) + 0.5);
    return QString("%1:%2").arg(total / 60).arg(total % 60, 2, 10, QChar('0'));
}

QString typeText(bool video, bool audio)
{
    if (video && audio) return "V+A";
    if (video) return "V";
    if (audio) return "A";
    return "";
}

// The thumbnail sub-rect at the top of a tile; hover-scrub maps mouse x across it.
QRect thumbRect(const QRect& item)
{
    const int w = item.width() - 2 * kPad;
    return QRect(item.left() + kPad, item.top() + kPad, w, w * 9 / 16);
}

QPixmap labelSwatch(int label, int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(labelColor(label));
    p.drawRoundedRect(QRectF(0.5, 0.5, size - 1, size - 1), 3, 3);
    return pm;
}

void paintFolder(QPainter& p, const QRectF& box, const QColor& color)
{
    const double s = std::min(box.width(), box.height()) * 0.44;
    const QPointF c = box.center();
    const QRectF f(c.x() - s, c.y() - s * 0.68, 2 * s, 1.36 * s);
    const double tabH = f.height() * 0.22;
    QPainterPath tab;
    tab.addRoundedRect(QRectF(f.left(), f.top(), f.width() * 0.5, tabH * 2.2), 2, 2);
    QPainterPath body;
    body.addRoundedRect(QRectF(f.left(), f.top() + tabH, f.width(), f.height() - tabH), 3, 3);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(tab);
    p.drawPath(body);
}

const QColor kFolderGray(150, 152, 158);

QPixmap folderPixmap(int size, const QColor& color = kFolderGray)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    paintFolder(p, QRectF(0, 0, size, size), color);
    return pm;
}

void drawBadge(QPainter& p, const QRect& thumb, const QString& text, bool rightAlign)
{
    if (text.isEmpty()) return;
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const int w = fm.horizontalAdvance(text) + 8;
    const int h = 15;
    const int x = rightAlign ? thumb.right() - w - 3 : thumb.left() + 3;
    const QRect box(x, thumb.bottom() - h - 3, w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(box, 3, 3);
    p.setPen(QColor(235, 235, 235));
    p.drawText(box, Qt::AlignCenter, text);
}

}  // namespace

int labelCount() { return static_cast<int>(std::size(kLabels)); }

QColor labelColor(int label)
{
    if (label < 1 || label > labelCount()) return QColor();
    return kLabels[label - 1].color;
}

QString labelName(int label)
{
    if (label < 1 || label > labelCount()) return QString();
    return kLabels[label - 1].name;
}

// ── Icon-view tile delegate ────────────────────────────────────────────────
class MediaBrowser::TileDelegate : public QStyledItemDelegate {
public:
    TileDelegate(MediaBrowser* owner)
        : QStyledItemDelegate(owner)
        , m_owner(owner)
    {
    }

    void setHover(MediaId id, double frac)
    {
        m_hoverId = id;
        m_hoverFrac = frac;
    }
    void setThumbWidth(int w) { m_thumbWidth = w; }

    // Put the rename editor over the name area, not the whole tile.
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override
    {
        const QRect r = option.rect;
        editor->setGeometry(r.left() + kPad, thumbRect(r).bottom() + 4, r.width() - 2 * kPad, kNameH);
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        const int tileW = m_thumbWidth + 2 * kPad;
        const int tileH = kPad + m_thumbWidth * 9 / 16 + 4 + kNameH + kPad;
        return { tileW, tileH };
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        const QRect r = option.rect;
        const bool selected = option.state & QStyle::State_Selected;
        const int type = index.data(kTypeRole).toInt();

        if (selected) {
            painter->setPen(QPen(QColor(255, 255, 255, 40), 1));
            painter->setBrush(QColor(255, 255, 255, 22));  // neutral gray, not blue
            painter->drawRoundedRect(QRectF(r).adjusted(1.5, 1.5, -1.5, -1.5), 8, 8);
        }

        const QRect thumb = thumbRect(r);
        if (type == kFolderType) {
            const int lbl = index.data(kLabelRole).toInt();
            paintFolder(*painter, thumb, lbl > 0 ? labelColor(lbl) : kFolderGray);
        } else {
            QPainterPath clip;
            clip.addRoundedRect(thumb, 5, 5);
            painter->setClipPath(clip);
            drawThumbnail(*painter, thumb, index);
            painter->setClipping(false);
            painter->setPen(QPen(QColor(255, 255, 255, 20), 1));  // crisp thumbnail frame
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(thumb).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
        }

        // Label stripe across the top of the thumbnail.
        const int label = index.data(kLabelRole).toInt();
        if (label > 0) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(labelColor(label));
            painter->drawRoundedRect(QRect(thumb.left(), thumb.top(), thumb.width(), 4), 2, 2);
        }

        if (type == kMediaType) {
            drawBadge(*painter, thumb, typeText(index.data(kVideoRole).toBool(),
                                                index.data(kAudioRole).toBool()),
                      /*rightAlign*/ false);
            drawBadge(*painter, thumb, shortDuration(index.data(kDurationRole).toLongLong()),
                      /*rightAlign*/ true);

            // Hover-scrub progress line.
            if (index.data(kIdRole).toULongLong() == m_hoverId && m_hoverFrac >= 0.0) {
                const int x = thumb.left() + static_cast<int>(m_hoverFrac * thumb.width());
                painter->setPen(QPen(QColor(255, 255, 255, 200), 2));
                painter->drawLine(x, thumb.bottom() - 2, x, thumb.bottom());
            }
        }

        // Filename / folder name.
        const QRect nameRect(r.left() + kPad, thumb.bottom() + 4, r.width() - 2 * kPad, kNameH);
        QFont nf = painter->font();
        nf.setPixelSize(12);
        painter->setFont(nf);
        painter->setPen(selected ? QColor(240, 240, 240) : QColor(200, 200, 200));
        const QString name = index.data(Qt::DisplayRole).toString();
        const QString elided = painter->fontMetrics().elidedText(name, Qt::ElideMiddle, nameRect.width());
        painter->drawText(nameRect, Qt::AlignHCenter | Qt::AlignTop, elided);

        painter->restore();
    }

private:
    void drawThumbnail(QPainter& p, const QRect& box, const QModelIndex& index) const
    {
        const MediaId id = index.data(kIdRole).toULongLong();
        const PreviewCache::Thumbnails* thumbs = m_owner->previews() ? m_owner->previews()->thumbnails(id) : nullptr;
        if (!thumbs || thumbs->images.empty()) {
            p.fillRect(box, QColor(30, 31, 34));
            p.setPen(QColor(90, 92, 96));
            QFont f = p.font();
            f.setPixelSize(10);
            p.setFont(f);
            p.drawText(box, Qt::AlignCenter, "no preview");
            return;
        }
        const int n = static_cast<int>(thumbs->images.size());
        int idx = 0;
        if (id == m_hoverId && n > 1 && m_hoverFrac >= 0.0) {
            idx = std::clamp(static_cast<int>(std::lround(m_hoverFrac * (n - 1))), 0, n - 1);
        }
        p.fillRect(box, QColor(20, 21, 23));
        const QImage& img = thumbs->images[idx];
        const QSize scaled = img.size().scaled(box.size(), Qt::KeepAspectRatio);
        const QRect dst(box.left() + (box.width() - scaled.width()) / 2,
                        box.top() + (box.height() - scaled.height()) / 2, scaled.width(), scaled.height());
        p.drawImage(dst, img);
    }

    MediaBrowser* m_owner;
    MediaId m_hoverId = 0;
    double m_hoverFrac = -1.0;
    int m_thumbWidth = 150;
};

// ── Icon view ──────────────────────────────────────────────────────────────
class MediaBrowser::IconView : public QListWidget {
public:
    explicit IconView(MediaBrowser* owner)
        : QListWidget(owner)
        , m_owner(owner)
    {
        setViewMode(QListView::IconMode);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setWrapping(true);
        setUniformItemSizes(true);
        setSpacing(6);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setSelectionRectVisible(true);
        setDragDropMode(QAbstractItemView::DropOnly);  // drags started by hand below
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);  // external file drops land on the viewport
        setEditTriggers(QAbstractItemView::NoEditTriggers);  // rename is driven explicitly
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) m_pressPos = event->pos();
        QListWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override
    {
        QListWidgetItem* item = itemAt(event->pos());
        if (item && item->data(kTypeRole).toInt() == kFolderType) {
            // Double-click the name label renames; double-click the thumbnail enters.
            const QRect r = visualItemRect(item);
            const QRect nameRect(r.left() + kPad, thumbRect(r).bottom() + 4, r.width() - 2 * kPad, kNameH);
            if (nameRect.contains(event->pos())) {
                editItem(item);
            } else {
                m_owner->enterFolder(static_cast<FolderId>(item->data(kIdRole).toULongLong()));
            }
            return;
        }
        QListWidget::mouseDoubleClickEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_F2) {
            if (QListWidgetItem* item = currentItem();
                item && item->data(kTypeRole).toInt() == kFolderType) {
                editItem(item);
                return;
            }
        }
        QListWidget::keyPressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton)
            && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            // Only drag media; press on empty space / a folder keeps normal
            // rubber-band selection.
            if (const QListWidgetItem* pressed = itemAt(m_pressPos);
                pressed && pressed->data(kTypeRole).toInt() == kMediaType) {
                beginDrag();
                return;
            }
        }
        updateHover(event->pos());
        QListWidget::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override
    {
        if (m_hoverId != 0) {
            m_hoverId = 0;
            m_owner->m_delegate->setHover(0, -1.0);
            viewport()->update();
        }
        QListWidget::leaveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override { acceptIfMedia(event); }
    void dragMoveEvent(QDragMoveEvent* event) override { acceptIfMedia(event); }

    void dropEvent(QDropEvent* event) override
    {
        const QListWidgetItem* item = itemAt(event->position().toPoint());
        const bool onFolder = item && item->data(kTypeRole).toInt() == kFolderType;
        if (event->mimeData()->hasFormat(kMediaListMimeType)) {
            if (onFolder) {
                m_owner->dropOnFolder(parseIds(event->mimeData()->data(kMediaListMimeType)),
                                      static_cast<FolderId>(item->data(kIdRole).toULongLong()));
                event->acceptProposedAction();
            }
        } else if (event->mimeData()->hasUrls()) {
            const FolderId target = onFolder ? static_cast<FolderId>(item->data(kIdRole).toULongLong())
                                             : m_owner->currentFolder();
            m_owner->dropFiles(localFiles(event->mimeData()), target);
            event->acceptProposedAction();
        }
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        const QListWidgetItem* item = itemAt(event->pos());
        const FolderId folder = (item && item->data(kTypeRole).toInt() == kFolderType)
                                    ? static_cast<FolderId>(item->data(kIdRole).toULongLong())
                                    : 0;
        m_owner->showMenu(event->globalPos(), selectedMedia(this), folder);
    }

private:
    void updateHover(const QPoint& pos)
    {
        MediaId id = 0;
        double frac = -1.0;
        if (const QListWidgetItem* item = itemAt(pos); item && item->data(kTypeRole).toInt() == kMediaType) {
            const QRect tr = thumbRect(visualItemRect(item));
            id = item->data(kIdRole).toULongLong();
            frac = std::clamp((pos.x() - tr.left()) / static_cast<double>(std::max(1, tr.width())), 0.0, 1.0);
        }
        if (id != m_hoverId || (id != 0 && std::abs(frac - m_hoverFrac) > 0.001)) {
            m_hoverId = id;
            m_hoverFrac = frac;
            m_owner->m_delegate->setHover(id, frac);
            viewport()->update();
        }
    }

    void beginDrag()
    {
        const QList<MediaId> ids = selectedMedia(this);
        if (ids.isEmpty()) return;
        MediaId primary = ids.front();
        if (const QListWidgetItem* pressed = itemAt(m_pressPos);
            pressed && pressed->data(kTypeRole).toInt() == kMediaType) {
            primary = pressed->data(kIdRole).toULongLong();
        }
        QPixmap pm;
        if (const auto* t = m_owner->previews() ? m_owner->previews()->thumbnails(primary) : nullptr;
            t && !t->images.empty()) {
            pm = QPixmap::fromImage(t->images.front()).scaled(96, 54, Qt::KeepAspectRatio,
                                                              Qt::SmoothTransformation);
        }
        m_owner->startMediaDrag(this, ids, primary, pm);
        // exec() ate the mouse release; clear the drag-selecting state so the
        // rubber band doesn't resume when the cursor returns to the view.
        setState(QAbstractItemView::NoState);
        viewport()->update();
    }

    MediaBrowser* m_owner;
    QPoint m_pressPos;
    MediaId m_hoverId = 0;
    double m_hoverFrac = -1.0;

public:
    // Shared helpers, also used by ListView.
    static void acceptIfMedia(QDropEvent* event)
    {
        if (event->mimeData()->hasFormat(kMediaListMimeType) || event->mimeData()->hasUrls())
            event->acceptProposedAction();
        else
            event->ignore();
    }
    static QList<MediaId> selectedMedia(const QAbstractItemView* view);
    static QList<MediaId> parseIds(const QByteArray& data)
    {
        QList<MediaId> ids;
        for (const QByteArray& part : data.split(',')) {
            if (!part.isEmpty()) ids << part.toULongLong();
        }
        return ids;
    }
    static QStringList localFiles(const QMimeData* mime)
    {
        QStringList paths;
        for (const QUrl& url : mime->urls()) {
            if (url.isLocalFile()) paths << url.toLocalFile();
        }
        return paths;
    }
};

// ── List view ──────────────────────────────────────────────────────────────
namespace {

class BinItem : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const int ta = data(0, kTypeRole).toInt();
        const int tb = other.data(0, kTypeRole).toInt();
        if (ta != tb) return ta < tb;  // folders (0) group before media (1)
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == kColDuration || col == kColFps) {
            return data(col, Qt::UserRole).toDouble() < other.data(col, Qt::UserRole).toDouble();
        }
        return text(col).compare(other.text(col), Qt::CaseInsensitive) < 0;
    }
};

}  // namespace

class MediaBrowser::ListView : public QTreeWidget {
public:
    explicit ListView(MediaBrowser* owner)
        : QTreeWidget(owner)
        , m_owner(owner)
    {
        setColumnCount(kColCount);
        setHeaderLabels({ "Name", "Type", "Duration", "FPS", "Path" });
        setRootIsDecorated(false);
        setUniformRowHeights(true);
        setAlternatingRowColors(true);
        setSortingEnabled(true);
        sortByColumn(kColName, Qt::AscendingOrder);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setSelectionBehavior(QAbstractItemView::SelectRows);
        setDragDropMode(QAbstractItemView::DropOnly);
        setAcceptDrops(true);
        viewport()->setAcceptDrops(true);  // external file drops land on the viewport
        setEditTriggers(QAbstractItemView::NoEditTriggers);  // rename is driven explicitly
        header()->setSectionResizeMode(kColName, QHeaderView::Stretch);
        header()->setSectionResizeMode(kColPath, QHeaderView::Interactive);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) m_pressPos = event->pos();
        QTreeWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_F2) {
            if (QTreeWidgetItem* item = currentItem();
                item && item->data(0, kTypeRole).toInt() == kFolderType) {
                editItem(item, kColName);
                return;
            }
        }
        QTreeWidget::keyPressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if ((event->buttons() & Qt::LeftButton)
            && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            if (const QTreeWidgetItem* pressed = itemAt(m_pressPos);
                pressed && pressed->data(0, kTypeRole).toInt() == kMediaType) {
                const QList<MediaId> ids = IconView::selectedMedia(this);
                if (!ids.isEmpty()) {
                    m_owner->startMediaDrag(this, ids, pressed->data(0, kIdRole).toULongLong(), QPixmap());
                    setState(QAbstractItemView::NoState);  // exec() ate the mouse release
                    return;
                }
            }
        }
        QTreeWidget::mouseMoveEvent(event);
    }

    void dragEnterEvent(QDragEnterEvent* event) override { IconView::acceptIfMedia(event); }
    void dragMoveEvent(QDragMoveEvent* event) override { IconView::acceptIfMedia(event); }

    void dropEvent(QDropEvent* event) override
    {
        const QTreeWidgetItem* item = itemAt(event->position().toPoint());
        const bool onFolder = item && item->data(0, kTypeRole).toInt() == kFolderType;
        if (event->mimeData()->hasFormat(kMediaListMimeType)) {
            if (onFolder) {
                m_owner->dropOnFolder(IconView::parseIds(event->mimeData()->data(kMediaListMimeType)),
                                      static_cast<FolderId>(item->data(0, kIdRole).toULongLong()));
                event->acceptProposedAction();
            }
        } else if (event->mimeData()->hasUrls()) {
            const FolderId target = onFolder ? static_cast<FolderId>(item->data(0, kIdRole).toULongLong())
                                             : m_owner->currentFolder();
            m_owner->dropFiles(IconView::localFiles(event->mimeData()), target);
            event->acceptProposedAction();
        }
    }

    void contextMenuEvent(QContextMenuEvent* event) override
    {
        const QTreeWidgetItem* item = itemAt(event->pos());
        const FolderId folder = (item && item->data(0, kTypeRole).toInt() == kFolderType)
                                    ? static_cast<FolderId>(item->data(0, kIdRole).toULongLong())
                                    : 0;
        m_owner->showMenu(event->globalPos(), IconView::selectedMedia(this), folder);
    }

private:
    MediaBrowser* m_owner;
    QPoint m_pressPos;
};

QList<MediaId> MediaBrowser::IconView::selectedMedia(const QAbstractItemView* view)
{
    QList<MediaId> ids;
    if (const auto* list = qobject_cast<const QListWidget*>(view)) {
        for (const QListWidgetItem* it : list->selectedItems()) {
            if (it->data(kTypeRole).toInt() == kMediaType) ids << it->data(kIdRole).toULongLong();
        }
    } else if (const auto* tree = qobject_cast<const QTreeWidget*>(view)) {
        for (const QTreeWidgetItem* it : tree->selectedItems()) {
            if (it->data(0, kTypeRole).toInt() == kMediaType) ids << it->data(0, kIdRole).toULongLong();
        }
    }
    return ids;
}

// ── MediaBrowser ───────────────────────────────────────────────────────────
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

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText("Filter…");
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_filterText = text.trimmed();
        populate();
    });

    m_sortButton = new QToolButton(this);
    m_sortButton->setText("Sort");
    m_sortButton->setToolTip("Sort by");
    m_sortButton->setAutoRaise(true);
    m_sortButton->setPopupMode(QToolButton::InstantPopup);
    auto* sortMenu = new QMenu(m_sortButton);
    const char* sortNames[] = { "Name", "Duration", "Type" };
    for (int i = 0; i < 3; ++i) {
        QAction* a = sortMenu->addAction(sortNames[i]);
        a->setCheckable(true);
        a->setChecked(i == 0);
        connect(a, &QAction::triggered, this, [this, i, sortMenu] {
            for (QAction* other : sortMenu->actions()) other->setChecked(false);
            sortMenu->actions().at(i)->setChecked(true);
            setSortKey(i);
        });
    }
    m_sortButton->setMenu(sortMenu);

    m_sizeSlider = new QSlider(Qt::Horizontal, this);
    m_sizeSlider->setRange(kMinThumb, kMaxThumb);
    m_sizeSlider->setValue(m_thumbWidth);
    m_sizeSlider->setFixedWidth(96);
    m_sizeSlider->setToolTip("Thumbnail size");
    m_sizeSlider->setStyleSheet(
        "QSlider::groove:horizontal { height: 4px; background: #34363c; border-radius: 2px; }"
        "QSlider::sub-page:horizontal { height: 4px; background: #6f7480; border-radius: 2px; }"
        "QSlider::add-page:horizontal { height: 4px; background: #34363c; border-radius: 2px; }"
        "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0;"
        " border-radius: 6px; background: #d4d6dc; }"
        "QSlider::handle:horizontal:hover { background: #ffffff; }"
        "QSlider::handle:horizontal:pressed { background: #b7b9c0; }");
    connect(m_sizeSlider, &QSlider::valueChanged, this, &MediaBrowser::setThumbSize);

    m_viewButton = new QToolButton(this);
    m_viewButton->setText("List");
    m_viewButton->setToolTip("Toggle icon / list view");
    m_viewButton->setAutoRaise(true);
    connect(m_viewButton, &QToolButton::clicked, this, [this] { setIconMode(!m_iconMode); });

    // Row 1: breadcrumb navigation (its own line, so it can't squish the search).
    auto* navBar = new QHBoxLayout;
    navBar->setContentsMargins(6, 4, 6, 2);
    navBar->setSpacing(4);
    navBar->addWidget(m_upButton);
    navBar->addWidget(m_breadcrumb, 1);

    // Row 2: full-width search/filter.
    auto* searchBar = new QHBoxLayout;
    searchBar->setContentsMargins(6, 0, 6, 4);
    searchBar->addWidget(m_filter);

    m_iconView = new IconView(this);
    m_delegate = new TileDelegate(this);
    m_delegate->setThumbWidth(m_thumbWidth);
    m_iconView->setItemDelegate(m_delegate);
    // Enter vs. rename on double-click is decided by region in IconView itself.
    connect(m_iconView, &QListWidget::itemSelectionChanged, this, [this] {
        const auto ids = IconView::selectedMedia(m_iconView);
        reportSelection(ids.isEmpty() ? kInvalidMedia : ids.front());
    });
    connect(m_iconView, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (item->data(kTypeRole).toInt() == kFolderType)
            commitRename(static_cast<FolderId>(item->data(kIdRole).toULongLong()), item->text());
    });

    m_listView = new ListView(this);
    connect(m_listView, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item && item->data(0, kTypeRole).toInt() == kFolderType)
            enterFolder(static_cast<FolderId>(item->data(0, kIdRole).toULongLong()));
    });
    connect(m_listView, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto ids = IconView::selectedMedia(m_listView);
        reportSelection(ids.isEmpty() ? kInvalidMedia : ids.front());
    });
    connect(m_listView, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (column == kColName && item->data(0, kTypeRole).toInt() == kFolderType)
            commitRename(static_cast<FolderId>(item->data(0, kIdRole).toULongLong()), item->text(kColName));
    });

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_iconView);
    m_stack->addWidget(m_listView);

    // Bottom action bar — Premiere-style: view controls on the left, New Bin on
    // the right.
    auto* newBin = new QToolButton(this);
    newBin->setIcon(QIcon(folderPixmap(16)));
    newBin->setToolTip("New Bin");
    newBin->setAutoRaise(true);
    connect(newBin, &QToolButton::clicked, this, [this] { emit newFolderRequested(m_current); });

    auto* bottomBar = new QHBoxLayout;
    bottomBar->setContentsMargins(6, 3, 6, 3);
    bottomBar->setSpacing(6);
    bottomBar->addWidget(m_viewButton);
    bottomBar->addWidget(m_sortButton);
    bottomBar->addWidget(m_sizeSlider);
    bottomBar->addStretch(1);
    bottomBar->addWidget(newBin);

    auto* topLine = new QFrame(this);
    topLine->setFrameShape(QFrame::HLine);
    topLine->setStyleSheet("color: #3a3c42;");
    auto* bottomLine = new QFrame(this);
    bottomLine->setFrameShape(QFrame::HLine);
    bottomLine->setStyleSheet("color: #3a3c42;");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(navBar);
    layout->addLayout(searchBar);
    layout->addWidget(topLine);
    layout->addWidget(m_stack);
    layout->addWidget(bottomLine);
    layout->addLayout(bottomBar);

    // Restore view prefs.
    QSettings settings;
    settings.beginGroup("mediaBrowser");
    m_thumbWidth = std::clamp(settings.value("thumbWidth", m_thumbWidth).toInt(), kMinThumb, kMaxThumb);
    m_sortKey = std::clamp(settings.value("sortKey", 0).toInt(), 0, 2);
    m_iconMode = settings.value("iconMode", true).toBool();
    settings.endGroup();

    m_sizeSlider->setValue(m_thumbWidth);
    m_delegate->setThumbWidth(m_thumbWidth);
    setIconMode(m_iconMode);
}

MediaBrowser::~MediaBrowser()
{
    QSettings settings;
    settings.beginGroup("mediaBrowser");
    settings.setValue("thumbWidth", m_thumbWidth);
    settings.setValue("sortKey", m_sortKey);
    settings.setValue("iconMode", m_iconMode);
    settings.endGroup();
}

void MediaBrowser::setProject(const Project* project)
{
    m_project = project;
    m_current = kRootFolder;
    refresh();
}

void MediaBrowser::setIconMode(bool icon)
{
    m_iconMode = icon;
    m_stack->setCurrentWidget(icon ? static_cast<QWidget*>(m_iconView) : m_listView);
    m_viewButton->setText(icon ? "List" : "Icons");
    m_sizeSlider->setVisible(icon);
    m_sortButton->setVisible(icon);  // list view sorts via its headers
    refresh();
}

void MediaBrowser::setThumbSize(int width)
{
    m_thumbWidth = std::clamp(width, kMinThumb, kMaxThumb);
    m_delegate->setThumbWidth(m_thumbWidth);
    m_iconView->doItemsLayout();  // re-query sizeHint (uniformItemSizes caches it)
    m_iconView->viewport()->update();
}

void MediaBrowser::setSortKey(int key)
{
    m_sortKey = key;
    populate();
}

void MediaBrowser::enterFolder(FolderId folder)
{
    m_current = folder;
    m_filter->clear();  // fresh bin
    refresh();
}

void MediaBrowser::goUp()
{
    if (m_current == kRootFolder || !m_project) return;
    for (const BinFolder& folder : m_project->folders()) {
        if (folder.id == m_current) {
            m_current = folder.parent;
            break;
        }
    }
    m_filter->clear();
    refresh();
}

void MediaBrowser::reportSelection(MediaId primary) { emit mediaSelected(primary); }

void MediaBrowser::beginRenameFolder(FolderId folder)
{
    if (m_iconMode) {
        for (int i = 0; i < m_iconView->count(); ++i) {
            QListWidgetItem* item = m_iconView->item(i);
            if (item->data(kTypeRole).toInt() == kFolderType
                && item->data(kIdRole).toULongLong() == folder) {
                m_iconView->setCurrentItem(item);
                m_iconView->editItem(item);
                return;
            }
        }
    } else {
        for (int i = 0; i < m_listView->topLevelItemCount(); ++i) {
            QTreeWidgetItem* item = m_listView->topLevelItem(i);
            if (item->data(0, kTypeRole).toInt() == kFolderType
                && item->data(0, kIdRole).toULongLong() == folder) {
                m_listView->setCurrentItem(item);
                m_listView->editItem(item, kColName);
                return;
            }
        }
    }
}

void MediaBrowser::commitRename(FolderId folder, const QString& name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        refresh();  // reject empty; revert the label to the model's name
        return;
    }
    emit folderRenamed(folder, trimmed);
}

void MediaBrowser::startMediaDrag(QAbstractItemView* view, const QList<MediaId>& ids, MediaId primary,
                                  const QPixmap& pixmap)
{
    QByteArray list;
    for (MediaId id : ids) {
        if (!list.isEmpty()) list += ',';
        list += QByteArray::number(static_cast<qulonglong>(id));
    }
    auto* mime = new QMimeData;
    mime->setData(kMediaMimeType, QByteArray::number(static_cast<qulonglong>(primary)));
    mime->setData(kMediaListMimeType, list);

    auto* drag = new QDrag(view);
    drag->setMimeData(mime);
    if (!pixmap.isNull()) {
        drag->setPixmap(pixmap);
        drag->setHotSpot(QPoint(pixmap.width() / 2, pixmap.height() / 2));
    }
    drag->exec(Qt::CopyAction);
}

void MediaBrowser::dropOnFolder(const QList<MediaId>& ids, FolderId folder)
{
    if (!ids.isEmpty()) emit mediaMovedToFolder(ids, folder);
}

void MediaBrowser::dropFiles(const QStringList& paths, FolderId folder)
{
    if (!paths.isEmpty()) emit filesImported(paths, folder);
}

void MediaBrowser::showMenu(const QPoint& globalPos, const QList<MediaId>& media, FolderId folderUnderCursor)
{
    QMenu menu(this);
    QAction* newFolder = menu.addAction("New Folder");
    QAction* import = menu.addAction("Import Media…");

    // Label targets the media selection, or (if none) the folder under the cursor.
    const bool labelMedia = !media.isEmpty();
    const bool labelFolder = !labelMedia && folderUnderCursor != 0;

    QList<QAction*> labelActions;
    if (labelMedia || labelFolder) {
        menu.addSeparator();
        QMenu* labelMenu = menu.addMenu("Label");
        QAction* none = labelMenu->addAction("None");
        none->setData(0);
        labelActions << none;
        for (int i = 1; i <= labelCount(); ++i) {
            QAction* a = labelMenu->addAction(QIcon(labelSwatch(i, 14)), labelName(i));
            a->setData(i);
            labelActions << a;
        }
    }

    QAction* del = labelMedia ? menu.addAction(media.size() > 1 ? "Delete Items" : "Delete") : nullptr;

    QAction* rename = nullptr;
    QAction* delFolder = nullptr;
    if (folderUnderCursor != 0) {
        menu.addSeparator();
        rename = menu.addAction("Rename");
        delFolder = menu.addAction("Delete Folder");
    }

    QAction* chosen = menu.exec(globalPos);
    if (!chosen) return;
    if (chosen == newFolder) {
        emit newFolderRequested(m_current);
    } else if (chosen == import) {
        emit importRequested(m_current);
    } else if (del && chosen == del) {
        emit deleteMediaRequested(media);
    } else if (rename && chosen == rename) {
        beginRenameFolder(folderUnderCursor);
    } else if (delFolder && chosen == delFolder) {
        emit deleteFolderRequested(folderUnderCursor);
    } else if (labelActions.contains(chosen)) {
        const int lbl = chosen->data().toInt();
        if (labelMedia) {
            emit mediaLabelChanged(media, lbl);
        } else {
            emit folderLabelChanged(folderUnderCursor, lbl);
        }
    }
}

void MediaBrowser::refresh()
{
    populate();
    updateBreadcrumb();
}

void MediaBrowser::populate()
{
    // Rebuilding sets item text/data, which would fire itemChanged and be mistaken
    // for a rename; silence both views while we repopulate.
    const QSignalBlocker blockIcon(m_iconView);
    const QSignalBlocker blockList(m_listView);

    m_iconView->clear();
    m_listView->clear();
    if (!m_project) return;

    const bool filtering = !m_filterText.isEmpty();
    const auto matches = [&](const QString& name) {
        return !filtering || name.contains(m_filterText, Qt::CaseInsensitive);
    };

    // Folders first, sorted by name.
    std::vector<const BinFolder*> folders;
    for (const BinFolder& folder : m_project->folders()) {
        if (folder.parent == m_current && folder.id != kRootFolder
            && matches(QString::fromStdString(folder.name))) {
            folders.push_back(&folder);
        }
    }
    std::sort(folders.begin(), folders.end(),
              [](const BinFolder* a, const BinFolder* b) { return a->name < b->name; });

    // Media in this bin, filtered and sorted by the current key.
    std::vector<const MediaSource*> media;
    for (const MediaSource& m : m_project->mediaPool()) {
        if (m.folder == m_current
            && matches(QFileInfo(QString::fromStdString(m.path)).fileName())) {
            media.push_back(&m);
        }
    }
    std::sort(media.begin(), media.end(), [this](const MediaSource* a, const MediaSource* b) {
        switch (m_sortKey) {
        case 1:
            return a->duration < b->duration;
        case 2:
            return typeText(a->hasVideo, a->hasAudio) < typeText(b->hasVideo, b->hasAudio);
        default:
            return QFileInfo(QString::fromStdString(a->path)).fileName().toLower()
                 < QFileInfo(QString::fromStdString(b->path)).fileName().toLower();
        }
    });

    for (const BinFolder* folder : folders) addFolderRow(*folder);
    for (const MediaSource* m : media) addMediaRow(*m);
}

void MediaBrowser::addFolderRow(const BinFolder& folder)
{
    const QString name = QString::fromStdString(folder.name);
    const auto id = static_cast<qulonglong>(folder.id);

    const QColor color = folder.label > 0 ? labelColor(folder.label) : kFolderGray;

    auto* icon = new QListWidgetItem(name, m_iconView);
    icon->setData(kTypeRole, kFolderType);
    icon->setData(kIdRole, id);
    icon->setData(kLabelRole, folder.label);
    icon->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled
                   | Qt::ItemIsEditable);

    auto* row = new BinItem(m_listView);
    row->setData(0, kTypeRole, kFolderType);
    row->setData(0, kIdRole, id);
    row->setIcon(kColName, QIcon(folderPixmap(16, color)));
    row->setText(kColName, name);
    row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDropEnabled
                  | Qt::ItemIsEditable);
}

void MediaBrowser::addMediaRow(const MediaSource& m)
{
    const QString name = QFileInfo(QString::fromStdString(m.path)).fileName();
    const auto id = static_cast<qulonglong>(m.id);
    const double fps = m.rateDen > 0 ? static_cast<double>(m.rateNum) / m.rateDen : 0.0;

    auto* icon = new QListWidgetItem(name, m_iconView);
    icon->setData(kTypeRole, kMediaType);
    icon->setData(kIdRole, id);
    icon->setData(kDurationRole, static_cast<qlonglong>(m.duration));
    icon->setData(kVideoRole, m.hasVideo);
    icon->setData(kAudioRole, m.hasAudio);
    icon->setData(kLabelRole, m.label);
    icon->setData(kFpsRole, fps);
    icon->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);

    auto* row = new BinItem(m_listView);
    row->setData(0, kTypeRole, kMediaType);
    row->setData(0, kIdRole, id);
    if (m.label > 0) row->setIcon(kColName, QIcon(labelSwatch(m.label, 12)));
    row->setText(kColName, name);
    row->setText(kColType, typeText(m.hasVideo, m.hasAudio));
    row->setText(kColDuration, shortDuration(m.duration));
    row->setData(kColDuration, Qt::UserRole, secondsFromTicks(m.duration));
    row->setText(kColFps, fps > 0 ? QString::number(fps, 'f', 2) : QString("—"));
    row->setData(kColFps, Qt::UserRole, fps);
    row->setText(kColPath, QString::fromStdString(m.path));
    row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
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

}  // namespace hopline
