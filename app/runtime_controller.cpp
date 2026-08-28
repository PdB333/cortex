#include "runtime_controller.h"

#include "api/mcp_contract.h"
#include "api/semantic_tools.h"

#include <QVariantMap>

namespace {

using json = nlohmann::json;

QString FromUtf8(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

json RouteResult(const json& output) {
    if (!output.is_object()) return output;
    auto it = output.find("result");
    return it != output.end() ? *it : output;
}

QString Pretty(const json& value) {
    return FromUtf8(value.dump(2));
}

} // namespace

RuntimeController::RuntimeController(PayloadController& payload,
                                     std::function<bool()> mutationAllowed,
                                     QObject* parent)
    : QObject(parent),
      payload_(payload),
      mutationAllowed_(std::move(mutationAllowed)) {}

bool RuntimeController::refreshTools() {
    nlohmann::json output;
    QString error;
    if (!payload_.CallTool("tools", nlohmann::json::object(), output, &error)) {
        setError(error);
        return false;
    }

    const json manifest = RouteResult(output);
    if (!manifest.is_array()) {
        setError(QStringLiteral("runtime_tool_manifest_invalid"));
        return false;
    }

    tools_.clear();
    toolMeta_.clear();
    primitiveCount_ = 0;
    semanticCount_ = 0;

    for (const auto& entry : manifest) {
        if (!entry.is_object()) continue;
        const std::string name = entry.value("name", std::string());
        if (name.empty() || name == "mcp") continue;
        const std::string method = entry.value("method", std::string("GET"));
        const std::string path = entry.value("path", std::string());
        const auto risk = api::mcp_contract::ClassifyTool(name, method, path);
        const bool mutation = api::mcp_contract::RequiresMutationPermission(risk);

        QVariantMap row;
        row.insert(QStringLiteral("name"), FromUtf8(name));
        row.insert(QStringLiteral("description"), FromUtf8(entry.value("description", std::string())));
        row.insert(QStringLiteral("method"), FromUtf8(method));
        row.insert(QStringLiteral("path"), FromUtf8(path));
        row.insert(QStringLiteral("risk"), QString::fromLatin1(api::mcp_contract::RiskName(risk)));
        row.insert(QStringLiteral("mutationRequired"), mutation);
        row.insert(QStringLiteral("semantic"), false);
        row.insert(QStringLiteral("argumentTemplate"), QStringLiteral("{}"));

        json hints = json::object();
        if (entry.contains("body")) hints["body"] = entry["body"];
        if (entry.contains("query")) hints["_query"] = entry["query"];
        if (path.find('{') != std::string::npos) hints["_path"] = "required path substitutions";
        row.insert(QStringLiteral("hint"), hints.empty() ? QString() : Pretty(hints));

        tools_.push_back(row);
        toolMeta_[name] = ToolMeta{false, mutation};
        ++primitiveCount_;
    }

    for (const auto& entry : api::semantic::Catalog()) {
        if (!entry.is_object()) continue;
        const std::string name = entry.value("name", std::string());
        if (name.empty()) continue;

        QVariantMap row;
        row.insert(QStringLiteral("name"), FromUtf8(name));
        row.insert(QStringLiteral("description"), FromUtf8(entry.value("description", std::string())));
        row.insert(QStringLiteral("method"), QStringLiteral("MCP"));
        row.insert(QStringLiteral("path"), QString());
        row.insert(QStringLiteral("risk"), QStringLiteral("semantic"));
        row.insert(QStringLiteral("mutationRequired"), false);
        row.insert(QStringLiteral("semantic"), true);
        row.insert(QStringLiteral("argumentTemplate"), QStringLiteral("{\n  \"objective\": \"\"\n}"));
        row.insert(QStringLiteral("hint"), entry.contains("inputSchema") ? Pretty(entry["inputSchema"]) : QString());
        tools_.push_back(row);
        toolMeta_[name] = ToolMeta{true, false};
        ++semanticCount_;
    }

    emit toolsChanged();
    setError(QString());
    return true;
}

bool RuntimeController::callToolJson(const QString& nameValue, const QString& argumentsJson) {
    const std::string name = nameValue.trimmed().toStdString();
    if (name.empty()) {
        setError(QStringLiteral("missing_tool_name"));
        return false;
    }

    auto metaIt = toolMeta_.find(name);
    if (metaIt == toolMeta_.end()) {
        if (!refreshTools()) return false;
        metaIt = toolMeta_.find(name);
        if (metaIt == toolMeta_.end()) {
            setError(QStringLiteral("unknown_tool"));
            return false;
        }
    }

    json arguments;
    try {
        const std::string raw = argumentsJson.trimmed().isEmpty()
            ? std::string("{}")
            : argumentsJson.toStdString();
        arguments = json::parse(raw);
    } catch (const std::exception& exception) {
        setError(QStringLiteral("invalid_json: %1").arg(QString::fromUtf8(exception.what())));
        return false;
    }
    if (!arguments.is_object()) {
        setError(QStringLiteral("arguments_must_be_object"));
        return false;
    }

    const bool mutationAllowed = mutationAllowed_ && mutationAllowed_();
    if (metaIt->second.mutationRequired && !mutationAllowed) {
        setError(QStringLiteral("mutation_permission_required"));
        return false;
    }
    if (metaIt->second.semantic && arguments.value("mutation_permission", false) && !mutationAllowed) {
        setError(QStringLiteral("mutation_permission_required"));
        return false;
    }

    json output;
    QString error;
    const bool ok = payload_.CallTool(name, arguments, output, &error);
    lastResult_ = output.is_null() ? QString() : Pretty(output);
    emit resultChanged();
    if (!ok) {
        setError(error.isEmpty() ? QStringLiteral("tool_call_failed") : error);
        return false;
    }
    setError(QString());
    return true;
}

void RuntimeController::clearResult() {
    if (lastResult_.isEmpty() && lastError_.isEmpty()) return;
    lastResult_.clear();
    lastError_.clear();
    emit resultChanged();
}

void RuntimeController::reset() {
    tools_.clear();
    toolMeta_.clear();
    primitiveCount_ = 0;
    semanticCount_ = 0;
    lastResult_.clear();
    lastError_.clear();
    emit toolsChanged();
    emit resultChanged();
}

void RuntimeController::setError(const QString& error) {
    if (lastError_ == error) return;
    lastError_ = error;
    emit resultChanged();
}
