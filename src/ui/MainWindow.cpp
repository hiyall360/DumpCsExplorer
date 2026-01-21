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
#include <QMap>
#include <QGridLayout>
#include <QSignalBlocker>
#include <QFrame>
#include <QPixmap>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QDialog>
#include <QComboBox>
#include <QMessageBox>
#include <QTabWidget>
#include <QFile>
#include <QTextStream>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTableView>
#include <QHeaderView>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>
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
#include <QCloseEvent>

#include <QTimer>

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

static QString defaultSnippetTemplateBnm() {
    return
        "auto clazz = BNM::Class(\"${namespace}\", \"${className}\");\n"
        "auto method = clazz.GetMethod(\"${methodName}\", ${parameterCount});\n";
}

static QString defaultSnippetTemplateFrida() {
    return
        "var asm = Il2Cpp.Domain.assembly(\"${assemblyName}\");\n"
        "var clazz = asm.image.class(\"${namespace}\", \"${className}\");\n"
        "var method = clazz.method(\"${methodName}\", ${parameterCount});\n";
}

static QString snippetSettingsPath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    QDir().mkpath(base);
    return base + "/DumpCsExplorer.ini";
}

static QSettings appSettings() {
    return QSettings(snippetSettingsPath(), QSettings::IniFormat);
}

static QStringList loadRecentFiles() {
    QSettings s = appSettings();
    s.beginGroup("App");
    QStringList list = s.value("RecentFiles").toStringList();
    s.endGroup();
    return list;
}

static void saveRecentFiles(const QStringList& list) {
    QSettings s = appSettings();
    s.beginGroup("App");
    s.setValue("RecentFiles", list);
    s.endGroup();
}

static QString applySnippetTemplate(QString tpl, const QMap<QString, QString>& vars) {
    for (auto it = vars.begin(); it != vars.end(); ++it)
        tpl.replace(it.key(), it.value());
    return tpl;
}

static QString uniqueTemplateName(const QMap<QString, QString>& existing, const QString& base) {
    if (!existing.contains(base))
        return base;
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QString("%1 (%2)").arg(base).arg(i);
        if (!existing.contains(candidate))
            return candidate;
    }
    return base;
}

static QStringList extractSnippetPlaceholders(const QString& tpl) {
    static const QRegularExpression rx(R"(\$\{[A-Za-z_][A-Za-z0-9_]*\})");
    QStringList out;
    QRegularExpressionMatchIterator it = rx.globalMatch(tpl);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString v = m.captured(0);
        if (!out.contains(v))
            out.push_back(v);
    }
    return out;
}

