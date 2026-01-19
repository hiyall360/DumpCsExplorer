#include "MainWindow.h"
#include "parser/DumpCsParser.h"

#include <QStackedWidget>
#include <QTreeView>
#include <QPlainTextEdit>
#include <QStandardItemModel>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QStyle>
#include <QLineEdit>
#include <QClipboard>
#include <QApplication>
#include <QMenu>
#include <QStatusBar>
#include <QProgressBar>
#include <QListWidget>
#include <QCheckBox>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QFrame>
#include <QPixmap>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDialog>
#include <QTableView>
#include <QHeaderView>
#include <QHash>
#include <QtCore/qglobal.h>
#include <unordered_set>
#include <QBrush>
#include <QColor>
#include <QStyledItemDelegate>
#include <QRegularExpression>
#include <QFontMetricsF>
#include <QPainter>
#include <QPointF>
#include <QVariant>
#include <QMetaType>
#include <QtCore/qvariant.h>

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include <map>
#include <string>

static bool groupMatches(MemberKind mk, const QString& groupKey) {
    if (groupKey == "method")    return mk == MemberKind::Method;
    if (groupKey == "ctor")      return mk == MemberKind::Ctor;
    if (groupKey == "field")     return mk == MemberKind::Field;
    if (groupKey == "property")  return mk == MemberKind::Property;
    if (groupKey == "event")     return mk == MemberKind::Event;
    if (groupKey == "enum")      return mk == MemberKind::EnumValue;
    return false;
}

static QString normalizeSignature(const DumpMember& m) {
    QString s = QString::fromStdString(m.signature).trimmed();

    static const QRegularExpression rxMods(
        R"(^(?:(?:public|private|protected|internal|static|virtual|override|abstract|sealed|extern|readonly|const|volatile|unsafe|new|partial|async|ref|out|in)\s+)+)"
    );
    s.replace(rxMods, "");

    static const QRegularExpression rxGeneric(R"(<[^>]*>)");
    s.replace(rxGeneric, "<>");

    if (m.kind == MemberKind::Method || m.kind == MemberKind::Ctor || m.kind == MemberKind::Property || m.kind == MemberKind::Event) {
        const int firstSpace = s.indexOf(' ');
        if (firstSpace > 0)
            s = s.mid(firstSpace + 1).trimmed();
        if (!s.endsWith(')'))
            s += "()";
        return s;
    }

    if (m.kind == MemberKind::EnumValue)
        return QString::fromStdString(m.name);

    static const QRegularExpression rxFieldName(R"(\b([A-Za-z_][A-Za-z0-9_]*)\b\s*(?:;|=))");
    const auto mm = rxFieldName.match(s);
    if (mm.hasMatch())
        return mm.captured(1);

    return s;
}

static QString kindToString(MemberKind k) {
    switch (k) {
    case MemberKind::Method: return "Method";
    case MemberKind::Ctor: return "Ctor";
    case MemberKind::Field: return "Field";
    case MemberKind::Property: return "Property";
    case MemberKind::Event: return "Event";
    case MemberKind::EnumValue: return "Enum";
    default: return "Member";
    }
}

class SignatureHighlightDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (!painter) return;

        if (option.state & QStyle::State_Selected) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        const QString text = opt.text;
        opt.text.clear();

        const QWidget* w = opt.widget;
        QStyle* style = w ? w->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, w);

        const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, w);
        if (textRect.isEmpty())
            return;

        QColor base = opt.palette.color(QPalette::Text);
        const QVariant roleColor = index.data(Qt::UserRole + 200);
        if (roleColor.isValid()) {
            const QColor c(roleColor.toString());
            if (c.isValid())
                base = c;
        }

        const QColor kwMod(255, 180, 90);
        const QColor kwType(120, 200, 255);
        const QColor kwLit(190, 150, 255);
        const QColor kwNum(130, 220, 160);

        static const std::unordered_set<std::string> modifiers = {
            "public","private","protected","internal","static","virtual","override","abstract",
            "sealed","extern","readonly","const","volatile","unsafe","new","partial","async",
            "ref","out","in"
        };
        static const std::unordered_set<std::string> primTypes = {
            "void","bool","byte","sbyte","short","ushort","int","uint","long","ulong",
            "float","double","decimal","char","string","object","nint","nuint"
        };
        static const std::unordered_set<std::string> literals = {
            "true","false","null"
        };

        static const QRegularExpression rx(
            R"((0x[0-9A-Fa-f]+|\b\d+(?:\.\d+)?\b|\b[A-Za-z_][A-Za-z0-9_]*\b|::|\.\.\.|\S))");

        QFont f = opt.font;
        QFontMetricsF fm(f);

        painter->save();
        painter->setFont(f);
        painter->setClipRect(textRect);

        qreal x = (qreal)textRect.left();
        const qreal y = (qreal)textRect.top() + fm.ascent() + (textRect.height() - fm.height()) / 2.0;

        auto drawSeg = [&](const QString& seg, const QColor& c) {
            if (seg.isEmpty()) return true;
            const qreal wSeg = fm.horizontalAdvance(seg);
            if (x + wSeg > (qreal)textRect.right()) {
                painter->setPen(base);
                painter->drawText(QPointF(x, y), "...");
                return false;
            }
            painter->setPen(c);
            painter->drawText(QPointF(x, y), seg);
            x += wSeg;
            return true;
        };

        int last = 0;
        QRegularExpressionMatchIterator it = rx.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int start = m.capturedStart(1);
            const int len = m.capturedLength(1);
            if (start < 0 || len <= 0) continue;
            if (start > last) {
                if (!drawSeg(text.mid(last, start - last), base))
                    break;
            }

            const QString tok = m.captured(1);
            QColor c = base;
            const QString low = tok.toLower();
            const std::string lowStd = low.toStdString();

            if (tok.startsWith("0x") || tok[0].isDigit()) c = kwNum;
            else if (modifiers.contains(lowStd)) c = kwMod;
            else if (primTypes.contains(lowStd)) c = kwType;
            else if (literals.contains(lowStd)) c = kwLit;

            if (!drawSeg(tok, c))
                break;

            last = start + len;
        }

        if (last < text.size())
            drawSeg(text.mid(last), base);

        painter->restore();
    }
};

