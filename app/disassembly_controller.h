#pragma once

#include "services/disassembly_service.h"
#include "payload_controller.h"
#include "target/session_manager.h"

#include <QObject>
#include <QVariantList>
#include <QStringList>

class DisassemblyController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList rows READ rows NOTIFY rowsChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY historyChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY historyChanged)
    Q_PROPERTY(QString currentAddress READ currentAddress NOTIFY historyChanged)
    Q_PROPERTY(QString analysisResult READ analysisResult NOTIFY analysisChanged)
    Q_PROPERTY(QString analysisError READ analysisError NOTIFY analysisChanged)
    Q_PROPERTY(QString analysisKind READ analysisKind NOTIFY analysisChanged)

public:
    DisassemblyController(cortex::target::SessionManager& sessions, PayloadController& payload, QObject* parent = nullptr);

    const QVariantList& rows() const { return rows_; }
    QString lastError() const { return lastError_; }
    bool canGoBack() const { return historyIndex_ > 0; }
    bool canGoForward() const { return historyIndex_ >= 0 && historyIndex_ + 1 < history_.size(); }
    QString currentAddress() const { return historyIndex_ >= 0 && historyIndex_ < history_.size() ? history_.at(historyIndex_) : QString(); }
    QString analysisResult() const { return analysisResult_; }
    QString analysisError() const { return analysisError_; }
    QString analysisKind() const { return analysisKind_; }

    Q_INVOKABLE bool disassemble(const QString& address, int count = 128);
    Q_INVOKABLE bool goBack();
    Q_INVOKABLE bool goForward();
    Q_INVOKABLE bool analyzeCfg(const QString& address);
    Q_INVOKABLE bool analyzeXrefs(const QString& address, bool includeData = true);
    Q_INVOKABLE bool analyzeStructure(const QString& address);
    Q_INVOKABLE void clearAnalysis();
    Q_INVOKABLE void clear();

signals:
    void rowsChanged();
    void lastErrorChanged();
    void historyChanged();
    void analysisChanged();

private:
    void setLastError(const QString& error);
    bool decode(const QString& address, int count, bool recordHistory);
    bool analyze(const std::string& tool, const nlohmann::json& arguments, const QString& kind);

    cortex::services::DisassemblyService service_;
    PayloadController& payload_;
    QVariantList rows_;
    QString lastError_;
    QStringList history_;
    int historyIndex_ = -1;
    int lastCount_ = 128;
    QString analysisResult_;
    QString analysisError_;
    QString analysisKind_;
};
