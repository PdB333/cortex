#include "native_routes.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <regex>
#include <vector>

namespace api {
namespace {

struct RouteEntry {
    std::string method;
    std::string pattern;
    NativeRouteHandler handler;
};

std::vector<RouteEntry> g_routes;
std::mutex g_routesMutex;

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

std::string PercentDecode(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            const int hi = HexDigit(input[i + 1]);
            const int lo = HexDigit(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                output.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        output.push_back(input[i] == '+' ? ' ' : input[i]);
    }
    return output;
}

void ParseQuery(const std::string& query, httplib::Params& params) {
    size_t cursor = 0;
    while (cursor <= query.size()) {
        const size_t amp = query.find('&', cursor);
        const size_t end = amp == std::string::npos ? query.size() : amp;
        if (end > cursor) {
            const size_t eq = query.find('=', cursor);
            if (eq == std::string::npos || eq >= end) {
                params.emplace(PercentDecode(query.substr(cursor, end - cursor)), std::string());
            } else {
                params.emplace(PercentDecode(query.substr(cursor, eq - cursor)),
                               PercentDecode(query.substr(eq + 1, end - eq - 1)));
            }
        }
        if (amp == std::string::npos) break;
        cursor = amp + 1;
    }
}

std::string Upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

} // namespace

void RegisterNativeRoute(const std::string& method,
                         const std::string& pattern,
                         NativeRouteHandler handler) {
    std::lock_guard<std::mutex> lock(g_routesMutex);
    g_routes.push_back({Upper(method), pattern, std::move(handler)});
}

void ClearNativeRoutes() {
    std::lock_guard<std::mutex> lock(g_routesMutex);
    g_routes.clear();
}

NativeRouteResult DispatchNativeRoute(const std::string& method,
                                      const std::string& target,
                                      const std::string& body,
                                      const httplib::Headers& headers) {
    std::string path = target;
    std::string query;
    const size_t question = target.find('?');
    if (question != std::string::npos) {
        path = target.substr(0, question);
        query = target.substr(question + 1);
    }

    std::vector<RouteEntry> routes;
    {
        std::lock_guard<std::mutex> lock(g_routesMutex);
        routes = g_routes;
    }

    const std::string wantedMethod = Upper(method);
    for (const auto& route : routes) {
        if (route.method != wantedMethod) continue;

        std::smatch matches;
        bool matched = false;
        try {
            matched = std::regex_match(path, matches, std::regex(route.pattern));
        } catch (const std::regex_error&) {
            // cpp-httplib accepts ordinary literal patterns too. Escaping a
            // malformed regex here would risk changing route semantics, so a
            // bad native pattern is simply skipped and remains REST-only.
            continue;
        }
        if (!matched) continue;

        httplib::Request request;
        request.method = wantedMethod;
        request.target = target;
        request.path = path;
        request.body = body;
        request.headers = headers;
        request.matches = matches;
        ParseQuery(query, request.params);

        httplib::Response response;
        try {
            route.handler(request, response);
        } catch (const std::exception& error) {
            response.status = 500;
            response.set_content(std::string("{\"error\":\"native_handler_exception\",\"message\":") +
                                     nlohmann::json(error.what()).dump() + "}",
                                 "application/json");
        } catch (...) {
            response.status = 500;
            response.set_content("{\"error\":\"native_handler_exception\"}", "application/json");
        }

        NativeRouteResult result;
        result.found = true;
        result.status = response.status > 0 ? response.status : 200;
        result.body = std::move(response.body);
        result.contentType = response.get_header_value("Content-Type");
        return result;
    }

    return {};
}

} // namespace api