void MainWindow::buildSearchIndex() {
    searchIndex_.clear();

    for (const auto& [ns, _] : nsItems_) {
        SearchEntry e;
        e.kind = SearchEntry::Kind::Namespace;
        e.ns = QString::fromStdString(ns);
        e.display = e.ns;
        e.detail = e.ns;
        searchIndex_.push_back(e);
    }

    for (int ti = 0; ti < (int)types_.size(); ++ti) {
        const auto& t = types_[(size_t)ti];
        SearchEntry e;
        e.kind = SearchEntry::Kind::Type;
        e.ns = QString::fromStdString(t.nameSpace);
        e.typeIndex = ti;
        e.display = QString::fromStdString(t.nameSpace + "::" + t.name);
        e.detail = e.display;
        searchIndex_.push_back(e);

        for (int mi = 0; mi < (int)t.members.size(); ++mi) {
            const auto& m = t.members[(size_t)mi];
            SearchEntry em;
            em.kind = SearchEntry::Kind::Member;
            em.ns = QString::fromStdString(t.nameSpace);
            em.typeIndex = ti;
            em.memberIndex = mi;
            em.memberKind = m.kind;
            em.display = QString::fromStdString(t.nameSpace + "::" + t.name + "  " + m.signature);
            em.detail = QString::fromStdString(m.details);
            searchIndex_.push_back(em);
        }
    }
}

void MainWindow::updateSearchResults() {
    if (!resultsList_ || !resultsSearch_)
        return;

    const QString q = resultsSearch_->text().trimmed().toLower();

    const bool allowNs = filterNs_ && filterNs_->isChecked();
    const bool allowType = filterType_ && filterType_->isChecked();
    const bool allowMethod = filterMethod_ && filterMethod_->isChecked();
    const bool allowCtor = filterCtor_ && filterCtor_->isChecked();
    const bool allowField = filterField_ && filterField_->isChecked();
    const bool allowProperty = filterProperty_ && filterProperty_->isChecked();
    const bool allowEvent = filterEvent_ && filterEvent_->isChecked();
    const bool allowEnumValue = filterEnumValue_ && filterEnumValue_->isChecked();

    resultsList_->setUpdatesEnabled(false);
    resultsList_->clear();

    int shown = 0;
    for (int i = 0; i < (int)searchIndex_.size(); ++i) {
        const auto& e = searchIndex_[(size_t)i];

        bool kindOk = false;
        if (e.kind == SearchEntry::Kind::Namespace) kindOk = allowNs;
        else if (e.kind == SearchEntry::Kind::Type) kindOk = allowType;
        else {
            switch (e.memberKind) {
            case MemberKind::Method: kindOk = allowMethod; break;
            case MemberKind::Ctor: kindOk = allowCtor; break;
            case MemberKind::Field: kindOk = allowField; break;
            case MemberKind::Property: kindOk = allowProperty; break;
            case MemberKind::Event: kindOk = allowEvent; break;
            case MemberKind::EnumValue: kindOk = allowEnumValue; break;
            }
        }
        if (!kindOk)
            continue;

        if (!q.isEmpty()) {
            const QString hay = (e.display + "\n" + e.detail).toLower();
            if (!hay.contains(q))
                continue;
        }

        auto* it = new QListWidgetItem(e.display, resultsList_);
        it->setData(Qt::UserRole + 1, i);
        ++shown;
    }

    resultsList_->setUpdatesEnabled(true);
    if (resultsCount_)
        resultsCount_->setText(QString::number(shown));
}

void MainWindow::navigateToSearchResult(QListWidgetItem* item) {
    if (!item)
        return;
    const int idx = item->data(Qt::UserRole + 1).toInt();
    if (idx < 0 || idx >= (int)searchIndex_.size())
        return;

    const auto& e = searchIndex_[(size_t)idx];

    auto expandProxyAncestors = [this](QModelIndex pidx) {
        for (QModelIndex cur = pidx; cur.isValid(); cur = cur.parent())
            tree_->expand(cur);
    };

    QStandardItem* target = nullptr;
    if (e.kind == SearchEntry::Kind::Namespace) {
        auto it = nsItems_.find(e.ns.toStdString());
        if (it != nsItems_.end())
            target = it->second;
    } else if (e.kind == SearchEntry::Kind::Type) {
        if (e.typeIndex >= 0 && (size_t)e.typeIndex < typeItems_.size())
            target = typeItems_[(size_t)e.typeIndex];
    } else {
        if (e.typeIndex < 0 || (size_t)e.typeIndex >= typeItems_.size())
            return;

        auto* typeItem = typeItems_[(size_t)e.typeIndex];
        if (!typeItem)
            return;

        onTreeExpanded(typeItem->index());

        const QString groupKey =
            (e.memberKind == MemberKind::Ctor)     ? "ctor" :
            (e.memberKind == MemberKind::Method)   ? "method" :
            (e.memberKind == MemberKind::Field)    ? "field" :
            (e.memberKind == MemberKind::Property) ? "property" :
            (e.memberKind == MemberKind::Event)    ? "event" :
            (e.memberKind == MemberKind::EnumValue)? "enum" :
            "";

        QStandardItem* groupItem = nullptr;
        for (int r = 0; r < typeItem->rowCount(); ++r) {
            auto* ch = typeItem->child(r);
            if (!ch) continue;
            if (ch->data(Qt::UserRole + 3).toString() == groupKey) {
                groupItem = ch;
                break;
            }
        }
        if (!groupItem)
            return;

        for (int r = 0; r < groupItem->rowCount(); ++r) {
            auto* ch = groupItem->child(r);
            if (!ch) continue;
            if (ch->data(Qt::UserRole + 10).toInt() == e.memberIndex) {
                target = ch;
                break;
            }
        }

        if (!target && e.memberIndex >= 0 && e.memberIndex < (int)types_[(size_t)e.typeIndex].members.size()) {
            const auto& m = types_[(size_t)e.typeIndex].members[(size_t)e.memberIndex];

            const QIcon childIcon =
                (m.kind == MemberKind::Ctor)     ? icoCtor_ :
                (m.kind == MemberKind::Method)   ? icoMethod_ :
                (m.kind == MemberKind::Field)    ? icoField_ :
                (m.kind == MemberKind::Property) ? icoProperty_ :
                (m.kind == MemberKind::Event)    ? icoEvent_ :
                (m.kind == MemberKind::EnumValue)? icoEnumValue_ :
                icoClass_;

            if (groupItem->rowCount() == 1) {
                auto* only = groupItem->child(0);
                if (only && only->text() == "(none)")
                    groupItem->removeRow(0);
            }

            const QString display = QString::fromStdString(m.signature);
            const QString detail  = QString::fromStdString(m.details);

            const QString sig = QString::fromStdString(m.signature);
            const QString rva = QString("0x%1").arg(QString::number((qulonglong)m.rva, 16));
            const QString off = QString("0x%1").arg(QString::number((qulonglong)m.offset, 16));
            const QString va  = QString("0x%1").arg(QString::number((qulonglong)m.va, 16));

            auto* it = new QStandardItem(display);
            it->setIcon(childIcon);
            it->setData(detail, Qt::UserRole + 100);
            it->setData(sig,    Qt::UserRole + 101);
            it->setData(off,    Qt::UserRole + 102);
            it->setData(va,     Qt::UserRole + 103);
            it->setData(rva,    Qt::UserRole + 104);
            it->setData(e.memberIndex, Qt::UserRole + 10);

            groupItem->appendRow(it);
            target = it;
        }
    }

    if (!target)
        return;

    const QModelIndex srcIdx = target->index();
    const QModelIndex proxyIdx = proxy_->mapFromSource(srcIdx);
    if (!proxyIdx.isValid())
        return;

    expandProxyAncestors(proxyIdx.parent());
    tree_->setCurrentIndex(proxyIdx);
    tree_->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
}

