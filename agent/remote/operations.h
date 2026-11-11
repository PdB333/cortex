#pragma once
#include "backend.h"
#include "../../core/remote/queries.h"

namespace cortex::remote {
// Scan exactly one already-authorized read. In particular this does not loop
// over all processes, attach repeatedly, change permissions or silently retry.
inline json ExecuteOperation(Backend& backend, const std::string& action,
                             const json& args, const std::string& instance) {
    ValidateArguments(action, args);
    if (action != "scan") return backend.Execute(action, args, instance);
    json read = {{"pid", args.at("pid")}, {"target_id", args.at("target_id")},
                 {"address", args.at("address")}, {"length", args.at("length")}};
    auto observed = backend.Execute("read", read, instance);
    ValidateResponseResult("read", read, observed);
    if (!observed.at("ok").get<bool>()) return observed;
    auto result = ScanBytes(Unhex(Text(observed, "hex", kMaxMemory * 2)), args);
    if (observed.contains("effect")) result["effect"] = observed["effect"];
    return result;
}
} // namespace cortex::remote
