#include "routes.h"
#include "../process/address.h"
#include "../call/call.h"
#include "../overlay/overlay.h"
#include "../action/action.h"

#include <nlohmann/json.hpp>
#include <sstream>

using json = nlohmann::json;

namespace api {
namespace {

uintptr_t ParseAddress(const json& jaddr) {
    std::string error;
    const uintptr_t value = process::ResolveAddress(jaddr, &error);
    if (!value && !error.empty()) throw std::runtime_error(error);
    return value;
}

std::string HexAddr(uintptr_t a) {
    std::ostringstream s;
    s << "0x" << std::hex << a;
    return s.str();
}

bool ParseConvention(const std::string& value, remotecall::Convention& out) {
    if (value == "cdecl") { out = remotecall::Convention::Cdecl; return true; }
    if (value == "stdcall") { out = remotecall::Convention::Stdcall; return true; }
    if (value == "thiscall") { out = remotecall::Convention::Thiscall; return true; }
    if (value == "fastcall") { out = remotecall::Convention::Fastcall; return true; }
    return false;
}

std::vector<uintptr_t> ParseArgs(const json& body) {
    std::vector<uintptr_t> args;
    if (!body.contains("args")) return args;
    if (!body.at("args").is_array()) throw std::runtime_error("args_must_be_array");
    for (const auto& value : body.at("args")) args.push_back(ParseAddress(value));
    if (args.size() > 8) throw std::runtime_error("too_many_args_max_8");
    return args;
}

uint32_t ParseTimeout(const json& body) {
    uint64_t timeout = body.value("timeout_ms", static_cast<uint64_t>(3000));
    if (timeout < 1) timeout = 1;
    if (timeout > 60000) timeout = 60000;
    return static_cast<uint32_t>(timeout);
}

json ResultJson(const remotecall::CallResult& result) {
    json out{{"ok", result.ok},
             {"thread_id", result.threadId},
             {"duration_ms", result.durationMs}};
    if (result.ok) {
        out["return_value"] = HexAddr(static_cast<uintptr_t>(result.returnValue));
        out["return_value_u64"] = result.returnValue;
    } else {
        out["error"] = result.error.empty() ? "native_call_failed" : result.error;
        if (result.exceptionCode) {
            std::ostringstream code;
            code << "0x" << std::hex << result.exceptionCode;
            out["exception_code"] = code.str();
        }
    }
    return out;
}

bool ParseCall(const httplib::Request& req,
               json& body,
               uintptr_t& address,
               std::vector<uintptr_t>& args,
               remotecall::Convention& convention,
               std::string& error) {
    try {
        body = json::parse(req.body.empty() ? "{}" : req.body);
        if (!body.contains("address")) { error = "missing_address"; return false; }
        address = ParseAddress(body.at("address"));
        args = ParseArgs(body);
        const std::string conventionName = body.value("convention", std::string("cdecl"));
        if (!ParseConvention(conventionName, convention)) {
            error = "invalid_convention_expected_cdecl_stdcall_thiscall_fastcall";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

void WriteCallResponse(httplib::Response& res, const remotecall::CallResult& result) {
    if (!result.ok && (result.error.find("timeout") != std::string::npos ||
                       result.error == "game_thread_not_observed_before_timeout")) {
        res.status = 504;
    } else if (!result.ok) {
        res.status = 400;
    }
    res.set_content(ResultJson(result).dump(), "application/json");
}

} // namespace

void RegisterCallRoutes(httplib::Server& svr) {
    // Legacy immediate call on the API worker thread. Kept for compatibility;
    // /call/game-thread is the default choice for engine/gameplay functions.
    svr.Post("/call/function", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        json body; uintptr_t address = 0; std::vector<uintptr_t> args;
        remotecall::Convention convention = remotecall::Convention::Cdecl; std::string error;
        if (!ParseCall(req, body, address, args, convention, error)) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
            return;
        }
        const auto result = remotecall::Invoke(address, args, convention);
        WriteCallResponse(res, result);
        overlay::LogApiCall("POST /call/function @ " + HexAddr(address));
    });

    // Marshals onto the thread observed in the render/present hook and runs
    // on the next frame. This is the safe default for functions that require
    // game/render-thread TLS or thread affinity.
    svr.Post("/call/game-thread", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        json body; uintptr_t address = 0; std::vector<uintptr_t> args;
        remotecall::Convention convention = remotecall::Convention::Cdecl; std::string error;
        if (!ParseCall(req, body, address, args, convention, error)) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
            return;
        }
        const auto result = remotecall::InvokeOnGameThread(address, args, convention, ParseTimeout(body));
        WriteCallResponse(res, result);
        overlay::LogApiCall("POST /call/game-thread @ " + HexAddr(address));
    });

    // Safe cooperative dispatch to an arbitrary message-pumping thread. Cortex
    // deliberately refuses non-dispatchable worker threads instead of doing a
    // suspend/context/stack hijack that can corrupt the target.
    svr.Post("/call/thread", [](const httplib::Request& req, httplib::Response& res) {
        auto mutation = action::LockMutations();
        json body; uintptr_t address = 0; std::vector<uintptr_t> args;
        remotecall::Convention convention = remotecall::Convention::Cdecl; std::string error;
        if (!ParseCall(req, body, address, args, convention, error)) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", error}}.dump(), "application/json");
            return;
        }
        if (!body.contains("thread_id") || !body.at("thread_id").is_number_unsigned() && !body.at("thread_id").is_number_integer()) {
            res.status = 400;
            res.set_content(json{{"ok", false}, {"error", "thread_id_integer_required"}}.dump(), "application/json");
            return;
        }
        const uint32_t tid = body.at("thread_id").get<uint32_t>();
        const auto result = remotecall::InvokeOnThread(tid, address, args, convention, ParseTimeout(body));
        WriteCallResponse(res, result);
        overlay::LogApiCall("POST /call/thread tid=" + std::to_string(tid) + " @ " + HexAddr(address));
    });

    svr.Get("/call/game-thread/status", [](const httplib::Request&, httplib::Response& res) {
        const uint64_t last = remotecall::LastGameThreadPumpMs();
        const uint64_t now = GetTickCount64();
        res.set_content(json{{"ok", true},
                             {"thread_id", remotecall::GameThreadId()},
                             {"last_frame_ms", last},
                             {"age_ms", last ? now - last : 0},
                             {"active", last != 0 && now - last < 2000},
                             {"pending_calls", remotecall::PendingGameThreadCalls()}}.dump(),
                        "application/json");
    });
}

} // namespace api