class TreeFilterProxy final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override {
        if (QSortFilterProxyModel::filterAcceptsRow(source_row, source_parent))
            return true;

        QModelIndex idx = sourceModel()->index(source_row, filterKeyColumn(), source_parent);
        const int childCount = sourceModel()->rowCount(idx);
        for (int i = 0; i < childCount; ++i) {
            if (filterAcceptsRow(i, idx))
                return true;
        }
        return false;
    }
};

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setWindowTitle("DumpCsExplorer");
    resize(1200, 760);
    statusBar()->showMessage("Ready");
    stack_->setCurrentWidget(welcomePage_);

    watcher_ = new QFutureWatcher<std::vector<DumpType>>(this);
    connect(watcher_, &QFutureWatcher<std::vector<DumpType>>::finished,
            this, &MainWindow::finishParseAsync);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    stack_ = new QStackedWidget(this);
    welcomePage_  = buildWelcomePage();
    explorerPage_ = buildExplorerPage();

    stack_->addWidget(welcomePage_);
    stack_->addWidget(explorerPage_);
    setCentralWidget(stack_);

    icoFolder_     = style()->standardIcon(QStyle::SP_DirIcon);
    icoNamespace_  = icoFolder_;
    icoClass_      = icoFolder_;
    icoEnumType_   = style()->standardIcon(QStyle::SP_FileDialogDetailedView);

    icoMethod_     = style()->standardIcon(QStyle::SP_CommandLink);
    icoCtor_       = style()->standardIcon(QStyle::SP_DialogYesButton);
    icoField_      = style()->standardIcon(QStyle::SP_DialogSaveButton);
    icoProperty_   = style()->standardIcon(QStyle::SP_DialogApplyButton);
    icoEvent_      = style()->standardIcon(QStyle::SP_BrowserReload);
    icoEnumValue_  = style()->standardIcon(QStyle::SP_FileDialogInfoView);

    const QString envLogo = qEnvironmentVariable("DUMPCSEXPLORER_LOGO");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList logoCandidates = {
        envLogo,
        QDir(appDir).filePath("Icon.png"),
        QDir(appDir).filePath("Icon.ico")
    };
    for (const auto& p : logoCandidates) {
        if (!p.isEmpty() && QFileInfo::exists(p)) {
            setWindowIcon(QIcon(p));
            break;
        }
    }

    static const QString qss =
        "QMainWindow, QWidget { background-color: #061b1c; color: #d7f6f1; }\n"
        "QFrame#Card { background-color: #0b2a2d; border: 1px solid #124448; border-radius: 8px; }\n"
        "QFrame#HeaderBar { background-color: #052427; border-bottom: 1px solid #124448; }\n"
        "QLabel#BrandText { color: #22ff66; font-weight: 700; letter-spacing: 0.5px; }\n"
        "QLineEdit, QPlainTextEdit, QListWidget, QTreeView {"
        "  background-color: #0b2a2d; border: 1px solid #124448; border-radius: 6px;"
        "  selection-background-color: #1b7a3b; selection-color: #eafff6;"
        "}\n"
        "QLineEdit { padding: 6px 8px; }\n"
        "QPlainTextEdit { padding: 8px; }\n"
        "QPushButton { background-color: #134046; border: 1px solid #1a5860; border-radius: 6px; padding: 8px 12px; }\n"
        "QPushButton:hover { background-color: #1a5560; }\n"
        "QPushButton:pressed { background-color: #1b7a3b; border-color: #1b7a3b; }\n"
        "QCheckBox { spacing: 6px; }\n"
        "QCheckBox::indicator { width: 14px; height: 14px; }\n"
        "QCheckBox::indicator:unchecked { border: 1px solid #1a5860; background: #0b2a2d; border-radius: 3px; }\n"
        "QCheckBox::indicator:checked { border: 1px solid #22ff66; background: #1b7a3b; border-radius: 3px; }\n"
        "QProgressBar { border: 1px solid #124448; background: #0b2a2d; border-radius: 6px; height: 14px; }\n"
        "QProgressBar::chunk { background: #22ff66; border-radius: 6px; }\n"
        "QSplitter::handle { background-color: #124448; }\n"
        "QScrollBar:vertical { background: #061b1c; width: 10px; margin: 0px; }\n"
        "QScrollBar::handle:vertical { background: #124448; border-radius: 4px; min-height: 20px; }\n"
        "QScrollBar::handle:vertical:hover { background: #1a5860; }\n"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }\n"
        "QScrollBar:horizontal { background: #061b1c; height: 10px; margin: 0px; }\n"
        "QScrollBar::handle:horizontal { background: #124448; border-radius: 4px; min-width: 20px; }\n"
        "QScrollBar::handle:horizontal:hover { background: #1a5860; }\n"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }\n";
    qApp->setStyleSheet(qss);
}

