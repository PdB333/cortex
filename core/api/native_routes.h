#pragma once

#include <httplib.h>

#include <functional>
#include <string>
#include <utility>

namespace api {

using NativeRouteHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

struct NativeRouteResult {
    bool found = false;
    int status = 404;
    std::string body;
    std::string contentType;
};

// Register/dispatch the same route handlers used by cpp-httplib without
// opening a loopback socket. The registry is populated while API routes are
// registered during api::Start().
void RegisterNativeRoute(const std::string& method,
                         const std::string& pattern,
                         NativeRouteHandler handler);
void ClearNativeRoutes();
bool HasNativeRoute(const std::string& method, const std::string& target);
size_t NativeRouteCount();
NativeRouteResult DispatchNativeRoute(const std::string& method,
                                      const std::string& target,
                                      const std::string& body = {},
                                      const httplib::Headers& headers = {});

// Small facade used by routes_*.cpp. Each Get/Post/... call is mirrored into
// the native registry and delegated to the real httplib::Server, keeping one
// business handler as the source of truth for REST and MCP.
class RouteRegistrar {
public:
    explicit RouteRegistrar(httplib::Server& server) : server_(server) {}

    RouteRegistrar& Get(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("GET", pattern, handler);
        server_.Get(pattern, std::move(handler));
        return *this;
    }

    RouteRegistrar& Post(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("POST", pattern, handler);
        server_.Post(pattern, std::move(handler));
        return *this;
    }

    RouteRegistrar& Put(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("PUT", pattern, handler);
        server_.Put(pattern, std::move(handler));
        return *this;
    }

    RouteRegistrar& Patch(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("PATCH", pattern, handler);
        server_.Patch(pattern, std::move(handler));
        return *this;
    }

    RouteRegistrar& Delete(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("DELETE", pattern, handler);
        server_.Delete(pattern, std::move(handler));
        return *this;
    }

    RouteRegistrar& Options(const std::string& pattern, NativeRouteHandler handler) {
        RegisterNativeRoute("OPTIONS", pattern, handler);
        server_.Options(pattern, std::move(handler));
        return *this;
    }

private:
    httplib::Server& server_;
};

} // namespace api

