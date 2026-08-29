#include "re_controller.h"
#include <QJsonDocument>
#include <QJsonParseError>

namespace {
using json=nlohmann::json;
json Result(const json& out){if(out.is_object()){auto it=out.find("result");if(it!=out.end())return *it;}return out;}
QVariant ToVariant(const json& value){QJsonParseError error{};auto doc=QJsonDocument::fromJson(QByteArray::fromStdString(value.dump()),&error);if(error.error!=QJsonParseError::NoError)return {};return doc.toVariant();}
json ParseJson(const QString& text,bool& ok){try{ok=true;return json::parse(text.toUtf8().constData());}catch(...){ok=false;return json();}}
json AddressValue(const QString& text){QString v=text.trimmed();bool ok=false;qulonglong n=v.toULongLong(&ok,0);if(ok)return static_cast<uint64_t>(n);return v.toStdString();}
}

ReController::ReController(PayloadController& payload,std::function<bool()> mutationAllowed,QObject* parent)
    :QObject(parent),payload_(payload),mutationAllowed_(std::move(mutationAllowed)){}

void ReController::fail(const QString& error){lastError_=error;emit changed();}
void ReController::setResult(const json& result){result_=QString::fromUtf8(result.dump(2).c_str());lastError_.clear();emit changed();}

bool ReController::call(const std::string& tool,json args,json& result,bool mutation){
    if(mutation){if(!mutationAllowed_||!mutationAllowed_()){fail(QStringLiteral("Enable Mutation before this RE operation."));return false;}args["mutation_permission"]=true;}
    QString error;json out;if(!payload_.CallTool(tool,args,out,&error)){fail(error);return false;}result=Result(out);if(result.is_object()&&result.contains("ok")&&!result.value("ok",true)){result_=QString::fromUtf8(result.dump(2).c_str());fail(QString::fromUtf8(result.value("error",std::string("operation_failed")).c_str()));return false;}return true;
}

void ReController::reset(){tracks_.clear();selectedTrack_.clear();trackEvents_.clear();session_.clear();sessions_.clear();checkpoints_.clear();result_.clear();lastError_.clear();emit changed();}

bool ReController::refresh(){json r;if(!call("re_object_tracks",json::object(),r))return false;tracks_=ToVariant(r.value("tracks",json::array())).toList();json s;if(call("re_session",json::object(),s))session_=ToVariant(s).toMap();emit changed();return true;}
bool ReController::refreshSessions(){json r;if(!call("session_list",json::object(),r))return false;sessions_=ToVariant(r.value("sessions",json::array())).toList();emit changed();return true;}
bool ReController::refreshCheckpoints(){json r;if(!call("re_checkpoint_list",json::object(),r))return false;checkpoints_=ToVariant(r.value("checkpoints",json::array())).toList();emit changed();return true;}
bool ReController::selectTrack(int id){json r;if(!call("re_object_get",{{"_path",{{"id",id}}}},r))return false;selectedTrack_=ToVariant(r).toMap();json e;if(call("re_object_events",{{"_path",{{"id",id}}}},e))trackEvents_=ToVariant(e.value("events",json::array())).toList();setResult(r);return true;}

bool ReController::trackObject(const QString& name,const QString& address,const QString& pointerPath,int size,bool persist,const QString& structName){json args{{"name",name.toStdString()},{"size",size},{"persist",persist}};if(!pointerPath.trimmed().isEmpty())args["pointer_path"]=pointerPath.trimmed().toStdString();else args["address"]=AddressValue(address);if(!structName.trimmed().isEmpty())args["struct_name"]=structName.trimmed().toStdString();json r;if(!call("re_track_object",args,r,true))return false;setResult(r);refresh();return true;}
bool ReController::deleteTrack(int id){json r;if(!call("re_object_delete",{{"_path",{{"id",id}}}},r,true))return false;selectedTrack_.clear();trackEvents_.clear();setResult(r);refresh();return true;}
bool ReController::findLastWriter(const QString& address,int size,int timeoutMs){json r;if(!call("re_find_last_writer",{{"address",AddressValue(address)},{"size",size},{"timeout_ms",timeoutMs}},r,true))return false;setResult(r);return true;}
bool ReController::detectSubobjects(const QString& address,int size){json r;if(!call("re_cpp_subobjects",{{"address",AddressValue(address)},{"size",size}},r))return false;setResult(r);return true;}