QWidget* MainWindow::buildHeaderBar(QWidget* parent, bool showOpenButton) {
    auto* bar = new QFrame(parent);
    bar->setObjectName("HeaderBar");
    bar->setFixedHeight(54);

    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(12, 8, 12, 8);
    lay->setSpacing(10);

    auto* logo = new QLabel(bar);
    logo->setFixedSize(34, 34);
    logo->setScaledContents(true);

    QPixmap px;
    const QString envLogo = qEnvironmentVariable("DUMPCSEXPLORER_LOGO");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList logoCandidates = {
        envLogo,
        QDir(appDir).filePath("Icon.png"),
        QDir(appDir).filePath("Icon.ico")
    };
    for (const auto& p : logoCandidates) {
        if (!p.isEmpty() && QFileInfo::exists(p) && px.load(p))
            break;
    }
    if (!px.isNull())
        logo->setPixmap(px.scaled(34, 34, Qt::KeepAspectRatio, Qt::SmoothTransformation));

    auto* brand = new QLabel("DumpCsExplorer", bar);
    brand->setObjectName("BrandText");
    QFont bf = brand->font();
    bf.setPointSize(13);
    bf.setBold(true);
    brand->setFont(bf);

    lay->addWidget(logo);
    lay->addWidget(brand);
    lay->addStretch(1);

    if (showOpenButton) {
        auto* open = new QPushButton("Open dump.cs", bar);
        open->setFixedHeight(34);
        connect(open, &QPushButton::clicked, this, &MainWindow::openDumpCs);
        lay->addWidget(open);

        compareBtn_ = new QPushButton("Compare...", bar);
        compareBtn_->setFixedHeight(34);
        compareBtn_->setVisible(hasLoadedPrimary_);
        compareBtn_->setEnabled(hasLoadedPrimary_);
        connect(compareBtn_, &QPushButton::clicked, this, &MainWindow::compareDumpCs);
        lay->addWidget(compareBtn_);
    }

    return bar;
}

QWidget* MainWindow::buildWelcomePage() {
    auto* w = new QWidget(this);
    auto* lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    lay->addWidget(buildHeaderBar(w, false));
    auto* body = new QWidget(w);
    auto* bodyLay = new QVBoxLayout(body);
    bodyLay->setContentsMargins(12, 12, 12, 12);
    bodyLay->setSpacing(12);
    lay->addWidget(body, 1);

    bodyLay->addStretch();

    auto* sub = new QLabel("Select a dump.cs file to begin.", body);
    QFont f = sub->font();
    f.setPointSize(14);
    f.setBold(true);
    sub->setFont(f);
    sub->setAlignment(Qt::AlignCenter);
    bodyLay->addWidget(sub);

    bodyLay->addSpacing(24);

    openBtn_ = new QPushButton("Open dump.cs", body);
    openBtn_->setFixedHeight(42);
    openBtn_->setMaximumWidth(260);
    openBtn_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* row = new QHBoxLayout();
    row->addStretch();
    row->addWidget(openBtn_);
    row->addStretch();
    bodyLay->addLayout(row);

    bodyLay->addStretch();
    connect(openBtn_, &QPushButton::clicked, this, &MainWindow::openDumpCs);
    return w;
}

