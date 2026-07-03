#pragma once

#include <map>
#include <string>

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;

    static HttpResponse json(int statusCode, const std::string& payload) {
        HttpResponse response;
        response.status = statusCode;
        response.body = payload;
        response.headers["Content-Type"] = "application/json";
        response.headers["Connection"] = "close";
        return response;
    }
};
