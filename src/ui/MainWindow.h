#pragma once

#include <QMainWindow>
#include <QIcon>
#include <QMap>
#include <QSet>
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
class QTimer;
class QCheckBox;
class QTabWidget;
class QComboBox;
class QSplitter;
class QCloseEvent;

template <typename T> class QFutureWatcher;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void openDumpCs();
    void compareDumpCs();
    void onTreeExpanded(const QModelIndex& srcIdx);

private:
    void buildUi();
    QWidget* buildWelcomePage();
    QWidget* buildExplorerPage();
    QWidget* buildHeaderBar(QWidget* parent, bool showOpenButton);

    void addRecentFile(const QString& path);
    void refreshRecentUi();
    void restoreUiState();
    void saveUiState();

    void populateTree();
    void buildTypeChildren(QStandardItem* typeItem, int typeIndex);
    void buildGroupChildren(QStandardItem* groupItem);
    void setupInteractions();
    void showContextMenu(const QPoint& p);
    void showSnippetDialog(int typeIndex, int memberIndex, const QString& templateName = QString());
    void updateDetailsPanel(const QModelIndex& srcIdx);
    QString buildSnippetText(int typeIndex, int memberIndex, const QString& templateName = QString()) const;
    void navigateToTypeOrMember(int typeIndex, int memberIndex, MemberKind memberKind);
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
    QListWidget* recentList_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLineEdit* search_ = nullptr;
    QTabWidget* detailsTabs_ = nullptr;
    QPlainTextEdit* detailsSummary_ = nullptr;
    QPlainTextEdit* detailsRaw_ = nullptr;
    class QPushButton* copyRvaBtn_ = nullptr;
    class QPushButton* copyOffBtn_ = nullptr;
    class QPushButton* copyVaBtn_ = nullptr;
    class QPushButton* copySnippetBtn_ = nullptr;
    class QPushButton* exportJsonBtn_ = nullptr;
    class QPushButton* exportCsvBtn_ = nullptr;
    QLineEdit* resultsSearch_ = nullptr;
    QComboBox* resultsScope_ = nullptr;
    QListWidget* favoritesList_ = nullptr;
    QLabel* favoritesCount_ = nullptr;
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
    QString parseError_;
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
        QString assembly;
        QString ns;
        int typeIndex = -1;
        int memberIndex = -1;
        MemberKind memberKind = MemberKind::Method;
    };

    std::vector<SearchEntry> searchIndex_;
    QStandardItem* asmRootItem_ = nullptr;
    std::vector<QStandardItem*> typeItems_;
    std::map<std::string, QStandardItem*> asmItems_;
    std::map<std::string, QStandardItem*> nsItems_;

    QMap<QString, QString> snippetTemplates_;
    QString snippetDefaultTemplateName_;

    QFutureWatcher<QVector<int>>* resultsFilterWatcher_ = nullptr;
    int resultsFilterRequestId_ = 0;

    QSet<QString> favoriteKeys_;

    int selectedTypeIndex_ = -1;
    int selectedMemberIndex_ = -1;
    MemberKind selectedMemberKind_ = MemberKind::Method;

    QString selectedSignature_;
    QString selectedRva_;
    QString selectedOffset_;
    QString selectedVa_;
    QString selectedFqn_;
    QString selectedSnippet_;

    QTimer* resultsFilterTimer_ = nullptr;

    QSplitter* mainSplitter_ = nullptr;
    QSplitter* rightSplitter_ = nullptr;
};