QWidget* MainWindow::buildExplorerPage() {
    auto* page = new QWidget(this);
    auto* mainLay = new QVBoxLayout(page);
    mainLay->setContentsMargins(0, 0, 0, 0);
    mainLay->setSpacing(8);

    mainLay->addWidget(buildHeaderBar(page, true));

    auto* content = new QWidget(page);
    auto* contentLay = new QVBoxLayout(content);
    contentLay->setContentsMargins(12, 10, 12, 12);
    contentLay->setSpacing(8);
    mainLay->addWidget(content, 1);

    busyRow_ = new QWidget(content);
    auto* busyLay = new QHBoxLayout(busyRow_);
    busyLay->setContentsMargins(0, 0, 0, 0);

    busyLabel_ = new QLabel("Parsing…", busyRow_);
    busyBar_ = new QProgressBar(busyRow_);
    busyBar_->setRange(0, 100);
    busyBar_->setValue(0);

    busyLay->addWidget(busyLabel_);
    busyLay->addWidget(busyBar_, 1);
    busyRow_->setVisible(false);

    contentLay->addWidget(busyRow_);

    search_ = new QLineEdit(content);
    search_->setPlaceholderText("Search (types, methods, fields, properties, enum values)...");
    contentLay->addWidget(search_);

    auto* splitter = new QSplitter(content);

    auto* treeCard = new QFrame(splitter);
    treeCard->setObjectName("Card");
    auto* treeLay = new QVBoxLayout(treeCard);
    treeLay->setContentsMargins(8, 8, 8, 8);
    treeLay->setSpacing(6);

    tree_ = new QTreeView(treeCard);
    treeLay->addWidget(tree_, 1);

    auto* rightSplitter = new QSplitter(Qt::Vertical, splitter);

    auto* detailsCard = new QFrame(rightSplitter);
    detailsCard->setObjectName("Card");
    auto* detailsLay = new QVBoxLayout(detailsCard);
    detailsLay->setContentsMargins(8, 8, 8, 8);
    detailsLay->setSpacing(6);

    details_ = new QPlainTextEdit(detailsCard);
    details_->setReadOnly(true);
    detailsLay->addWidget(details_, 1);

    auto* resultsCard = new QFrame(rightSplitter);
    resultsCard->setObjectName("Card");
    auto* resultsLay = new QVBoxLayout(resultsCard);
    resultsLay->setContentsMargins(8, 8, 8, 8);
    resultsLay->setSpacing(6);

    auto* resultsHeader = new QHBoxLayout();
    resultsHeader->setContentsMargins(0, 0, 0, 0);
    resultsHeader->setSpacing(6);

    auto* resultsTitle = new QLabel("Results", resultsCard);
    resultsCount_ = new QLabel("0", resultsCard);
    resultsHeader->addWidget(resultsTitle);
    resultsHeader->addStretch(1);
    resultsHeader->addWidget(resultsCount_);
    resultsLay->addLayout(resultsHeader);

    resultsSearch_ = new QLineEdit(resultsCard);
    resultsSearch_->setPlaceholderText("Filter results...");
    resultsLay->addWidget(resultsSearch_);

    auto* filtersGrid = new QGridLayout();
    filtersGrid->setContentsMargins(0, 0, 0, 0);
    filtersGrid->setHorizontalSpacing(10);
    filtersGrid->setVerticalSpacing(4);

    filterNs_ = new QCheckBox("Namespace", resultsCard);
    filterType_ = new QCheckBox("Type", resultsCard);
    filterMethod_ = new QCheckBox("Method", resultsCard);
    filterCtor_ = new QCheckBox("Ctor", resultsCard);
    filterField_ = new QCheckBox("Field", resultsCard);
    filterProperty_ = new QCheckBox("Property", resultsCard);
    filterEvent_ = new QCheckBox("Event", resultsCard);
    filterEnumValue_ = new QCheckBox("Enum", resultsCard);

    filterNs_->setChecked(true);
    filterType_->setChecked(true);
    filterMethod_->setChecked(true);
    filterCtor_->setChecked(true);
    filterField_->setChecked(true);
    filterProperty_->setChecked(true);
    filterEvent_->setChecked(true);
    filterEnumValue_->setChecked(true);

    filtersGrid->addWidget(filterNs_, 0, 0);
    filtersGrid->addWidget(filterType_, 0, 1);
    filtersGrid->addWidget(filterMethod_, 0, 2);
    filtersGrid->addWidget(filterCtor_, 0, 3);
    filtersGrid->addWidget(filterField_, 1, 0);
    filtersGrid->addWidget(filterProperty_, 1, 1);
    filtersGrid->addWidget(filterEvent_, 1, 2);
    filtersGrid->addWidget(filterEnumValue_, 1, 3);

    resultsLay->addLayout(filtersGrid);

    resultsList_ = new QListWidget(resultsCard);
    resultsList_->setUniformItemSizes(true);
    resultsList_->setItemDelegate(new SignatureHighlightDelegate(resultsList_));
    resultsLay->addWidget(resultsList_, 1);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels({"Name"});

    proxy_ = new TreeFilterProxy(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(0);

    tree_->setModel(proxy_);
    tree_->setUniformRowHeights(true);
    tree_->setAnimated(false);
    tree_->setItemDelegate(new SignatureHighlightDelegate(tree_));

    splitter->addWidget(treeCard);
    splitter->addWidget(rightSplitter);
    rightSplitter->addWidget(detailsCard);
    rightSplitter->addWidget(resultsCard);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 4);
    splitter->setHandleWidth(10);
    splitter->setSizes({800, 520});
    rightSplitter->setStretchFactor(0, 3);
    rightSplitter->setStretchFactor(1, 2);
    contentLay->addWidget(splitter, 1);

    connect(tree_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& proxyIdx, const QModelIndex&) {
                if (!proxyIdx.isValid()) return;
                const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
                details_->setPlainText(srcIdx.data(Qt::UserRole + 100).toString());
            });

    connect(tree_, &QTreeView::expanded, this, [this](const QModelIndex& proxyIdx) {
        const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
        onTreeExpanded(srcIdx);
    });

    setupInteractions();
    return page;
}

void MainWindow::setupInteractions() {
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        proxy_->setFilterFixedString(text);
        if (!text.isEmpty())
            tree_->expandToDepth(2);
    });

    connect(resultsSearch_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateSearchResults();
    });
    connect(resultsList_, &QListWidget::itemActivated, this, &MainWindow::navigateToSearchResult);
    connect(resultsList_, &QListWidget::itemClicked, this, &MainWindow::navigateToSearchResult);

    auto connectFilter = [this](QCheckBox* cb) {
        connect(cb, &QCheckBox::toggled, this, [this](bool) { updateSearchResults(); });
    };
    connectFilter(filterNs_);
    connectFilter(filterType_);
    connectFilter(filterMethod_);
    connectFilter(filterCtor_);
    connectFilter(filterField_);
    connectFilter(filterProperty_);
    connectFilter(filterEvent_);
    connectFilter(filterEnumValue_);

    connect(tree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& proxyIdx) {
        if (!proxyIdx.isValid()) return;
        const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);

        const QString sig = srcIdx.data(Qt::UserRole + 101).toString();
        const QString off = srcIdx.data(Qt::UserRole + 102).toString();
        const QString va  = srcIdx.data(Qt::UserRole + 103).toString();

        if (!off.isEmpty() && off != "0x0") copyTextToClipboard(off, "Copied Offset: " + off);
        else if (!va.isEmpty() && va != "0x0") copyTextToClipboard(va, "Copied VA: " + va);
        else if (!sig.isEmpty()) copyTextToClipboard(sig, "Copied Signature");
    });

    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
}