bool ReController::traceTransition(const QString& jsonText){bool ok=false;json args=ParseJson(jsonText,ok);if(!ok||!args.is_object()){fail(QStringLiteral("Transition JSON must be an object."));return false;}json r;if(!call("re_trace_transition",args,r,true))return false;setResult(r);return true;}
bool ReController::runTest(const QString& jsonText,bool experiment){bool ok=false;json args=ParseJson(jsonText,ok);if(!ok||!args.is_object()){fail(QStringLiteral("Test JSON must be an object."));return false;}json r;if(!call(experiment?"re_experiment_run":"re_test_run",args,r,true))return false;setResult(r);return true;}
bool ReController::createCheckpoint(const QString& label,const QString& rangesJson){bool ok=false;json ranges=ParseJson(rangesJson.trimmed().isEmpty()?QStringLiteral("[]"):rangesJson,ok);if(!ok||!ranges.is_array()){fail(QStringLiteral("Checkpoint ranges must be a JSON array."));return false;}json r;if(!call("re_checkpoint_create",{{"label",label.toStdString()},{"ranges",ranges}},r,true))return false;setResult(r);refreshCheckpoints();return true;}
bool ReController::rollbackCheckpoint(int id,bool keep){if(id<=0)return false;json r;if(!call("re_checkpoint_rollback",{{"_path",{{"id",id}}},{"keep",keep}},r,true))return false;setResult(r);refreshCheckpoints();refresh();return true;}
bool ReController::deleteCheckpoint(int id){if(id<=0)return false;json r;if(!call("re_checkpoint_delete",{{"_path",{{"id",id}}}},r,true))return false;setResult(r);refreshCheckpoints();return true;}
bool ReController::saveFact(const QString& key,const QString& valueJson){bool ok=false;json value=ParseJson(valueJson,ok);if(!ok)value=valueJson.toStdString();json r;if(!call("re_session_fact_set",{{"key",key.toStdString()},{"value",value}},r,true))return false;setResult(r);refresh();return true;}
bool ReController::saveBreakpointTemplates(const QString& jsonText){bool ok=false;json value=ParseJson(jsonText,ok);if(!ok||!value.is_array()){fail(QStringLiteral("Breakpoint templates must be a JSON array."));return false;}json r;if(!call("re_session_breakpoints",{{"templates",value}},r,true))return false;setResult(r);refresh();return true;}
bool ReController::applyBreakpointTemplates(){json r;if(!call("re_session_apply_breakpoints",json::object(),r,true))return false;setResult(r);return true;}
bool ReController::exportSession(){json r;if(!call("session_export",json::object(),r))return false;setResult(r);refreshSessions();return true;}
bool ReController::diffSessions(const QString& a,const QString& b){json r;if(!call("session_diff",{{"a",a.toStdString()},{"b",b.toStdString()}},r))return false;setResult(r);return true;}
bool ReController::ghidraExport(const QString& name){json r;if(!call("ghidra_export",{{"name",name.toStdString()}},r))return false;setResult(r);return true;}
bool ReController::ghidraImport(const QString& jsonText){bool ok=false;json doc=ParseJson(jsonText,ok);if(!ok||!doc.is_object()){fail(QStringLiteral("Ghidra import must be a JSON object."));return false;}json r;if(!call("ghidra_import_symbols",doc,r,true))return false;setResult(r);refresh();return true;}

