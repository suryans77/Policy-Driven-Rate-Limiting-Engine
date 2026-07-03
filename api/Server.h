#pragma once

#include "RestController.h"

class Server {
public:
    Server(RestController& controller, int port);
    void run();

private:
    RestController& controller_;
    int port_;
};