void MainWindow::showContextMenu(const QPoint& p) {
    const QModelIndex proxyIdx = tree_->indexAt(p);
    if (!proxyIdx.isValid()) return;

    const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);

    const QString sig = srcIdx.data(Qt::UserRole + 101).toString();
    const QString rva = srcIdx.data(Qt::UserRole + 104).toString();
    const QString off = srcIdx.data(Qt::UserRole + 102).toString();
    const QString va  = srcIdx.data(Qt::UserRole + 103).toString();

    QMenu menu(this);

    if (!sig.isEmpty())
        menu.addAction("Copy Signature", this, [this, sig]() { copyTextToClipboard(sig, "Copied Signature"); });

    if (!rva.isEmpty() && rva != "0x0")
        menu.addAction("Copy RVA", this, [this, rva]() { copyTextToClipboard(rva, "Copied RVA: " + rva); });

    if (!off.isEmpty() && off != "0x0")
        menu.addAction("Copy Offset", this, [this, off]() { copyTextToClipboard(off, "Copied Offset: " + off); });

    if (!va.isEmpty() && va != "0x0")
        menu.addAction("Copy VA", this, [this, va]() { copyTextToClipboard(va, "Copied VA: " + va); });

    if (menu.actions().isEmpty()) return;
    menu.exec(tree_->viewport()->mapToGlobal(p));
}

void MainWindow::copyTextToClipboard(const QString& text, const QString& statusMsg) {
    QApplication::clipboard()->setText(text);
    statusBar()->showMessage(statusMsg, 2500);
}

void MainWindow::setBusy(bool busy, const QString& msg) {
    if (busyRow_) busyRow_->setVisible(busy);
    if (busyLabel_) busyLabel_->setText(msg.isEmpty() ? "Parsing…" : msg);
    if (openBtn_) openBtn_->setEnabled(!busy);
    if (compareBtn_) compareBtn_->setEnabled(!busy && hasLoadedPrimary_);
    if (search_) search_->setEnabled(!busy);
    if (tree_) tree_->setEnabled(!busy);
    if (busy)
        statusBar()->showMessage(msg.isEmpty() ? "Parsing…" : msg);
}

void MainWindow::openDumpCs() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Open dump.cs", {}, "C# dump (*.cs);;All files (*.*)");
    if (path.isEmpty()) return;

    startParseAsync(path);
}

void MainWindow::compareDumpCs() {
    if (!hasLoadedPrimary_ || parsePath_.isEmpty())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, "Compare to dump.cs", {}, "C# dump (*.cs);;All files (*.*)");
    if (path.isEmpty())
        return;

    const auto baseTypes = types_;
    const QString basePath = parsePath_;

    setBusy(true, "Comparing: " + basePath + "  vs  " + path);

    auto* cmpWatcher = new QFutureWatcher<std::vector<DumpType>>(this);
    connect(cmpWatcher, &QFutureWatcher<std::vector<DumpType>>::finished, this, [this, cmpWatcher, baseTypes, basePath, path]() {
        const auto newTypes = cmpWatcher->result();
        cmpWatcher->deleteLater();

        struct Entry { MemberKind kind; quint64 offset; QString item; };

        const auto buildMap = [](const std::vector<DumpType>& types) {
            QHash<QString, Entry> map;
            for (int ti = 0; ti < (int)types.size(); ++ti) {
                const auto& t = types[(size_t)ti];
                const QString typeFqn = QString::fromStdString(t.nameSpace + "::" + t.name);
                for (int mi = 0; mi < (int)t.members.size(); ++mi) {
                    const auto& m = t.members[(size_t)mi];
                    const QString sigKey = normalizeSignature(m);
                    const QString key = typeFqn + "|" + kindToString(m.kind) + "|" + sigKey;
                    Entry e;
                    e.kind = m.kind;
                    e.offset = (quint64)m.offset;
                    e.item = typeFqn + "  " + kindToString(m.kind) + "  " + QString::fromStdString(m.signature);
                    map.insert(key, e);
                }
            }
            return map;
        };

        const auto oldMap = buildMap(baseTypes);
        const auto newMap = buildMap(newTypes);

        struct Row { QString status; QString item; QString oldOff; QString newOff; };
        QVector<Row> rows;
        rows.reserve(oldMap.size() + newMap.size());

        int changed = 0, added = 0, removed = 0;

        for (auto it = oldMap.begin(); it != oldMap.end(); ++it) {
            const auto jt = newMap.find(it.key());
            if (jt == newMap.end()) {
                ++removed;
                rows.push_back({"Removed", it.value().item,
                                QString("0x%1").arg(QString::number(it.value().offset, 16)),
                                {}});
            } else if (jt.value().offset != it.value().offset) {
                ++changed;
                rows.push_back({"Changed", it.value().item,
                                QString("0x%1").arg(QString::number(it.value().offset, 16)),
                                QString("0x%1").arg(QString::number(jt.value().offset, 16))});
            }
        }

        for (auto it = newMap.begin(); it != newMap.end(); ++it) {
            if (!oldMap.contains(it.key())) {
                ++added;
                rows.push_back({"Added", it.value().item, {},
                                QString("0x%1").arg(QString::number(it.value().offset, 16))});
            }
        }

        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(QString("Diff (%1 changed, %2 added, %3 removed)").arg(changed).arg(added).arg(removed));
        dlg->resize(980, 640);

        auto* lay = new QVBoxLayout(dlg);
        lay->setContentsMargins(12, 12, 12, 12);
        lay->setSpacing(8);

        auto* filter = new QLineEdit(dlg);
        filter->setPlaceholderText("Filter diff...");
        lay->addWidget(filter);

        auto* statusRow = new QHBoxLayout();
        statusRow->setContentsMargins(0, 0, 0, 0);
        statusRow->setSpacing(10);

        auto* cbAdded = new QCheckBox("Added", dlg);
        auto* cbChanged = new QCheckBox("Changed", dlg);
        auto* cbRemoved = new QCheckBox("Removed", dlg);
        cbAdded->setChecked(true);
        cbChanged->setChecked(true);
        cbRemoved->setChecked(true);

        statusRow->addWidget(cbAdded);
        statusRow->addWidget(cbChanged);
        statusRow->addWidget(cbRemoved);
        statusRow->addStretch(1);
        lay->addLayout(statusRow);

        auto* model = new QStandardItemModel(dlg);
        model->setHorizontalHeaderLabels({"Status", "Item", "Old Offset", "New Offset"});
        for (const auto& r : rows) {
            QList<QStandardItem*> items;

            auto* statusItem = new QStandardItem(r.status);
            QColor statusColor(215, 246, 241);
            QColor statusBg(11, 42, 45);
            if (r.status == "Added") { statusColor = QColor(34, 255, 102); statusBg = QColor(15, 60, 40); }
            else if (r.status == "Removed") { statusColor = QColor(255, 110, 110); statusBg = QColor(60, 20, 20); }
            else if (r.status == "Changed") { statusColor = QColor(255, 200, 90); statusBg = QColor(55, 45, 20); }
            statusItem->setForeground(QBrush(statusColor));
            statusItem->setBackground(QBrush(statusBg));

            items << statusItem;
            items << new QStandardItem(r.item);
            items << new QStandardItem(r.oldOff);
            items << new QStandardItem(r.newOff);
            model->appendRow(items);
        }

        auto* statusProxy = new QSortFilterProxyModel(dlg);
        statusProxy->setSourceModel(model);
        statusProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        statusProxy->setFilterKeyColumn(0);

        auto* textProxy = new QSortFilterProxyModel(dlg);
        textProxy->setSourceModel(statusProxy);
        textProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        textProxy->setFilterKeyColumn(1);

        auto* view = new QTableView(dlg);
        view->setModel(textProxy);
        view->setItemDelegateForColumn(1, new SignatureHighlightDelegate(view));
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSortingEnabled(true);
        view->horizontalHeader()->setStretchLastSection(true);
        view->verticalHeader()->setVisible(false);
        lay->addWidget(view, 1);

        auto updateStatusFilter = [statusProxy, cbAdded, cbChanged, cbRemoved]() {
            QStringList parts;
            if (cbAdded->isChecked()) parts << "Added";
            if (cbChanged->isChecked()) parts << "Changed";
            if (cbRemoved->isChecked()) parts << "Removed";

            if (parts.isEmpty()) {
                statusProxy->setFilterRegularExpression(QRegularExpression("a^"));
                return;
            }
            const QString pat = QString("^(%1)$").arg(parts.join("|"));
            statusProxy->setFilterRegularExpression(QRegularExpression(pat, QRegularExpression::CaseInsensitiveOption));
        };

        connect(cbAdded, &QCheckBox::toggled, dlg, [updateStatusFilter](bool) { updateStatusFilter(); });
        connect(cbChanged, &QCheckBox::toggled, dlg, [updateStatusFilter](bool) { updateStatusFilter(); });
        connect(cbRemoved, &QCheckBox::toggled, dlg, [updateStatusFilter](bool) { updateStatusFilter(); });
        updateStatusFilter();

        connect(filter, &QLineEdit::textChanged, dlg, [textProxy](const QString& t) {
            textProxy->setFilterFixedString(t);
        });

        dlg->show();

        setBusy(false);
        statusBar()->showMessage("Compared: " + basePath + " vs " + path, 4000);
    });

    auto future = QtConcurrent::run([path]() {
        return DumpCsParser::parse(path.toStdString());
    });
    cmpWatcher->setFuture(future);
}

