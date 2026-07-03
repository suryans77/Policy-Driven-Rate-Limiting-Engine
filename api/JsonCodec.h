#pragma once

#include "policy/Policy.h"
#include "policy/Request.h"

#include <string>
#include <vector>

struct DecodeRequestResult {
    bool ok = false;
    Request request;
    std::string error;
};

struct DecodePolicyResult {
    bool ok = false;
    Policy policy;
    std::string error;
};

class JsonCodec {
public:
    static DecodeRequestResult decodeEvaluateRequest(const std::string& body);
    static DecodePolicyResult decodePolicy(const std::string& body);

    static std::string encodeEvaluateResponse(bool allowed,
                                              const Request& request,
                                              const std::string& policyKey,
                                              const std::string& algorithm,
                                              const std::string& state);
    static std::string encodePolicies(const std::vector<Policy>& policies);
    static std::string encodePolicy(const Policy& policy);
    static std::string encodeError(const std::string& message);
};
