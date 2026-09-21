#pragma once

#include "payload_controller.h"

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>
#include <unordered_map>

class RuntimeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tools READ tools NOTIFY toolsChanged)
    Q_PROPERTY(QString lastResult READ lastResult NOTIFY resultChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY resultChanged)
    Q_PROPERTY(int primitiveCount READ primitiveCount NOTIFY toolsChanged)
    Q_PROPERTY(int semanticCount READ semanticCount NOTIFY toolsChanged)

public:
    RuntimeController(PayloadController& payload,
                      std::function<bool()> mutationAllowed,
                      QObject* parent = nullptr);

    const QVariantList& tools() const { return tools_; }
    QString lastResult() const { return lastResult_; }
    QString lastError() const { return lastError_; }
    int primitiveCount() const { return primitiveCount_; }
    int semanticCount() const { return semanticCount_; }

    Q_INVOKABLE bool refreshTools();
    Q_INVOKABLE bool callToolJson(const QString& name, const QString& argumentsJson);
    Q_INVOKABLE void clearResult();
    Q_INVOKABLE void reset();

signals:
    void toolsChanged();
    void resultChanged();

private:
    struct ToolMeta {
        bool semantic = false;
        bool mutationRequired = false;
    };

    void setError(const QString& error);

    PayloadController& payload_;
    std::function<bool()> mutationAllowed_;
    QVariantList tools_;
    std::unordered_map<std::string, ToolMeta> toolMeta_;
    QString lastResult_;
    QString lastError_;
    int primitiveCount_ = 0;
    int semanticCount_ = 0;
};