void MainWindow::startParseAsync(const QString& path) {
    parsePath_ = path;
    stack_->setCurrentWidget(explorerPage_);

    setBusy(true, "Parsing: " + path);

    auto future = QtConcurrent::run([this, path]() {
        DumpCsParser::setProgressCallback([this](int pct) {
            QMetaObject::invokeMethod(this, [this, pct]() {
                if (busyBar_) busyBar_->setValue(pct);
            }, Qt::QueuedConnection);
        });

        return DumpCsParser::parse(path.toStdString());
    });
    watcher_->setFuture(future);
}

void MainWindow::finishParseAsync() {
    types_ = watcher_->result();

    hasLoadedPrimary_ = true;
    if (compareBtn_) {
        compareBtn_->setVisible(true);
        compareBtn_->setEnabled(true);
    }

    populateTree();
    buildSearchIndex();
    updateSearchResults();
    setBusy(false);

    statusBar()->showMessage("Loaded: " + parsePath_, 3000);
}

void MainWindow::populateTree() {
    model_->clear();
    model_->setHorizontalHeaderLabels({"Name"});

    nsItems_.clear();
    typeItems_.assign(types_.size(), nullptr);
    nsRootItem_ = nullptr;

    auto* root = model_->invisibleRootItem();
    auto* nsRoot = new QStandardItem("Namespaces");
    nsRoot->setIcon(icoNamespace_);
    nsRoot->setData("Namespaces", Qt::UserRole + 100);
    root->appendRow(nsRoot);

    nsRootItem_ = nsRoot;

    std::map<std::string, std::vector<int>> nsMap;
    for (size_t i = 0; i < types_.size(); ++i)
        nsMap[types_[i].nameSpace].push_back((int)i);

    for (auto& [ns, indices] : nsMap) {
        auto* nsItem = new QStandardItem(QString::fromStdString(ns));
        nsItem->setIcon(icoNamespace_);
        nsItem->setData(QString::fromStdString(ns), Qt::UserRole + 100);
        nsRoot->appendRow(nsItem);

        nsItems_[ns] = nsItem;

        for (int idx : indices) {
            const bool isEnum = types_[idx].isEnum;
            auto* typeItem = new QStandardItem(QString::fromStdString(types_[idx].name));
            typeItem->setIcon(isEnum ? icoEnumType_ : icoClass_);
            typeItem->setData(isEnum ? QString("#A06EFF") : QString("#50A0FF"), Qt::UserRole + 200);
            typeItem->setForeground(isEnum ? QBrush(QColor(160, 110, 255)) : QBrush(QColor(80, 160, 255)));
            typeItem->setData(idx, Qt::UserRole + 1);
            typeItem->setData("Type", Qt::UserRole + 2);
            typeItem->setData(QString::fromStdString(types_[idx].nameSpace + "::" + types_[idx].name), Qt::UserRole + 100);
            typeItem->appendRow(new QStandardItem("Loading..."));
            nsItem->appendRow(typeItem);

            if ((size_t)idx < typeItems_.size())
                typeItems_[(size_t)idx] = typeItem;
        }
    }

    tree_->expand(proxy_->mapFromSource(nsRoot->index()));
}

