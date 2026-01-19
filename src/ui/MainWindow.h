#pragma once

#include <QMainWindow>
#include <QIcon>
#include <vector>
#include <map>

#include "parser/DumpCsParser.h"

class QStackedWidget;
class QTreeView;
class QPlainTextEdit;
class QStandardItemModel;
class QModelIndex;
class QWidget;
class QStandardItem;
class QLineEdit;
class QSortFilterProxyModel;
class QProgressBar;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QCheckBox;

template <typename T> class QFutureWatcher;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void openDumpCs();
    void compareDumpCs();
    void onTreeExpanded(const QModelIndex& srcIdx);

private:
    void buildUi();
    QWidget* buildWelcomePage();
    QWidget* buildExplorerPage();
    QWidget* buildHeaderBar(QWidget* parent, bool showOpenButton);

    void populateTree();
    void buildTypeChildren(QStandardItem* typeItem, int typeIndex);
    void buildGroupChildren(QStandardItem* groupItem);
    void setupInteractions();
    void showContextMenu(const QPoint& p);
    void copyTextToClipboard(const QString& text, const QString& statusMsg);
    void startParseAsync(const QString& path);
    void finishParseAsync();
    void setBusy(bool busy, const QString& msg = {});
    void buildSearchIndex();
    void updateSearchResults();
    void navigateToSearchResult(QListWidgetItem* item);
    QStackedWidget* stack_ = nullptr;
    QWidget* welcomePage_ = nullptr;
    QWidget* explorerPage_ = nullptr;
    class QPushButton* openBtn_ = nullptr;
    class QPushButton* compareBtn_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QLineEdit* resultsSearch_ = nullptr;
    QListWidget* resultsList_ = nullptr;
    QLabel* resultsCount_ = nullptr;
    QCheckBox* filterNs_ = nullptr;
    QCheckBox* filterType_ = nullptr;
    QCheckBox* filterMethod_ = nullptr;
    QCheckBox* filterCtor_ = nullptr;
    QCheckBox* filterField_ = nullptr;
    QCheckBox* filterProperty_ = nullptr;
    QCheckBox* filterEvent_ = nullptr;
    QCheckBox* filterEnumValue_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
    QWidget* busyRow_ = nullptr;
    QLabel* busyLabel_ = nullptr;
    QProgressBar* busyBar_ = nullptr;
    QFutureWatcher<std::vector<DumpType>>* watcher_ = nullptr;
    QString parsePath_;
    bool hasLoadedPrimary_ = false;

    QIcon icoNamespace_;
    QIcon icoClass_;
    QIcon icoEnumType_;
    QIcon icoMethod_;
    QIcon icoCtor_;
    QIcon icoField_;
    QIcon icoProperty_;
    QIcon icoEvent_;
    QIcon icoEnumValue_;
    QIcon icoFolder_;

    std::vector<DumpType> types_;

    struct SearchEntry {
        enum class Kind { Namespace, Type, Member } kind;
        QString display;
        QString detail;
        QString ns;
        int typeIndex = -1;
        int memberIndex = -1;
        MemberKind memberKind = MemberKind::Method;
    };

    std::vector<SearchEntry> searchIndex_;
    QStandardItem* nsRootItem_ = nullptr;
    std::vector<QStandardItem*> typeItems_;
    std::map<std::string, QStandardItem*> nsItems_;
};