static QStringList validateSnippetPlaceholders(const QString& tpl) {
    static const QStringList allowed = {
        "${assemblyName}", "${assembly}", "${namespace}", "${className}", "${methodName}", "${parameterCount}"
    };

    QStringList bad;
    const auto vars = extractSnippetPlaceholders(tpl);
    for (const auto& v : vars) {
        if (!allowed.contains(v) && !bad.contains(v))
            bad.push_back(v);
    }
    return bad;
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

static int countParamsTopLevel(const std::string& params) {
    size_t i = 0;
    while (i < params.size() && (params[i] == ' ' || params[i] == '\t'))
        ++i;
    if (i >= params.size())
        return 0;

    int depthAngle = 0;
    int count = 1;
    for (char c : params) {
        if (c == '<') ++depthAngle;
        else if (c == '>' && depthAngle > 0) --depthAngle;
        else if (c == ',' && depthAngle == 0) ++count;
    }
    return count;
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

class ResultsQueryHighlightDelegate final : public QStyledItemDelegate {
public:
    explicit ResultsQueryHighlightDelegate(QLineEdit* query, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), query_(query) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const QString q = query_ ? query_->text().trimmed() : QString();
        const QString text = opt.text;

        opt.text.clear();
        QStyle* st = opt.widget ? opt.widget->style() : QApplication::style();
        st->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const QRect textRect = st->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        if (!textRect.isValid())
            return;

        painter->save();
        painter->setClipRect(textRect);

        const QColor baseText = opt.palette.color((opt.state & QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text);
        const QColor hlBg(255, 220, 110, 120);
        const QColor hlText = baseText;

        QFontMetrics fm(opt.font);
        int x = textRect.left();
        const int y = textRect.top() + fm.ascent() + (textRect.height() - fm.height()) / 2;

        auto drawSeg = [&](const QString& seg, const QColor& col, bool highlight) {
            if (seg.isEmpty())
                return true;
            const int w = fm.horizontalAdvance(seg);
            if (x + w > textRect.right() + 1)
                return false;

            if (highlight) {
                QRect r(x, y - fm.ascent(), w, fm.height());
                painter->fillRect(r.adjusted(0, 1, 0, -1), QBrush(hlBg));
            }
            painter->setPen(col);
            painter->drawText(QPoint(x, y), seg);
            x += w;
            return true;
        };

        if (q.isEmpty()) {
            drawSeg(fm.elidedText(text, Qt::ElideRight, textRect.width()), baseText, false);
            painter->restore();
            return;
        }

        const QString lowText = text.toLower();
        const QString lowQ = q.toLower();

        int pos = 0;
        while (pos < text.size()) {
            const int hit = lowText.indexOf(lowQ, pos);
            if (hit < 0) {
                const QString tail = text.mid(pos);
                drawSeg(fm.elidedText(tail, Qt::ElideRight, textRect.right() + 1 - x), baseText, false);
                break;
            }

            const QString pre = text.mid(pos, hit - pos);
            if (!drawSeg(pre, baseText, false))
                break;

            const QString mid = text.mid(hit, lowQ.size());
            if (!drawSeg(mid, hlText, true))
                break;

            pos = hit + lowQ.size();
        }

        painter->restore();
    }

private:
    QLineEdit* query_ = nullptr;
};

void MainWindow::buildSearchIndex() {
    searchIndex_.clear();

    for (const auto& [key, item] : nsItems_) {
        (void)item;
        const auto pos = key.find('|');
        const std::string asmName = (pos == std::string::npos) ? std::string() : key.substr(0, pos);
        const std::string nsName = (pos == std::string::npos) ? key : key.substr(pos + 1);

        SearchEntry e;
        e.kind = SearchEntry::Kind::Namespace;
        e.assembly = QString::fromStdString(asmName);
        e.ns = QString::fromStdString(nsName);
        e.display = e.ns;
        e.detail = (e.assembly.isEmpty() ? QString() : (e.assembly + " :: ")) + e.ns;
        e.haystack = (e.display + "\n" + e.detail).toLower();
        searchIndex_.push_back(e);
    }

    for (int ti = 0; ti < (int)types_.size(); ++ti) {
        const auto& t = types_[(size_t)ti];
        SearchEntry e;
        e.kind = SearchEntry::Kind::Type;
        e.assembly = QString::fromStdString(t.assembly);
        e.ns = QString::fromStdString(t.nameSpace);
        e.typeIndex = ti;
        const QString typeFqn = QString::fromStdString(t.nameSpace + "::" + t.name);
        e.display = QString::fromStdString(t.name);
        e.detail = (e.assembly.isEmpty() ? QString() : (e.assembly + " :: ")) + typeFqn;
        e.haystack = (e.display + "\n" + e.detail).toLower();
        searchIndex_.push_back(e);

        for (int mi = 0; mi < (int)t.members.size(); ++mi) {
            const auto& m = t.members[(size_t)mi];
            SearchEntry em;
            em.kind = SearchEntry::Kind::Member;
            em.assembly = QString::fromStdString(t.assembly);
            em.ns = QString::fromStdString(t.nameSpace);
            em.typeIndex = ti;
            em.memberIndex = mi;
            em.memberKind = m.kind;
            const QString memberDisp = QString::fromStdString(t.name + "  " + m.signature);
            const QString memberFqn = QString::fromStdString(t.nameSpace + "::" + t.name + "  " + m.signature);
            em.display = memberDisp;
            em.detail = (em.assembly.isEmpty() ? QString() : (em.assembly + " :: ")) + memberFqn + "\n" + QString::fromStdString(m.details);
            em.haystack = (em.display + "\n" + em.detail).toLower();
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


    if (!resultsFilterWatcher_) {
        resultsFilterWatcher_ = new QFutureWatcher<QVector<int>>(this);
        connect(resultsFilterWatcher_, &QFutureWatcher<QVector<int>>::finished, this, [this]() {
            const int req = resultsFilterWatcher_->property("requestId").toInt();
            if (req != resultsFilterRequestId_)
                return;

            const QVector<int> indices = resultsFilterWatcher_->result();
            resultsList_->setUpdatesEnabled(false);
            resultsList_->clear();
            for (int i : indices) {
                if (i < 0 || i >= (int)searchIndex_.size())
                    continue;
                const auto& e = searchIndex_[(size_t)i];
                auto* it = new QListWidgetItem(e.display, resultsList_);
                it->setToolTip(e.detail);
                if (e.kind == SearchEntry::Kind::Namespace) {
                    it->setIcon(icoNamespace_);
                } else if (e.kind == SearchEntry::Kind::Type) {
                    bool isEnum = false;
                    if (e.typeIndex >= 0 && (size_t)e.typeIndex < types_.size())
                        isEnum = types_[(size_t)e.typeIndex].isEnum;
                    it->setIcon(isEnum ? icoEnumType_ : icoClass_);
                    it->setForeground(isEnum ? QBrush(QColor(160, 110, 255)) : QBrush(QColor(80, 160, 255)));
                } else {
                    const QIcon childIcon =
                        (e.memberKind == MemberKind::Ctor)     ? icoCtor_ :
                        (e.memberKind == MemberKind::Method)   ? icoMethod_ :
                        (e.memberKind == MemberKind::Field)    ? icoField_ :
                        (e.memberKind == MemberKind::Property) ? icoProperty_ :
                        (e.memberKind == MemberKind::Event)    ? icoEvent_ :
                        (e.memberKind == MemberKind::EnumValue)? icoEnumValue_ :
                        icoClass_;
                    it->setIcon(childIcon);
                }
                it->setData(Qt::UserRole + 1, i);
            }
            resultsList_->setUpdatesEnabled(true);
            if (resultsCount_)
                resultsCount_->setText(QString::number(indices.size()));
        });
    }

    const int scopeMode = resultsScope_ ? resultsScope_->currentIndex() : 0;
    QString scopeAssembly;
    QString scopeNs;
    int scopeTypeIndex = -1;
    bool scopeValid = (scopeMode == 0);

    auto fillFromTypeIndex = [&](int ti) {
        if (ti < 0 || (size_t)ti >= types_.size())
            return false;
        const auto& t = types_[(size_t)ti];
        scopeAssembly = QString::fromStdString(t.assembly);
        scopeNs = QString::fromStdString(t.nameSpace);
        scopeTypeIndex = ti;
        return true;
    };

    if (scopeMode == 1 || scopeMode == 2 || scopeMode == 3) {
        if (fillFromTypeIndex(selectedTypeIndex_))
            scopeValid = true;

        if (!scopeValid && tree_) {
            QModelIndex idx = tree_->currentIndex();
            if (idx.isValid() && proxy_)
                idx = proxy_->mapToSource(idx);

            int ti = -1;
            if (idx.data(Qt::UserRole + 11).isValid()) {
                ti = idx.data(Qt::UserRole + 11).toInt();
            } else if (idx.data(Qt::UserRole + 2).toString() == "Type") {
                ti = idx.data(Qt::UserRole + 1).toInt();
            } else {
                QModelIndex cur = idx;
                while (cur.isValid()) {
                    if (cur.data(Qt::UserRole + 2).toString() == "Type") {
                        ti = cur.data(Qt::UserRole + 1).toInt();
                        break;
                    }
                    cur = cur.parent();
                }
            }

            if (ti >= 0) {
                if (fillFromTypeIndex(ti))
                    scopeValid = true;
            }

            if (!scopeValid) {
                const QString curName = idx.data(Qt::UserRole + 100).toString();
                const QString parentName = idx.parent().data(Qt::UserRole + 100).toString();
                if (!curName.isEmpty()) {
                    if (parentName == "Assemblies") {
                        scopeAssembly = curName;
                        scopeNs.clear();
                        scopeTypeIndex = -1;
                        scopeValid = true;
                    } else if (!parentName.isEmpty()) {
                        scopeAssembly = parentName;
                        scopeNs = curName;
                        scopeTypeIndex = -1;
                        scopeValid = true;
                    }
                }
            }
        }

        if (scopeMode == 1) {
            scopeNs.clear();
            scopeTypeIndex = -1;
            scopeValid = !scopeAssembly.isEmpty();
        } else if (scopeMode == 2) {
            scopeTypeIndex = -1;
            scopeValid = !scopeAssembly.isEmpty() && !scopeNs.isEmpty();
        } else if (scopeMode == 3) {
            scopeValid = scopeTypeIndex >= 0;
        }
    }

    ++resultsFilterRequestId_;
    const int requestId = resultsFilterRequestId_;
    resultsFilterWatcher_->setProperty("requestId", requestId);

    const auto indexSnapshot = &searchIndex_;
    QFuture<QVector<int>> fut = QtConcurrent::run([=]() {
        QVector<int> out;
        out.reserve((int)indexSnapshot->size());

        if (scopeMode != 0 && !scopeValid) {
            out.squeeze();
            return out;
        }

        for (int i = 0; i < (int)indexSnapshot->size(); ++i) {
            const auto& e = (*indexSnapshot)[(size_t)i];

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

            if (scopeMode == 1) {
                if (e.assembly != scopeAssembly)
                    continue;
            } else if (scopeMode == 2) {
                if (e.assembly != scopeAssembly || e.ns != scopeNs)
                    continue;
            } else if (scopeMode == 3) {
                if (e.typeIndex != scopeTypeIndex)
                    continue;
            }

            if (!q.isEmpty() && !e.haystack.contains(q))
                continue;

            out.push_back(i);
        }

        out.squeeze();
        return out;
    });

    resultsFilterWatcher_->setFuture(fut);
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
        const std::string key = e.assembly.toStdString() + "|" + e.ns.toStdString();
        auto it = nsItems_.find(key);
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
            it->setData(e.typeIndex, Qt::UserRole + 11);
            it->setData((int)e.memberKind, Qt::UserRole + 12);

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

    QSettings settings(snippetSettingsPath(), QSettings::IniFormat);
    settings.beginGroup("Snippets");
    const QString legacySingle = settings.value("Template").toString();
    snippetDefaultTemplateName_ = settings.value("DefaultTemplate").toString();

    settings.beginGroup("Templates");
    const QStringList keys = settings.childKeys();
    for (const auto& k : keys)
        snippetTemplates_.insert(k, settings.value(k).toString());
    settings.endGroup();

    if (snippetTemplates_.isEmpty()) {
        if (!legacySingle.isEmpty()) {
            snippetDefaultTemplateName_ = "Default";
            snippetTemplates_.insert(snippetDefaultTemplateName_, legacySingle);
        } else {
            snippetDefaultTemplateName_ = "BNM";
            snippetTemplates_.insert("BNM", defaultSnippetTemplateBnm());
            snippetTemplates_.insert("Frida", defaultSnippetTemplateFrida());
        }

        settings.setValue("DefaultTemplate", snippetDefaultTemplateName_);
        settings.remove("Template");
        settings.beginGroup("Templates");
        for (auto it = snippetTemplates_.begin(); it != snippetTemplates_.end(); ++it)
            settings.setValue(it.key(), it.value());
        settings.endGroup();
    }

    if (snippetDefaultTemplateName_.isEmpty() || !snippetTemplates_.contains(snippetDefaultTemplateName_)) {
        snippetDefaultTemplateName_ = snippetTemplates_.isEmpty() ? QString() : snippetTemplates_.firstKey();
        settings.setValue("DefaultTemplate", snippetDefaultTemplateName_);
    }
    settings.endGroup();

    watcher_ = new QFutureWatcher<std::vector<DumpType>>(this);
    connect(watcher_, &QFutureWatcher<std::vector<DumpType>>::finished,
            this, &MainWindow::finishParseAsync);

    restoreUiState();
    refreshRecentUi();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    saveUiState();
    QMainWindow::closeEvent(e);
}

void MainWindow::addRecentFile(const QString& path) {
    if (path.isEmpty())
        return;

    QStringList list = loadRecentFiles();
    list.removeAll(path);
    list.prepend(path);
    while (list.size() > 12)
        list.removeLast();
    saveRecentFiles(list);
}

void MainWindow::refreshRecentUi() {
    if (!recentList_)
        return;
    recentList_->clear();

    QStringList list = loadRecentFiles();
    QStringList cleaned;
    for (const auto& p : list) {
        if (p.isEmpty())
            continue;
        const QFileInfo fi(p);
        if (!fi.exists())
            continue;

        cleaned << fi.absoluteFilePath();
        const QString label = fi.fileName() + "\n" + fi.absoluteFilePath();
        auto* it = new QListWidgetItem(label, recentList_);
        it->setData(Qt::UserRole, fi.absoluteFilePath());
    }

    if (cleaned != list)
        saveRecentFiles(cleaned);
}

void MainWindow::restoreUiState() {
    QSettings s = appSettings();
    s.beginGroup("Ui");
    const QByteArray mainSplit = s.value("MainSplitter").toByteArray();
    const QByteArray rightSplit = s.value("RightSplitter").toByteArray();
    s.endGroup();

    if (mainSplitter_ && !mainSplit.isEmpty())
        mainSplitter_->restoreState(mainSplit);
    if (rightSplitter_ && !rightSplit.isEmpty())
        rightSplitter_->restoreState(rightSplit);
}

void MainWindow::saveUiState() {
    QSettings s = appSettings();
    s.beginGroup("Ui");
    if (mainSplitter_)
        s.setValue("MainSplitter", mainSplitter_->saveState());
    if (rightSplitter_)
        s.setValue("RightSplitter", rightSplitter_->saveState());
    s.endGroup();
}

MainWindow::~MainWindow() = default;

void MainWindow::navigateToTypeOrMember(int typeIndex, int memberIndex, MemberKind memberKind) {
    auto expandProxyAncestors = [this](QModelIndex pidx) {
        for (QModelIndex cur = pidx; cur.isValid(); cur = cur.parent())
            tree_->expand(cur);
    };

    QStandardItem* target = nullptr;
    if (typeIndex >= 0 && memberIndex < 0) {
        if ((size_t)typeIndex < typeItems_.size())
            target = typeItems_[(size_t)typeIndex];
    } else if (typeIndex >= 0 && memberIndex >= 0) {
        if ((size_t)typeIndex >= typeItems_.size())
            return;
        auto* typeItem = typeItems_[(size_t)typeIndex];
        if (!typeItem)
            return;

        onTreeExpanded(typeItem->index());

        const QString groupKey =
            (memberKind == MemberKind::Ctor)     ? "ctor" :
            (memberKind == MemberKind::Method)   ? "method" :
            (memberKind == MemberKind::Field)    ? "field" :
            (memberKind == MemberKind::Property) ? "property" :
            (memberKind == MemberKind::Event)    ? "event" :
            (memberKind == MemberKind::EnumValue)? "enum" :
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
            if (ch->data(Qt::UserRole + 10).toInt() == memberIndex) {
                target = ch;
                break;
            }
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

QString MainWindow::buildSnippetText(int typeIndex, int memberIndex, const QString& templateName) const {
    if (typeIndex < 0 || (size_t)typeIndex >= types_.size())
        return {};
    const auto& t = types_[(size_t)typeIndex];
    if (memberIndex < 0 || (size_t)memberIndex >= t.members.size())
        return {};

    const auto& m = t.members[(size_t)memberIndex];
    if (!(m.kind == MemberKind::Method || m.kind == MemberKind::Ctor))
        return {};

    const QString clazz = QString::fromStdString(t.name);
    const QString method = QString::fromStdString(m.name);
    const int argc = countParamsTopLevel(m.params);

    QString assembly = QString::fromStdString(t.assembly);
    if (assembly.isEmpty())
        assembly = "(unknown)";

    QString ns = QString::fromStdString(t.nameSpace);
    const QString nsForSnippet = (ns == "-") ? QString() : ns;

    QMap<QString, QString> vars;
    vars.insert("${assemblyName}", assembly);
    vars.insert("${assembly}", assembly);
    vars.insert("${namespace}", nsForSnippet);
    vars.insert("${className}", clazz);
    vars.insert("${methodName}", method);
    vars.insert("${parameterCount}", QString::number(argc));

    QString chosen = templateName;
    if (chosen.isEmpty())
        chosen = snippetDefaultTemplateName_;
    if (chosen.isEmpty() || !snippetTemplates_.contains(chosen))
        chosen = snippetTemplates_.isEmpty() ? QString() : snippetTemplates_.firstKey();

    QString tpl = snippetTemplates_.value(chosen);
    if (tpl.isEmpty())
        tpl = defaultSnippetTemplateBnm();

    return applySnippetTemplate(tpl, vars);
}

void MainWindow::updateDetailsPanel(const QModelIndex& srcIdx) {
    selectedTypeIndex_ = -1;
    selectedMemberIndex_ = -1;
    selectedMemberKind_ = MemberKind::Method;
    selectedSignature_.clear();
    selectedRva_.clear();
    selectedOffset_.clear();
    selectedVa_.clear();
    selectedFqn_.clear();
    selectedSnippet_.clear();

    if (!srcIdx.isValid()) {
        if (detailsSummary_) detailsSummary_->clear();
        if (detailsRaw_) detailsRaw_->clear();
        return;
    }

    const bool hasMemberIndex = srcIdx.data(Qt::UserRole + 10).isValid() && srcIdx.data(Qt::UserRole + 11).isValid();
    if (hasMemberIndex) {
        selectedMemberIndex_ = srcIdx.data(Qt::UserRole + 10).toInt();
        selectedTypeIndex_ = srcIdx.data(Qt::UserRole + 11).toInt();
        selectedMemberKind_ = (MemberKind)srcIdx.data(Qt::UserRole + 12).toInt();
    } else if (srcIdx.data(Qt::UserRole + 2).toString() == "Type") {
        selectedTypeIndex_ = srcIdx.data(Qt::UserRole + 1).toInt();
    }

    QString assembly;
    QString ns;
    QString clazz;
    QString memberName;
    QString kind;

    if (selectedTypeIndex_ >= 0 && (size_t)selectedTypeIndex_ < types_.size()) {
        const auto& t = types_[(size_t)selectedTypeIndex_];
        assembly = QString::fromStdString(t.assembly);
        ns = QString::fromStdString(t.nameSpace);
        clazz = QString::fromStdString(t.name);
    }

    selectedSignature_ = srcIdx.data(Qt::UserRole + 101).toString();
    selectedOffset_ = srcIdx.data(Qt::UserRole + 102).toString();
    selectedVa_ = srcIdx.data(Qt::UserRole + 103).toString();
    selectedRva_ = srcIdx.data(Qt::UserRole + 104).toString();

    if (selectedMemberIndex_ >= 0 && selectedTypeIndex_ >= 0 && (size_t)selectedTypeIndex_ < types_.size()) {
        const auto& t = types_[(size_t)selectedTypeIndex_];
        if ((size_t)selectedMemberIndex_ < t.members.size()) {
            const auto& m = t.members[(size_t)selectedMemberIndex_];
            memberName = QString::fromStdString(m.name);
            kind = kindToString(m.kind);
        }
    }

    if (!assembly.isEmpty())
        selectedFqn_ = assembly + " :: " + ns + "::" + clazz;
    else
        selectedFqn_ = ns + "::" + clazz;
    if (!memberName.isEmpty())
        selectedFqn_ += "::" + memberName;

    if (selectedTypeIndex_ >= 0 && selectedMemberIndex_ >= 0 &&
        (selectedMemberKind_ == MemberKind::Method || selectedMemberKind_ == MemberKind::Ctor)) {
        selectedSnippet_ = buildSnippetText(selectedTypeIndex_, selectedMemberIndex_);
    }

    const QString body = srcIdx.data(Qt::UserRole + 100).toString();

    QString summary;
    if (!assembly.isEmpty()) summary += "Assembly: " + assembly + "\n";
    if (!ns.isEmpty()) summary += "Namespace: " + ns + "\n";
    if (!clazz.isEmpty()) summary += "Class: " + clazz + "\n";
    if (!kind.isEmpty()) summary += "Kind: " + kind + "\n";
    if (!memberName.isEmpty()) summary += "Name: " + memberName + "\n";
    if (!selectedSignature_.isEmpty()) summary += "Signature: " + selectedSignature_ + "\n";
    if (!selectedRva_.isEmpty() && selectedRva_ != "0x0") summary += "RVA: " + selectedRva_ + "\n";
    if (!selectedOffset_.isEmpty() && selectedOffset_ != "0x0") summary += "Offset: " + selectedOffset_ + "\n";
    if (!selectedVa_.isEmpty() && selectedVa_ != "0x0") summary += "VA: " + selectedVa_ + "\n";
    if (!selectedFqn_.isEmpty()) summary += "FQN: " + selectedFqn_ + "\n";

    if (detailsSummary_) detailsSummary_->setPlainText(summary.trimmed());
    if (detailsRaw_) {
        QString header;
        if (!assembly.isEmpty() || !ns.isEmpty() || !clazz.isEmpty()) {
            if (!assembly.isEmpty()) header += "Assembly: " + assembly + "\n";
            if (!ns.isEmpty()) header += "Namespace: " + ns + "\n";
            if (!clazz.isEmpty()) header += "Class: " + clazz + "\n\n";
        }
        detailsRaw_->setPlainText(header + body);
    }

    auto setEnabledIf = [](QPushButton* b, const QString& v) {
        if (b) b->setEnabled(!v.isEmpty());
    };
    setEnabledIf(copyRvaBtn_, selectedRva_);
    setEnabledIf(copyOffBtn_, selectedOffset_);
    setEnabledIf(copyVaBtn_, selectedVa_);
    setEnabledIf(copySnippetBtn_, selectedSnippet_);
    if (exportJsonBtn_) exportJsonBtn_->setEnabled(selectedTypeIndex_ >= 0);
    if (exportCsvBtn_) exportCsvBtn_->setEnabled(selectedTypeIndex_ >= 0);

    if (resultsScope_ && resultsScope_->currentIndex() > 0)
        updateSearchResults();
}

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

    auto* recentCard = new QFrame(body);
    recentCard->setObjectName("Card");
    auto* recentLay = new QVBoxLayout(recentCard);
    recentLay->setContentsMargins(8, 8, 8, 8);
    recentLay->setSpacing(6);
    auto* recentTitle = new QLabel("Recent", recentCard);
    recentLay->addWidget(recentTitle);
    recentList_ = new QListWidget(recentCard);
    recentList_->setSelectionMode(QAbstractItemView::SingleSelection);
    recentLay->addWidget(recentList_, 1);
    recentCard->setMaximumWidth(620);

    auto* recentRow = new QHBoxLayout();
    recentRow->addStretch();
    recentRow->addWidget(recentCard);
    recentRow->addStretch();
    bodyLay->addLayout(recentRow);

    bodyLay->addSpacing(12);

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

    if (recentList_) {
        connect(recentList_, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
            if (!it) return;
            const QString path = it->data(Qt::UserRole).toString();
            if (path.isEmpty() || !QFileInfo::exists(path))
                return;
            startParseAsync(path);
        });
        connect(recentList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
            if (!it) return;
            const QString path = it->data(Qt::UserRole).toString();
            if (path.isEmpty() || !QFileInfo::exists(path))
                return;
            startParseAsync(path);
        });
    }
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
    mainSplitter_ = splitter;

    auto* treeCard = new QFrame(splitter);
    treeCard->setObjectName("Card");
    auto* treeLay = new QVBoxLayout(treeCard);
    treeLay->setContentsMargins(8, 8, 8, 8);
    treeLay->setSpacing(6);

    tree_ = new QTreeView(treeCard);
    treeLay->addWidget(tree_, 1);

    auto* rightSplitter = new QSplitter(Qt::Vertical, splitter);
    rightSplitter_ = rightSplitter;

    auto* detailsCard = new QFrame(rightSplitter);
    detailsCard->setObjectName("Card");
    auto* detailsLay = new QVBoxLayout(detailsCard);
    detailsLay->setContentsMargins(8, 8, 8, 8);
    detailsLay->setSpacing(6);

    auto* detailsButtons = new QHBoxLayout();
    detailsButtons->setContentsMargins(0, 0, 0, 0);
    detailsButtons->setSpacing(6);

    copyRvaBtn_ = new QPushButton("Copy RVA", detailsCard);
    copyOffBtn_ = new QPushButton("Copy Offset", detailsCard);
    copyVaBtn_  = new QPushButton("Copy VA", detailsCard);
    copySnippetBtn_ = new QPushButton("Copy Snippet", detailsCard);

    exportJsonBtn_ = new QPushButton("Export JSON", detailsCard);
    exportCsvBtn_  = new QPushButton("Export CSV", detailsCard);

    detailsButtons->addWidget(copyRvaBtn_);
    detailsButtons->addWidget(copyOffBtn_);
    detailsButtons->addWidget(copyVaBtn_);
    detailsButtons->addWidget(copySnippetBtn_);
    detailsButtons->addStretch(1);
    detailsButtons->addWidget(exportJsonBtn_);
    detailsButtons->addWidget(exportCsvBtn_);
    detailsLay->addLayout(detailsButtons);

    detailsTabs_ = new QTabWidget(detailsCard);
    detailsSummary_ = new QPlainTextEdit(detailsTabs_);
    detailsRaw_ = new QPlainTextEdit(detailsTabs_);
    detailsSummary_->setReadOnly(true);
    detailsRaw_->setReadOnly(true);
    detailsTabs_->addTab(detailsSummary_, "Summary");
    detailsTabs_->addTab(detailsRaw_, "Raw");
    detailsLay->addWidget(detailsTabs_, 1);

    copyRvaBtn_->setEnabled(false);
    copyOffBtn_->setEnabled(false);
    copyVaBtn_->setEnabled(false);
    copySnippetBtn_->setEnabled(false);
    exportJsonBtn_->setEnabled(false);
    exportCsvBtn_->setEnabled(false);

    connect(copyRvaBtn_, &QPushButton::clicked, this, [this]() {
        copyTextToClipboard(selectedRva_, "Copied RVA: " + selectedRva_);
    });
    connect(copyOffBtn_, &QPushButton::clicked, this, [this]() {
        copyTextToClipboard(selectedOffset_, "Copied Offset: " + selectedOffset_);
    });
    connect(copyVaBtn_, &QPushButton::clicked, this, [this]() {
        copyTextToClipboard(selectedVa_, "Copied VA: " + selectedVa_);
    });
    connect(copySnippetBtn_, &QPushButton::clicked, this, [this]() {
        copyTextToClipboard(selectedSnippet_, "Copied Snippet");
    });

    auto exportJson = [this]() {
        if (selectedTypeIndex_ < 0 || (size_t)selectedTypeIndex_ >= types_.size())
            return;

        const QString path = QFileDialog::getSaveFileName(
            this, "Export JSON", {}, "JSON (*.json);;All files (*.*)");
        if (path.isEmpty())
            return;

        const auto& t = types_[(size_t)selectedTypeIndex_];
        auto hex = [](qulonglong v) { return QString("0x%1").arg(QString::number(v, 16)); };

        QJsonObject typeObj;
        typeObj["assembly"] = QString::fromStdString(t.assembly);
        typeObj["namespace"] = QString::fromStdString(t.nameSpace);
        typeObj["className"] = QString::fromStdString(t.name);
        typeObj["typeDefIndex"] = t.typeDefIndex;

        auto memberToJson = [&](const DumpMember& m) {
            QJsonObject o;
            o["kind"] = kindToString(m.kind);
            o["name"] = QString::fromStdString(m.name);
            o["signature"] = QString::fromStdString(m.signature);
            o["rva"] = hex((qulonglong)m.rva);
            o["offset"] = hex((qulonglong)m.offset);
            o["va"] = hex((qulonglong)m.va);
            return o;
        };

        if (selectedMemberIndex_ >= 0 && (size_t)selectedMemberIndex_ < t.members.size()) {
            const auto& m = t.members[(size_t)selectedMemberIndex_];
            typeObj.insert("member", QJsonValue(memberToJson(m)));
        } else {
            QJsonArray arr;
            for (const auto& m : t.members)
                arr.push_back(memberToJson(m));
            typeObj.insert("members", QJsonValue(arr));
        }

        if (selectedSnippet_.isEmpty() == false)
            typeObj.insert("snippet", QJsonValue(selectedSnippet_));

        const QJsonDocument doc(typeObj);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return;
        f.write(doc.toJson(QJsonDocument::Indented));
        f.close();
        statusBar()->showMessage("Exported JSON: " + path, 2500);
    };

    auto exportCsv = [this]() {
        if (selectedTypeIndex_ < 0 || (size_t)selectedTypeIndex_ >= types_.size())
            return;

        const QString path = QFileDialog::getSaveFileName(
            this, "Export CSV", {}, "CSV (*.csv);;All files (*.*)");
        if (path.isEmpty())
            return;

        const auto& t = types_[(size_t)selectedTypeIndex_];
        auto hex = [](qulonglong v) { return QString("0x%1").arg(QString::number(v, 16)); };
        auto esc = [](QString s) {
            s.replace('"', "\"\"");
            return '"' + s + '"';
        };

        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return;
        QTextStream out(&f);
        out << "assembly,namespace,className,typeDefIndex,memberKind,memberName,signature,rva,offset,va\n";

        auto writeMember = [&](const DumpMember& m) {
            out
                << esc(QString::fromStdString(t.assembly)) << ','
                << esc(QString::fromStdString(t.nameSpace)) << ','
                << esc(QString::fromStdString(t.name)) << ','
                << t.typeDefIndex << ','
                << esc(kindToString(m.kind)) << ','
                << esc(QString::fromStdString(m.name)) << ','
                << esc(QString::fromStdString(m.signature)) << ','
                << esc(hex((qulonglong)m.rva)) << ','
                << esc(hex((qulonglong)m.offset)) << ','
                << esc(hex((qulonglong)m.va))
                << "\n";
        };

        if (selectedMemberIndex_ >= 0 && (size_t)selectedMemberIndex_ < t.members.size()) {
            writeMember(t.members[(size_t)selectedMemberIndex_]);
        } else {
            for (const auto& m : t.members)
                writeMember(m);
        }

        f.close();
        statusBar()->showMessage("Exported CSV: " + path, 2500);
    };

    connect(exportJsonBtn_, &QPushButton::clicked, this, exportJson);
    connect(exportCsvBtn_, &QPushButton::clicked, this, exportCsv);

    auto* favoritesCard = new QFrame(rightSplitter);
    favoritesCard->setObjectName("Card");
    auto* favoritesLay = new QVBoxLayout(favoritesCard);
    favoritesLay->setContentsMargins(8, 8, 8, 8);
    favoritesLay->setSpacing(6);

    auto* favHeader = new QHBoxLayout();
    favHeader->setContentsMargins(0, 0, 0, 0);
    favHeader->setSpacing(6);
    auto* favTitle = new QLabel("Favorites", favoritesCard);
    favoritesCount_ = new QLabel("0", favoritesCard);
    favHeader->addWidget(favTitle);
    favHeader->addStretch(1);
    favHeader->addWidget(favoritesCount_);
    favoritesLay->addLayout(favHeader);

    favoritesList_ = new QListWidget(favoritesCard);
    favoritesList_->setUniformItemSizes(true);
    favoritesLay->addWidget(favoritesList_, 1);

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

    auto* resultsSearchRow = new QHBoxLayout();
    resultsSearchRow->setContentsMargins(0, 0, 0, 0);
    resultsSearchRow->setSpacing(6);

    resultsScope_ = new QComboBox(resultsCard);
    resultsScope_->addItem("All");
    resultsScope_->addItem("Selected Assembly");
    resultsScope_->addItem("Selected Namespace");
    resultsScope_->addItem("Selected Type");
    resultsScope_->setCurrentIndex(0);

    resultsSearchRow->addWidget(resultsSearch_, 1);
    resultsSearchRow->addWidget(resultsScope_);
    resultsLay->addLayout(resultsSearchRow);

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
    resultsList_->setItemDelegate(new ResultsQueryHighlightDelegate(resultsSearch_, resultsList_));
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
    rightSplitter->addWidget(favoritesCard);
    rightSplitter->addWidget(resultsCard);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 4);
    splitter->setHandleWidth(10);
    splitter->setSizes({800, 520});
    rightSplitter->setStretchFactor(0, 3);
    rightSplitter->setStretchFactor(1, 1);
    rightSplitter->setStretchFactor(2, 2);
    contentLay->addWidget(splitter, 1);

    connect(tree_->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            [this](const QModelIndex& proxyIdx, const QModelIndex&) {
                if (!proxyIdx.isValid()) {
                    updateDetailsPanel({});
                    return;
                }
                const QModelIndex srcIdx = proxy_->mapToSource(proxyIdx);
                updateDetailsPanel(srcIdx);
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

    if (!resultsFilterTimer_) {
        resultsFilterTimer_ = new QTimer(this);
        resultsFilterTimer_->setSingleShot(true);
        connect(resultsFilterTimer_, &QTimer::timeout, this, [this]() {
            updateSearchResults();
        });
    }

    connect(resultsSearch_, &QLineEdit::textChanged, this, [this](const QString&) {
        if (resultsList_)
            resultsList_->viewport()->update();
        if (!resultsFilterTimer_)
            return;
        resultsFilterTimer_->start(250);
    });
    if (resultsScope_) {
        connect(resultsScope_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
            updateSearchResults();
        });
    }
    connect(resultsList_, &QListWidget::itemActivated, this, &MainWindow::navigateToSearchResult);
    connect(resultsList_, &QListWidget::itemClicked, this, &MainWindow::navigateToSearchResult);

    if (favoritesList_) {
        auto navFav = [this](QListWidgetItem* item) {
            if (!item)
                return;
            const int kind = item->data(Qt::UserRole + 1).toInt();
            const QString asmName = item->data(Qt::UserRole + 2).toString();
            const QString nsName = item->data(Qt::UserRole + 3).toString();
            const int typeIndex = item->data(Qt::UserRole + 4).toInt();
            const int memberIndex = item->data(Qt::UserRole + 5).toInt();
            const MemberKind mk = (MemberKind)item->data(Qt::UserRole + 6).toInt();

            QStandardItem* target = nullptr;
            if (kind == 0) {
                const std::string key = asmName.toStdString();
                auto it = asmItems_.find(key);
                if (it != asmItems_.end())
                    target = it->second;
            } else if (kind == 1) {
                const std::string key = asmName.toStdString() + "|" + nsName.toStdString();
                auto it = nsItems_.find(key);
                if (it != nsItems_.end())
                    target = it->second;
            } else if (kind == 2) {
                navigateToTypeOrMember(typeIndex, -1, MemberKind::Method);
                return;
            } else if (kind == 3) {
                navigateToTypeOrMember(typeIndex, memberIndex, mk);
                return;
            }

            if (!target)
                return;

            auto expandProxyAncestors = [this](QModelIndex pidx) {
                for (QModelIndex cur = pidx; cur.isValid(); cur = cur.parent())
                    tree_->expand(cur);
            };

            const QModelIndex srcIdx = target->index();
            const QModelIndex proxyIdx = proxy_->mapFromSource(srcIdx);
            if (!proxyIdx.isValid())
                return;

            expandProxyAncestors(proxyIdx.parent());
            tree_->setCurrentIndex(proxyIdx);
            tree_->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
        };
        connect(favoritesList_, &QListWidget::itemActivated, this, navFav);
        connect(favoritesList_, &QListWidget::itemClicked, this, navFav);
    }

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

    const bool hasMemberIndex = srcIdx.data(Qt::UserRole + 10).isValid();
    const int memberIndex = srcIdx.data(Qt::UserRole + 10).toInt();
    const int typeIndex = srcIdx.data(Qt::UserRole + 11).toInt();
    const int memberKind = srcIdx.data(Qt::UserRole + 12).toInt();

    const QString typeTag = srcIdx.data(Qt::UserRole + 2).toString();
    const bool isType = (typeTag == "Type");
    const bool isGroup = srcIdx.data(Qt::UserRole + 3).isValid();

    QString asmName;
    QString nsName;
    if (hasMemberIndex || isType) {
        if (typeIndex >= 0 && (size_t)typeIndex < types_.size()) {
            const auto& t = types_[(size_t)typeIndex];
            asmName = QString::fromStdString(t.assembly.empty() ? std::string("(unknown)") : t.assembly);
            nsName = QString::fromStdString(t.nameSpace);
        }
    } else {
        const QString parentAsm = srcIdx.parent().data(Qt::UserRole + 100).toString();
        if (!parentAsm.isEmpty()) {
            asmName = parentAsm;
            nsName = srcIdx.data(Qt::UserRole + 100).toString();
        } else {
            asmName = srcIdx.data(Qt::UserRole + 100).toString();
        }
    }

    int favKind = -1;
    QString favKey;
    QString favDisplay;
    if (hasMemberIndex) {
        favKind = 3;
        favKey = QString("mem|%1|%2|%3").arg(typeIndex).arg(memberIndex).arg(memberKind);
        if (typeIndex >= 0 && (size_t)typeIndex < types_.size() && memberIndex >= 0 && (size_t)memberIndex < types_[(size_t)typeIndex].members.size()) {
            const auto& t = types_[(size_t)typeIndex];
            const auto& m = t.members[(size_t)memberIndex];
            favDisplay = QString::fromStdString(t.assembly + " :: " + t.nameSpace + "::" + t.name + "  " + m.signature);
        }
    } else if (isType) {
        favKind = 2;
        favKey = QString("type|%1").arg(typeIndex);
        if (typeIndex >= 0 && (size_t)typeIndex < types_.size()) {
            const auto& t = types_[(size_t)typeIndex];
            favDisplay = QString::fromStdString(t.assembly + " :: " + t.nameSpace + "::" + t.name);
        }
    } else if (!asmName.isEmpty() && !nsName.isEmpty() && !isGroup && srcIdx.parent().isValid()) {
        favKind = 1;
        favKey = QString("ns|%1|%2").arg(asmName, nsName);
        favDisplay = asmName + " :: " + nsName;
    } else if (!asmName.isEmpty() && !isGroup && srcIdx.isValid()) {
        favKind = 0;
        favKey = QString("asm|%1").arg(asmName);
        favDisplay = asmName;
    }

    QMenu menu(this);

    if (!favKey.isEmpty() && favKind >= 0) {
        if (!favoriteKeys_.contains(favKey)) {
            menu.addAction("Add to Favorites", this, [this, favKey, favKind, favDisplay, asmName, nsName, typeIndex, memberIndex, memberKind]() {
                if (favoriteKeys_.contains(favKey))
                    return;
                favoriteKeys_.insert(favKey);
                if (favoritesList_) {
                    auto* it = new QListWidgetItem(favDisplay.isEmpty() ? QString("(favorite)") : favDisplay, favoritesList_);
                    it->setData(Qt::UserRole, favKey);
                    it->setData(Qt::UserRole + 1, favKind);
                    it->setData(Qt::UserRole + 2, asmName);
                    it->setData(Qt::UserRole + 3, nsName);
                    it->setData(Qt::UserRole + 4, typeIndex);
                    it->setData(Qt::UserRole + 5, memberIndex);
                    it->setData(Qt::UserRole + 6, memberKind);
                }
                if (favoritesCount_)
                    favoritesCount_->setText(QString::number(favoritesList_ ? favoritesList_->count() : favoriteKeys_.size()));
            });
        } else {
            menu.addAction("Remove from Favorites", this, [this, favKey]() {
                favoriteKeys_.remove(favKey);
                if (favoritesList_) {
                    for (int i = favoritesList_->count() - 1; i >= 0; --i) {
                        auto* it = favoritesList_->item(i);
                        if (it && it->data(Qt::UserRole).toString() == favKey) {
                            delete favoritesList_->takeItem(i);
                            break;
                        }
                    }
                }
                if (favoritesCount_)
                    favoritesCount_->setText(QString::number(favoritesList_ ? favoritesList_->count() : favoriteKeys_.size()));
            });
        }
        menu.addSeparator();
    }

    if (!sig.isEmpty())
        menu.addAction("Copy Signature", this, [this, sig]() { copyTextToClipboard(sig, "Copied Signature"); });

    if (!rva.isEmpty() && rva != "0x0")
        menu.addAction("Copy RVA", this, [this, rva]() { copyTextToClipboard(rva, "Copied RVA: " + rva); });

    if (!off.isEmpty() && off != "0x0")
        menu.addAction("Copy Offset", this, [this, off]() { copyTextToClipboard(off, "Copied Offset: " + off); });

    if (!va.isEmpty() && va != "0x0")
        menu.addAction("Copy VA", this, [this, va]() { copyTextToClipboard(va, "Copied VA: " + va); });

    if (hasMemberIndex && typeIndex >= 0 && memberIndex >= 0 &&
        (memberKind == (int)MemberKind::Method || memberKind == (int)MemberKind::Ctor)) {
        QMenu* gen = menu.addMenu("Generate Snippet");

        const QString defName = snippetDefaultTemplateName_;
        QStringList names;
        if (!defName.isEmpty() && snippetTemplates_.contains(defName))
            names.push_back(defName);
        for (const auto& k : snippetTemplates_.keys()) {
            if (k != defName)
                names.push_back(k);
        }

        for (const auto& name : names) {
            QString label = name;
            if (name == defName)
                label += " (Default)";
            gen->addAction(label, this, [this, typeIndex, memberIndex, name]() {
                showSnippetDialog(typeIndex, memberIndex, name);
            });
        }

        gen->addSeparator();
        gen->addAction("Manage Templates...", this, [this, typeIndex, memberIndex]() {
            showSnippetDialog(typeIndex, memberIndex);
        });
    }

    if (menu.actions().isEmpty()) return;
    menu.exec(tree_->viewport()->mapToGlobal(p));
}

void MainWindow::showSnippetDialog(int typeIndex, int memberIndex, const QString& templateName) {
    if (typeIndex < 0 || (size_t)typeIndex >= types_.size())
        return;
    const auto& t = types_[(size_t)typeIndex];
    if (memberIndex < 0 || (size_t)memberIndex >= t.members.size())
        return;

    const auto& m = t.members[(size_t)memberIndex];
    if (!(m.kind == MemberKind::Method || m.kind == MemberKind::Ctor))
        return;

    const QString clazz = QString::fromStdString(t.name);
    const QString method = QString::fromStdString(m.name);
    const int argc = countParamsTopLevel(m.params);

    QString assembly = QString::fromStdString(t.assembly);
    if (assembly.isEmpty())
        assembly = "(unknown)";

    QString ns = QString::fromStdString(t.nameSpace);
    const QString nsForSnippet = (ns == "-") ? QString() : ns;

    QMap<QString, QString> vars;
    vars.insert("${assemblyName}", assembly);
    vars.insert("${assembly}", assembly);
    vars.insert("${namespace}", nsForSnippet);
    vars.insert("${className}", clazz);
    vars.insert("${methodName}", method);
    vars.insert("${parameterCount}", QString::number(argc));

    QString currentTemplateName = templateName;
    if (currentTemplateName.isEmpty())
        currentTemplateName = snippetDefaultTemplateName_;
    if (currentTemplateName.isEmpty() || !snippetTemplates_.contains(currentTemplateName))
        currentTemplateName = snippetTemplates_.isEmpty() ? QString() : snippetTemplates_.firstKey();

    auto buildSnippet = [this, vars](const QString& tplName) {
        QString tpl = snippetTemplates_.value(tplName);
        if (tpl.isEmpty())
            tpl = defaultSnippetTemplateBnm();
        return applySnippetTemplate(tpl, vars);
    };

    QString snippet = buildSnippet(currentTemplateName);

    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(QString("Snippet: %1::%2").arg(clazz, method));
    dlg->resize(760, 260);

    auto* lay = new QVBoxLayout(dlg);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    auto* topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(8);

    auto* tplLabel = new QLabel("Template:", dlg);
    auto* tplCombo = new QComboBox(dlg);
    auto* manageBtn = new QPushButton("Manage...", dlg);

    topRow->addWidget(tplLabel);
    topRow->addWidget(tplCombo, 1);
    topRow->addWidget(manageBtn);
    lay->addLayout(topRow);

    auto refreshTemplateCombo = [this, tplCombo, &currentTemplateName]() {
        const QString selected = currentTemplateName;
        tplCombo->clear();
        QStringList names;
        const QString defName = snippetDefaultTemplateName_;
        if (!defName.isEmpty() && snippetTemplates_.contains(defName))
            names.push_back(defName);
        for (const auto& k : snippetTemplates_.keys()) {
            if (k != defName)
                names.push_back(k);
        }
        for (const auto& n : names) {
            QString label = n;
            if (n == defName)
                label += " (Default)";
            tplCombo->addItem(label, n);
        }
        int idx = tplCombo->findData(selected);
        if (idx < 0)
            idx = tplCombo->findData(snippetDefaultTemplateName_);
        if (idx < 0 && tplCombo->count() > 0)
            idx = 0;
        if (idx >= 0)
            tplCombo->setCurrentIndex(idx);
        currentTemplateName = tplCombo->currentData().toString();
    };

    auto* edit = new QPlainTextEdit(dlg);
    edit->setReadOnly(true);
    edit->setPlainText(snippet);
    lay->addWidget(edit, 1);

    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);

    auto* copyBtn = new QPushButton("Copy", dlg);
    auto* closeBtn = new QPushButton("Close", dlg);
    btnRow->addStretch(1);
    btnRow->addWidget(copyBtn);
    btnRow->addWidget(closeBtn);
    lay->addLayout(btnRow);

    refreshTemplateCombo();

    connect(tplCombo, &QComboBox::currentIndexChanged, dlg, [tplCombo, edit, buildSnippet, &currentTemplateName](int) {
        currentTemplateName = tplCombo->currentData().toString();
        edit->setPlainText(buildSnippet(currentTemplateName));
    });

    connect(manageBtn, &QPushButton::clicked, dlg, [this, tplCombo, edit, buildSnippet, refreshTemplateCombo, vars]() {
        QDialog tdlg(this);
        tdlg.setWindowTitle("Snippet Templates");
        tdlg.resize(980, 640);

        QMap<QString, QString> workingTemplates = snippetTemplates_;
        QString workingDefault = snippetDefaultTemplateName_;

        auto* tl = new QVBoxLayout(&tdlg);
        tl->setContentsMargins(12, 12, 12, 12);
        tl->setSpacing(10);

        auto* body = new QHBoxLayout();
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(10);
        tl->addLayout(body, 1);

        auto* left = new QVBoxLayout();
        left->setContentsMargins(0, 0, 0, 0);
        left->setSpacing(8);
        body->addLayout(left, 0);

        auto* list = new QListWidget(&tdlg);
        list->setMinimumWidth(240);
        left->addWidget(list, 1);

        auto* leftBtns = new QHBoxLayout();
        leftBtns->setContentsMargins(0, 0, 0, 0);
        auto* addBtn = new QPushButton("Add", &tdlg);
        auto* dupBtn = new QPushButton("Duplicate", &tdlg);
        auto* delBtn = new QPushButton("Delete", &tdlg);
        leftBtns->addWidget(addBtn);
        leftBtns->addWidget(dupBtn);
        leftBtns->addWidget(delBtn);
        left->addLayout(leftBtns);

        auto* setDefaultBtn = new QPushButton("Set as Default", &tdlg);
        left->addWidget(setDefaultBtn);

        auto* right = new QVBoxLayout();
        right->setContentsMargins(0, 0, 0, 0);
        right->setSpacing(8);
        body->addLayout(right, 1);

        auto* nameRow = new QHBoxLayout();
        nameRow->setContentsMargins(0, 0, 0, 0);
        auto* nameLbl = new QLabel("Name:", &tdlg);
        auto* nameEdit = new QLineEdit(&tdlg);
        nameRow->addWidget(nameLbl);
        nameRow->addWidget(nameEdit, 1);
        right->addLayout(nameRow);

        auto* info = new QLabel(
            "Available variables:\n"
            "  ${assemblyName} (or ${assembly})\n"
            "  ${namespace}\n"
            "  ${className}\n"
            "  ${methodName}\n"
            "  ${parameterCount}",
            &tdlg);
        info->setTextInteractionFlags(Qt::TextSelectableByMouse);
        right->addWidget(info);

        auto* tplEdit = new QPlainTextEdit(&tdlg);
        right->addWidget(tplEdit, 1);

        auto* warn = new QLabel(&tdlg);
        right->addWidget(warn);

        auto* previewLbl = new QLabel("Preview:", &tdlg);
        right->addWidget(previewLbl);

        auto* preview = new QPlainTextEdit(&tdlg);
        preview->setReadOnly(true);
        preview->setMinimumHeight(150);
        right->addWidget(preview);

        auto* bottom = new QHBoxLayout();
        bottom->setContentsMargins(0, 0, 0, 0);
        auto* resetBnm = new QPushButton("Reset Selected to BNM Default", &tdlg);
        auto* resetFrida = new QPushButton("Reset Selected to Frida Default", &tdlg);
        auto* saveBtn = new QPushButton("Save", &tdlg);
        auto* cancelBtn = new QPushButton("Cancel", &tdlg);
        bottom->addWidget(resetBnm);
        bottom->addWidget(resetFrida);
        bottom->addStretch(1);
        bottom->addWidget(saveBtn);
        bottom->addWidget(cancelBtn);
        tl->addLayout(bottom);

        auto refreshList = [&]() {
            list->clear();
            QStringList names = workingTemplates.keys();
            for (const auto& n : names) {
                QString label = n;
                if (n == workingDefault)
                    label += " (Default)";
                auto* it = new QListWidgetItem(label, list);
                it->setData(Qt::UserRole, n);
            }
        };

        auto currentKey = [&]() -> QString {
            auto* it = list->currentItem();
            return it ? it->data(Qt::UserRole).toString() : QString();
        };

        auto renameKey = [&](const QString& oldKey, const QString& newKey) -> bool {
            const QString src = oldKey.trimmed();
            const QString dst = newKey.trimmed();
            if (src.isEmpty() || dst.isEmpty() || src == dst)
                return false;
            if (!workingTemplates.contains(src) || workingTemplates.contains(dst))
                return false;

            const QString tpl = workingTemplates.take(src);
            workingTemplates.insert(dst, tpl);
            if (workingDefault == src)
                workingDefault = dst;
            return true;
        };

        auto setWarnFromTemplate = [&]() {
            const QString k = currentKey();
            const QString tpl = tplEdit->toPlainText();
            const auto bad = validateSnippetPlaceholders(tpl);
            if (!bad.isEmpty()) {
                warn->setText("Unknown variables: " + bad.join(", "));
                warn->setStyleSheet("QLabel { color: #ff8080; }");
            } else {
                warn->setText(QString());
                warn->setStyleSheet(QString());
            }
            preview->setPlainText(applySnippetTemplate(tpl, vars));
            if (!k.isEmpty())
                workingTemplates[k] = tpl;
        };

        auto loadSelected = [&]() {
            const QString k = currentKey();
            if (k.isEmpty()) {
                nameEdit->setText(QString());
                tplEdit->setPlainText(QString());
                preview->setPlainText(QString());
                warn->setText(QString());
                return;
            }
            nameEdit->setText(k);
            tplEdit->setPlainText(workingTemplates.value(k));
            setWarnFromTemplate();
        };

        refreshList();
        if (list->count() > 0)
            list->setCurrentRow(0);

        connect(list, &QListWidget::currentItemChanged, &tdlg, [&](QListWidgetItem*, QListWidgetItem*) {
            loadSelected();
        });

        connect(tplEdit, &QPlainTextEdit::textChanged, &tdlg, [&]() {
            setWarnFromTemplate();
        });

        connect(nameEdit, &QLineEdit::editingFinished, &tdlg, [&]() {
            const QString oldKey = currentKey();
            const QString desired = nameEdit->text().trimmed();
            if (oldKey.isEmpty())
                return;
            if (desired.isEmpty()) {
                nameEdit->setText(oldKey);
                return;
            }
            if (desired == oldKey)
                return;
            if (workingTemplates.contains(desired)) {
                QMessageBox::warning(&tdlg, "Rename", "A template with that name already exists.");
                nameEdit->setText(oldKey);
                return;
            }

            if (!renameKey(oldKey, desired)) {
                nameEdit->setText(oldKey);
                return;
            }

            refreshList();
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == desired) {
                    list->setCurrentRow(i);
                    break;
                }
            }
        });

        connect(addBtn, &QPushButton::clicked, &tdlg, [&]() {
            const QString newName = uniqueTemplateName(workingTemplates, "New Template");
            workingTemplates.insert(newName, defaultSnippetTemplateBnm());
            refreshList();
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == newName) {
                    list->setCurrentRow(i);
                    break;
                }
            }
        });

        connect(dupBtn, &QPushButton::clicked, &tdlg, [&]() {
            const QString k = currentKey();
            if (k.isEmpty())
                return;
            const QString newName = uniqueTemplateName(workingTemplates, k);
            workingTemplates.insert(newName, workingTemplates.value(k));
            refreshList();
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == newName) {
                    list->setCurrentRow(i);
                    break;
                }
            }
        });

        connect(delBtn, &QPushButton::clicked, &tdlg, [&]() {
            const QString k = currentKey();
            if (k.isEmpty())
                return;
            if (workingTemplates.size() <= 1) {
                QMessageBox::information(&tdlg, "Delete", "You must keep at least one template.");
                return;
            }
            workingTemplates.remove(k);
            if (workingDefault == k)
                workingDefault = workingTemplates.firstKey();
            refreshList();
            if (list->count() > 0)
                list->setCurrentRow(0);
        });

        connect(setDefaultBtn, &QPushButton::clicked, &tdlg, [&]() {
            const QString k = currentKey();
            if (k.isEmpty())
                return;
            workingDefault = k;
            refreshList();
            for (int i = 0; i < list->count(); ++i) {
                if (list->item(i)->data(Qt::UserRole).toString() == k) {
                    list->setCurrentRow(i);
                    break;
                }
            }
        });

        connect(resetBnm, &QPushButton::clicked, &tdlg, [&]() {
            if (currentKey().isEmpty())
                return;
            tplEdit->setPlainText(defaultSnippetTemplateBnm());
        });

        connect(resetFrida, &QPushButton::clicked, &tdlg, [&]() {
            if (currentKey().isEmpty())
                return;
            tplEdit->setPlainText(defaultSnippetTemplateFrida());
        });

        connect(cancelBtn, &QPushButton::clicked, &tdlg, &QDialog::reject);

        connect(saveBtn, &QPushButton::clicked, &tdlg, [&]() {
            QMap<QString, QString> newTemplates;
            for (int i = 0; i < list->count(); ++i) {
                const QString key = list->item(i)->data(Qt::UserRole).toString();
                if (!key.isEmpty())
                    newTemplates.insert(key, workingTemplates.value(key));
            }

            if (newTemplates.isEmpty()) {
                QMessageBox::warning(&tdlg, "Save", "No templates to save.");
                return;
            }

            if (workingDefault.isEmpty() || !newTemplates.contains(workingDefault))
                workingDefault = newTemplates.firstKey();

            for (auto it = newTemplates.begin(); it != newTemplates.end(); ++it) {
                if (it.key().trimmed().isEmpty()) {
                    QMessageBox::warning(&tdlg, "Save", "Template names cannot be empty.");
                    return;
                }
            }

            snippetTemplates_ = newTemplates;
            snippetDefaultTemplateName_ = workingDefault;

            QSettings s(snippetSettingsPath(), QSettings::IniFormat);
            s.beginGroup("Snippets");
            s.setValue("DefaultTemplate", snippetDefaultTemplateName_);
            s.remove("Template");
            s.beginGroup("Templates");
            s.remove("");
            for (auto it = snippetTemplates_.begin(); it != snippetTemplates_.end(); ++it)
                s.setValue(it.key(), it.value());
            s.endGroup();
            s.endGroup();

            tdlg.accept();
        });

        loadSelected();

        if (tdlg.exec() == QDialog::Accepted) {
            refreshTemplateCombo();
            edit->setPlainText(buildSnippet(tplCombo->currentData().toString()));
        }
    });

    connect(copyBtn, &QPushButton::clicked, dlg, [this, edit]() {
        copyTextToClipboard(edit->toPlainText(), "Copied Snippet");
    });
    connect(closeBtn, &QPushButton::clicked, dlg, &QDialog::accept);

    dlg->show();
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

        struct Entry {
            QString exactKey;
            QString looseKey;
            QString assembly;
            QString typeFqn;
            MemberKind kind = MemberKind::Method;
            QString name;
            int paramCount = 0;
            QString signature;
            QString sigKey;
            quint64 offset = 0;
            quint64 rva = 0;
            quint64 va = 0;
        };

        struct Maps {
            QHash<QString, Entry> exact;
            QMultiHash<QString, QString> looseToExact;
        };

        const auto buildMaps = [](const std::vector<DumpType>& types) {
            Maps out;
            for (int ti = 0; ti < (int)types.size(); ++ti) {
                const auto& t = types[(size_t)ti];
                const QString asmName = QString::fromStdString(t.assembly.empty() ? std::string("(unknown)") : t.assembly);
                const QString typeFqn = QString::fromStdString(t.nameSpace + "::" + t.name);
                for (int mi = 0; mi < (int)t.members.size(); ++mi) {
                    const auto& m = t.members[(size_t)mi];
                    Entry e;
                    e.assembly = asmName;
                    e.typeFqn = typeFqn;
                    e.kind = m.kind;
                    e.name = QString::fromStdString(m.name);
                    e.paramCount = countParamsTopLevel(m.params);
                    e.signature = QString::fromStdString(m.signature);
                    e.sigKey = normalizeSignature(m);
                    e.offset = (quint64)m.offset;
                    e.rva = (quint64)m.rva;
                    e.va = (quint64)m.va;
                    e.exactKey = asmName + "|" + typeFqn + "|" + kindToString(m.kind) + "|" + e.sigKey;
                    e.looseKey = asmName + "|" + typeFqn + "|" + kindToString(m.kind) + "|" + e.name + "|" + QString::number(e.paramCount);

                    out.exact.insert(e.exactKey, e);
                    out.looseToExact.insert(e.looseKey, e.exactKey);
                }
            }
            return out;
        };

        const auto oldMaps = buildMaps(baseTypes);
        const auto newMaps = buildMaps(newTypes);

        auto hex = [](qulonglong v) { return QString("0x%1").arg(QString::number(v, 16)); };

        auto anyNonZeroOffset = [](const std::vector<DumpType>& types) {
            for (const auto& t : types) {
                for (const auto& m : t.members) {
                    if (m.offset != 0)
                        return true;
                }
            }
            return false;
        };

        const bool baseHasOffsets = anyNonZeroOffset(baseTypes);
        const bool newHasOffsets = anyNonZeroOffset(newTypes);
        const bool showRvaVaCols = !(baseHasOffsets && newHasOffsets);

        struct Row {
            QString status;
            QString assembly;
            QString typeFqn;
            MemberKind kind = MemberKind::Method;
            QString item;
            QString oldOff, newOff;
            QString oldRva, newRva;
            QString oldVa, newVa;
            QString oldSig, newSig;
        };

        QVector<Row> rows;
        rows.reserve(oldMaps.exact.size() + newMaps.exact.size());

        QSet<QString> pairedNewExact;

        int changed = 0, added = 0, removed = 0, sigChanged = 0;

        for (auto it = oldMaps.exact.begin(); it != oldMaps.exact.end(); ++it) {
            const auto jt = newMaps.exact.find(it.key());
            if (jt != newMaps.exact.end()) {
                const bool offDiff = jt.value().offset != it.value().offset;
                const bool rvaDiff = jt.value().rva != it.value().rva;
                const bool vaDiff  = jt.value().va  != it.value().va;

                const bool haveOldOff = (it.value().offset != 0);
                const bool haveNewOff = (jt.value().offset != 0);
                const bool haveBothOff = haveOldOff && haveNewOff;

                bool isChanged = false;
                if (haveBothOff) {
                    isChanged = offDiff;
                } else {
                    isChanged = offDiff || rvaDiff || vaDiff;
                }

                if (isChanged) {
                    ++changed;
                    Row r;
                    r.status = "Changed";
                    r.assembly = it.value().assembly;
                    r.typeFqn = it.value().typeFqn;
                    r.kind = it.value().kind;
                    r.item = kindToString(it.value().kind) + "  " + it.value().signature;
                    r.oldOff = hex(it.value().offset);
                    r.newOff = hex(jt.value().offset);
                    r.oldRva = hex(it.value().rva);
                    r.newRva = hex(jt.value().rva);
                    r.oldVa  = hex(it.value().va);
                    r.newVa  = hex(jt.value().va);
                    rows.push_back(r);
                }
            }
        }

        for (auto it = oldMaps.exact.begin(); it != oldMaps.exact.end(); ++it) {
            if (newMaps.exact.contains(it.key()))
                continue;

            const Entry& oldE = it.value();
            if (oldE.signature.isEmpty())
                continue;

            if (oldE.kind == MemberKind::Method || oldE.kind == MemberKind::Ctor) {
                const auto candidates = newMaps.looseToExact.values(oldE.looseKey);
                QString best;
                for (const auto& candKey : candidates) {
                    if (oldMaps.exact.contains(candKey))
                        continue;
                    if (pairedNewExact.contains(candKey))
                        continue;
                    const auto candIt = newMaps.exact.find(candKey);
                    if (candIt == newMaps.exact.end())
                        continue;
                    if (candIt.value().sigKey == oldE.sigKey)
                        continue;
                    best = candKey;
                    break;
                }

                if (!best.isEmpty()) {
                    const Entry& newE = newMaps.exact.value(best);
                    pairedNewExact.insert(best);
                    ++sigChanged;

                    Row r;
                    r.status = "Sig Changed";
                    r.assembly = oldE.assembly;
                    r.typeFqn = oldE.typeFqn;
                    r.kind = oldE.kind;
                    r.item = kindToString(oldE.kind) + "  " + oldE.name;
                    r.oldOff = hex(oldE.offset);
                    r.newOff = hex(newE.offset);
                    r.oldRva = hex(oldE.rva);
                    r.newRva = hex(newE.rva);
                    r.oldVa  = hex(oldE.va);
                    r.newVa  = hex(newE.va);
                    r.oldSig = oldE.signature;
                    r.newSig = newE.signature;
                    rows.push_back(r);
                    continue;
                }
            }

            ++removed;
            Row r;
            r.status = "Removed";
            r.assembly = oldE.assembly;
            r.typeFqn = oldE.typeFqn;
            r.kind = oldE.kind;
            r.item = kindToString(oldE.kind) + "  " + oldE.signature;
            r.oldOff = hex(oldE.offset);
            r.oldRva = hex(oldE.rva);
            r.oldVa  = hex(oldE.va);
            rows.push_back(r);
        }

        for (auto it = newMaps.exact.begin(); it != newMaps.exact.end(); ++it) {
            if (oldMaps.exact.contains(it.key()))
                continue;
            if (pairedNewExact.contains(it.key()))
                continue;
            ++added;
            const Entry& newE = it.value();
            Row r;
            r.status = "Added";
            r.assembly = newE.assembly;
            r.typeFqn = newE.typeFqn;
            r.kind = newE.kind;
            r.item = kindToString(newE.kind) + "  " + newE.signature;
            r.newOff = hex(newE.offset);
            r.newRva = hex(newE.rva);
            r.newVa  = hex(newE.va);
            rows.push_back(r);
        }

        auto* dlg = new QDialog(this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->setWindowTitle(QString("Diff (%1 changed, %2 added, %3 removed, %4 sig)").arg(changed).arg(added).arg(removed).arg(sigChanged));
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
        auto* cbSig = new QCheckBox("Sig Changed", dlg);
        cbAdded->setChecked(true);
        cbChanged->setChecked(true);
        cbRemoved->setChecked(true);
        cbSig->setChecked(true);

        statusRow->addWidget(cbAdded);
        statusRow->addWidget(cbChanged);
        statusRow->addWidget(cbRemoved);
        statusRow->addWidget(cbSig);
        statusRow->addStretch(1);

        auto* exportJsonBtn = new QPushButton("Export JSON", dlg);
        auto* exportCsvBtn  = new QPushButton("Export CSV", dlg);
        statusRow->addWidget(exportJsonBtn);
        statusRow->addWidget(exportCsvBtn);
        lay->addLayout(statusRow);

        class DiffFilterProxy final : public QSortFilterProxyModel {
        public:
            explicit DiffFilterProxy(QObject* parent = nullptr)
                : QSortFilterProxyModel(parent) {}

            QSet<QString> allowedStatuses;
            QString text;

        protected:
            bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override {
                if (!sourceModel())
                    return true;

                const QModelIndex idx0 = sourceModel()->index(source_row, 0, source_parent);
                const bool hasChildren = sourceModel()->rowCount(idx0) > 0;

                const bool filteringByStatus = !allowedStatuses.isEmpty();
                const bool filteringByText = !text.trimmed().isEmpty();
                bool selfOk = true;

                const QString st = sourceModel()->index(source_row, 1, source_parent).data().toString();
                if (!st.isEmpty() && !allowedStatuses.isEmpty()) {
                    if (!allowedStatuses.contains(st))
                        selfOk = false;
                }

                if (selfOk && !text.trimmed().isEmpty()) {
                    QString blob;
                    const int cols = sourceModel()->columnCount(source_parent);
                    for (int c = 0; c < cols; ++c) {
                        blob += sourceModel()->index(source_row, c, source_parent).data().toString();
                        blob += "\n";
                    }
                    if (!blob.toLower().contains(text.trimmed().toLower()))
                        selfOk = false;
                }

                if (hasChildren) {
                    if (!filteringByStatus && !filteringByText)
                        return true;

                    const int childCount = sourceModel()->rowCount(idx0);
                    for (int i = 0; i < childCount; ++i) {
                        if (filterAcceptsRow(i, idx0))
                            return true;
                    }

                    if (filteringByText && selfOk)
                        return true;

                    return false;
                }

                return selfOk;
            }
        };

        auto* model = new QStandardItemModel(dlg);
        model->setHorizontalHeaderLabels({
            "Item", "Status",
            "Old Offset", "New Offset",
            "Old RVA", "New RVA",
            "Old VA", "New VA",
            "Old Sig", "New Sig",
            "Assembly", "Type"
        });

        auto statusStyle = [](QStandardItem* statusItem, const QString& status) {
            QColor statusColor(215, 246, 241);
            QColor statusBg(11, 42, 45);
            if (status == "Added") { statusColor = QColor(34, 255, 102); statusBg = QColor(15, 60, 40); }
            else if (status == "Removed") { statusColor = QColor(255, 110, 110); statusBg = QColor(60, 20, 20); }
            else if (status == "Changed") { statusColor = QColor(255, 200, 90); statusBg = QColor(55, 45, 20); }
            else if (status == "Sig Changed") { statusColor = QColor(120, 200, 255); statusBg = QColor(20, 35, 60); }
            statusItem->setForeground(QBrush(statusColor));
            statusItem->setBackground(QBrush(statusBg));
        };

        QMap<QString, QStandardItem*> asmNodes;
        QMap<QString, QStandardItem*> typeNodes;

        for (const auto& r : rows) {
            const QString asmKey = r.assembly;
            QStandardItem* asmItem = asmNodes.value(asmKey, nullptr);
            if (!asmItem) {
                QList<QStandardItem*> rowItems;
                rowItems << new QStandardItem(asmKey);
                rowItems << new QStandardItem();
                for (int c = 2; c < model->columnCount(); ++c)
                    rowItems << new QStandardItem();
                asmItem = rowItems[0];
                asmItem->setIcon(icoFolder_);
                model->appendRow(rowItems);
                asmNodes.insert(asmKey, asmItem);
            }

            const QString typeKey = asmKey + "|" + r.typeFqn;
            QStandardItem* typeItem = typeNodes.value(typeKey, nullptr);
            if (!typeItem) {
                QList<QStandardItem*> rowItems;
                rowItems << new QStandardItem(r.typeFqn);
                rowItems << new QStandardItem();
                for (int c = 2; c < model->columnCount(); ++c)
                    rowItems << new QStandardItem();
                typeItem = rowItems[0];
                typeItem->setIcon(icoClass_);
                asmItem->appendRow(rowItems);
                typeNodes.insert(typeKey, typeItem);
            }

            QList<QStandardItem*> leaf;
            auto* item0 = new QStandardItem(r.item);
            const QIcon childIcon =
                (r.kind == MemberKind::Ctor)     ? icoCtor_ :
                (r.kind == MemberKind::Method)   ? icoMethod_ :
                (r.kind == MemberKind::Field)    ? icoField_ :
                (r.kind == MemberKind::Property) ? icoProperty_ :
                (r.kind == MemberKind::Event)    ? icoEvent_ :
                (r.kind == MemberKind::EnumValue)? icoEnumValue_ :
                icoClass_;
            item0->setIcon(childIcon);
            leaf << item0;

            auto* stItem = new QStandardItem(r.status);
            statusStyle(stItem, r.status);
            leaf << stItem;

            leaf << new QStandardItem(r.oldOff);
            leaf << new QStandardItem(r.newOff);
            leaf << new QStandardItem(r.oldRva);
            leaf << new QStandardItem(r.newRva);
            leaf << new QStandardItem(r.oldVa);
            leaf << new QStandardItem(r.newVa);
            leaf << new QStandardItem(r.oldSig);
            leaf << new QStandardItem(r.newSig);
            leaf << new QStandardItem(r.assembly);
            leaf << new QStandardItem(r.typeFqn);

            typeItem->appendRow(leaf);
        }

        auto* proxy = new DiffFilterProxy(dlg);
        proxy->setSourceModel(model);
        proxy->setDynamicSortFilter(true);

        auto* view = new QTreeView(dlg);
        view->setModel(proxy);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->setSelectionMode(QAbstractItemView::SingleSelection);
        view->setSortingEnabled(true);
        view->setRootIsDecorated(true);
        view->setItemsExpandable(true);
        view->setUniformRowHeights(true);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->setItemDelegateForColumn(0, new SignatureHighlightDelegate(view));

        view->header()->setStretchLastSection(false);
        view->header()->setSectionResizeMode(QHeaderView::Interactive);
        view->setColumnWidth(0, 700);
        view->setColumnWidth(1, 110);
        view->setColumnWidth(2, 110);
        view->setColumnWidth(3, 110);
        view->setColumnWidth(4, 110);
        view->setColumnWidth(5, 110);
        view->setColumnWidth(6, 140);
        view->setColumnWidth(7, 140);
        view->setColumnWidth(8, 260);
        view->setColumnWidth(9, 260);
        view->setColumnHidden(10, true);
        view->setColumnHidden(11, true);
        view->setColumnHidden(4, !showRvaVaCols);
        view->setColumnHidden(5, !showRvaVaCols);
        view->setColumnHidden(6, !showRvaVaCols);
        view->setColumnHidden(7, !showRvaVaCols);
        lay->addWidget(view, 1);

        auto updateFilters = [proxy, cbAdded, cbChanged, cbRemoved, cbSig, filter]() {
            QSet<QString> sts;
            if (cbAdded->isChecked()) sts.insert("Added");
            if (cbChanged->isChecked()) sts.insert("Changed");
            if (cbRemoved->isChecked()) sts.insert("Removed");
            if (cbSig->isChecked()) sts.insert("Sig Changed");
            proxy->allowedStatuses = sts;
            proxy->text = filter->text();
            proxy->invalidate();
        };

        connect(cbAdded, &QCheckBox::toggled, dlg, [updateFilters](bool) { updateFilters(); });
        connect(cbChanged, &QCheckBox::toggled, dlg, [updateFilters](bool) { updateFilters(); });
        connect(cbRemoved, &QCheckBox::toggled, dlg, [updateFilters](bool) { updateFilters(); });
        connect(cbSig, &QCheckBox::toggled, dlg, [updateFilters](bool) { updateFilters(); });
        connect(filter, &QLineEdit::textChanged, dlg, [updateFilters](const QString&) { updateFilters(); });
        updateFilters();

        auto collectVisibleLeaves = [proxy](auto&& self, const QModelIndex& parent) -> QVector<Row> {
            QVector<Row> out;
            const int rc = proxy->rowCount(parent);
            for (int r = 0; r < rc; ++r) {
                const QModelIndex idx0 = proxy->index(r, 0, parent);
                if (!idx0.isValid())
                    continue;
                if (proxy->rowCount(idx0) > 0) {
                    out += self(self, idx0);
                    continue;
                }

                Row row;
                row.item = proxy->index(r, 0, parent).data().toString();
                row.status = proxy->index(r, 1, parent).data().toString();
                row.oldOff = proxy->index(r, 2, parent).data().toString();
                row.newOff = proxy->index(r, 3, parent).data().toString();
                row.oldRva = proxy->index(r, 4, parent).data().toString();
                row.newRva = proxy->index(r, 5, parent).data().toString();
                row.oldVa  = proxy->index(r, 6, parent).data().toString();
                row.newVa  = proxy->index(r, 7, parent).data().toString();
                row.oldSig = proxy->index(r, 8, parent).data().toString();
                row.newSig = proxy->index(r, 9, parent).data().toString();
                row.assembly = proxy->index(r, 10, parent).data().toString();
                row.typeFqn  = proxy->index(r, 11, parent).data().toString();
                out.push_back(row);
            }
            return out;
        };

        connect(exportJsonBtn, &QPushButton::clicked, dlg, [dlg, collectVisibleLeaves]() {
            const QString path = QFileDialog::getSaveFileName(dlg, "Export Diff (JSON)", {}, "JSON (*.json);;All files (*.*)");
            if (path.isEmpty())
                return;

            const QVector<Row> rows = collectVisibleLeaves(collectVisibleLeaves, {});
            QJsonArray arr;
            for (const auto& r : rows) {
                QJsonObject o;
                o.insert("status", r.status);
                o.insert("assembly", r.assembly);
                o.insert("type", r.typeFqn);
                o.insert("item", r.item);
                o.insert("oldOffset", r.oldOff);
                o.insert("newOffset", r.newOff);
                o.insert("oldRva", r.oldRva);
                o.insert("newRva", r.newRva);
                o.insert("oldVa", r.oldVa);
                o.insert("newVa", r.newVa);
                if (!r.oldSig.isEmpty() || !r.newSig.isEmpty()) {
                    o.insert("oldSignature", r.oldSig);
                    o.insert("newSignature", r.newSig);
                }
                arr.append(o);
            }
            QJsonObject root;
            root.insert("diff", QJsonValue(arr));

            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                return;
            f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
            f.close();
        });

        connect(exportCsvBtn, &QPushButton::clicked, dlg, [dlg, collectVisibleLeaves]() {
            const QString path = QFileDialog::getSaveFileName(dlg, "Export Diff (CSV)", {}, "CSV (*.csv);;All files (*.*)");
            if (path.isEmpty())
                return;

            const QVector<Row> rows = collectVisibleLeaves(collectVisibleLeaves, {});
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
                return;
            QTextStream ts(&f);
            ts << "Status,Assembly,Type,Item,Old Offset,New Offset,Old RVA,New RVA,Old VA,New VA,Old Sig,New Sig\n";

            auto esc = [](QString s) {
                if (s.contains('"')) s.replace("\"", "\"\"");
                const bool needs = s.contains(',') || s.contains('\n') || s.contains('\r') || s.contains('"');
                if (needs) s = '"' + s + '"';
                return s;
            };

            for (const auto& r : rows) {
                ts << esc(r.status) << ','
                   << esc(r.assembly) << ','
                   << esc(r.typeFqn) << ','
                   << esc(r.item) << ','
                   << esc(r.oldOff) << ','
                   << esc(r.newOff) << ','
                   << esc(r.oldRva) << ','
                   << esc(r.newRva) << ','
                   << esc(r.oldVa) << ','
                   << esc(r.newVa) << ','
                   << esc(r.oldSig) << ','
                   << esc(r.newSig) << "\n";
            }
            f.close();
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

    addRecentFile(path);
    refreshRecentUi();

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

    favoriteKeys_.clear();
    if (favoritesList_) favoritesList_->clear();
    if (favoritesCount_) favoritesCount_->setText("0");

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
    asmItems_.clear();
    typeItems_.assign(types_.size(), nullptr);
    asmRootItem_ = nullptr;

    auto* root = model_->invisibleRootItem();
    auto* asmRoot = new QStandardItem("Assemblies");
    asmRoot->setIcon(icoFolder_);
    asmRoot->setData("Assemblies", Qt::UserRole + 100);
    root->appendRow(asmRoot);
    asmRootItem_ = asmRoot;

    std::map<std::string, std::map<std::string, std::vector<int>>> asmNsMap;
    for (size_t i = 0; i < types_.size(); ++i) {
        const std::string asmName = types_[i].assembly.empty() ? std::string("(unknown)") : types_[i].assembly;
        asmNsMap[asmName][types_[i].nameSpace].push_back((int)i);
    }

    for (auto& [asmName, nsMap] : asmNsMap) {
        auto* asmItem = new QStandardItem(QString::fromStdString(asmName));
        asmItem->setIcon(icoFolder_);
        asmItem->setData(QString::fromStdString(asmName), Qt::UserRole + 100);
        asmRoot->appendRow(asmItem);
        asmItems_[asmName] = asmItem;

        for (auto& [ns, indices] : nsMap) {
            auto* nsItem = new QStandardItem(QString::fromStdString(ns));
            nsItem->setIcon(icoNamespace_);
            nsItem->setData(QString::fromStdString(ns), Qt::UserRole + 100);
            asmItem->appendRow(nsItem);

            nsItems_[asmName + "|" + ns] = nsItem;

            for (int idx : indices) {
                const bool isEnum = types_[idx].isEnum;
                auto* typeItem = new QStandardItem(QString::fromStdString(types_[idx].name));
                typeItem->setIcon(isEnum ? icoEnumType_ : icoClass_);
                typeItem->setData(isEnum ? QString("#A06EFF") : QString("#50A0FF"), Qt::UserRole + 200);
                typeItem->setForeground(isEnum ? QBrush(QColor(160, 110, 255)) : QBrush(QColor(80, 160, 255)));
                typeItem->setData(idx, Qt::UserRole + 1);
                typeItem->setData("Type", Qt::UserRole + 2);
                typeItem->setData(QString::fromStdString(types_[idx].assembly + " :: " + types_[idx].nameSpace + "::" + types_[idx].name), Qt::UserRole + 100);
                typeItem->appendRow(new QStandardItem("Loading..."));
                nsItem->appendRow(typeItem);

                if ((size_t)idx < typeItems_.size())
                    typeItems_[(size_t)idx] = typeItem;
            }
        }
    }

    tree_->expand(proxy_->mapFromSource(asmRoot->index()));
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
        it->setData(typeIndex, Qt::UserRole + 11);
        it->setData((int)m.kind, Qt::UserRole + 12);

        groupItem->appendRow(it);
    }

    if (!any && groupItem->rowCount() == 0) {
        auto* none = new QStandardItem("(none)");
        none->setEnabled(false);
        groupItem->appendRow(none);
    }
}
