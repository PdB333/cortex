#pragma once

#include "investigation_context.h"

#include <cstdint>
#include <string>

namespace context::api {

// Route semantics for /context, /context/observation and /context/select,
// independent of the HTTP/pipe transport so they can be tested without a
// target process. routes_context.cpp adapts them to httplib.
struct Reply {
    int status = 200;
    json body;
};

// Maps an api::response::OperationResult to an HTTP status:
// completed 200, refused 403 (stale_target_generation 409), failed 400.
int HttpStatus(const json& result);

// GET /context?limit&since&expected_generation
Reply GetContext(InvestigationContext& shared, uint64_t limit, uint64_t since, uint64_t expectedGeneration,
                 uint64_t latestEventId);

// POST /context/observation (any caller; State tier over MCP).
Reply AddObservation(InvestigationContext& shared, const CallerInfo& caller, const std::string& body);

// POST /context/select (desktop channel only).
Reply SelectAddress(InvestigationContext& shared, const CallerInfo& caller, const std::string& body);

// The refusal every context route returns for a request pinned to another
// process lifetime.
Reply StaleGeneration(const InvestigationContext& shared);

// The failure for malformed query parameters.
Reply InvalidQuery(const InvestigationContext& shared, const std::string& code);

} // namespace context::api