void MainWindow::onTreeExpanded(const QModelIndex& srcIdx) {
    auto* item = model_->itemFromIndex(srcIdx);
    if (!item) return;

    int loadingRow = -1;
    for (int r = 0; r < item->rowCount(); ++r) {
        auto* ch = item->child(r);
        if (ch && ch->text() == "Loading...") {
            loadingRow = r;
            break;
        }
    }
    if (loadingRow < 0) return;

    item->removeRow(loadingRow);

    if (item->data(Qt::UserRole + 2).toString() == "Type")
        buildTypeChildren(item, item->data(Qt::UserRole + 1).toInt());
    else
        buildGroupChildren(item);
}

void MainWindow::buildTypeChildren(QStandardItem* typeItem, int typeIndex) {
    const bool isEnumType = types_[typeIndex].isEnum;

    bool hasCtor = false;
    bool hasMethod = false;
    bool hasField = false;
    bool hasProperty = false;
    bool hasEvent = false;
    bool hasEnumValue = false;

    for (const auto& m : types_[typeIndex].members) {
        switch (m.kind) {
        case MemberKind::Ctor:      hasCtor = true; break;
        case MemberKind::Method:    hasMethod = true; break;
        case MemberKind::Field:     hasField = true; break;
        case MemberKind::Property:  hasProperty = true; break;
        case MemberKind::Event:     hasEvent = true; break;
        case MemberKind::EnumValue: hasEnumValue = true; break;
        default: break;
        }
    }

    struct GroupDef { const char* label; const char* key; const QIcon* icon; bool enumOnly; };
    const GroupDef groups[] = {
        {"Fields",       "field",   &icoField_,     false},
        {"Constructors", "ctor",    &icoCtor_,      false},
        {"Methods",      "method",  &icoMethod_,    false},
        {"Properties",   "property",&icoProperty_,  false},
        {"Events",       "event",   &icoEvent_,     false},
        {"Enum Values",  "enum",    &icoEnumValue_, true}
    };

    for (const auto& g : groups) {
        const std::string key = g.key;
        if (isEnumType && key != "field" && key != "enum")
            continue;
        if (g.enumOnly && !isEnumType)
            continue;

        bool present = false;
        if (key == "ctor") present = hasCtor;
        else if (key == "method") present = hasMethod;
        else if (key == "field") present = hasField;
        else if (key == "property") present = hasProperty;
        else if (key == "event") present = hasEvent;
        else if (key == "enum") present = hasEnumValue;

        if (!present)
            continue;

        auto* grp = new QStandardItem(g.label);
        grp->setIcon(*g.icon);
        grp->setData(QString(g.key), Qt::UserRole + 3);
        grp->setData(typeIndex, Qt::UserRole + 1);
        grp->appendRow(new QStandardItem("Loading..."));
        typeItem->appendRow(grp);
    }
}

void MainWindow::buildGroupChildren(QStandardItem* groupItem) {
    const int typeIndex = groupItem->data(Qt::UserRole + 1).toInt();
    const QString groupKey = groupItem->data(Qt::UserRole + 3).toString();
    const auto& members = types_[typeIndex].members;

    const QIcon childIcon =
        (groupKey == "ctor")     ? icoCtor_ :
        (groupKey == "method")   ? icoMethod_ :
        (groupKey == "field")    ? icoField_ :
        (groupKey == "property") ? icoProperty_ :
        (groupKey == "event")    ? icoEvent_ :
        (groupKey == "enum")     ? icoEnumValue_ :
        icoClass_;

    std::unordered_set<int> existing;
    existing.reserve((size_t)groupItem->rowCount());
    for (int r = 0; r < groupItem->rowCount(); ++r) {
        auto* ch = groupItem->child(r);
        if (!ch) continue;
        if (!ch->data(Qt::UserRole + 10).isValid()) continue;
        existing.insert(ch->data(Qt::UserRole + 10).toInt());
    }

    bool any = groupItem->rowCount() > 0;
    for (int mi = 0; mi < (int)members.size(); ++mi) {
        const auto& m = members[(size_t)mi];
        if (!groupMatches(m.kind, groupKey))
            continue;

        if (existing.contains(mi))
            continue;

        any = true;

        const QString display = QString::fromStdString(m.signature);
        const QString detail  = QString::fromStdString(m.details);

        const QString sig = QString::fromStdString(m.signature);
        const QString rva = QString("0x%1").arg(QString::number((qulonglong)m.rva, 16));
        const QString off = QString("0x%1").arg(QString::number((qulonglong)m.offset, 16));
        const QString va  = QString("0x%1").arg(QString::number((qulonglong)m.va, 16));

        auto* it = new QStandardItem(display);
        it->setIcon(childIcon);

        it->setData(detail, Qt::UserRole + 100);
        it->setData(sig,    Qt::UserRole + 101);
        it->setData(off,    Qt::UserRole + 102);
        it->setData(va,     Qt::UserRole + 103);
        it->setData(rva,    Qt::UserRole + 104);
        it->setData(mi,     Qt::UserRole + 10);

        groupItem->appendRow(it);
    }

    if (!any && groupItem->rowCount() == 0) {
        auto* none = new QStandardItem("(none)");
        none->setEnabled(false);
        groupItem->appendRow(none);
    }
}
