#pragma once

#include "RestController.h"

class Server {
public:
    Server(RestController& controller, int port, int workers = 4,
           int maxQueuedConnections = 256);
    bool run();

private:
    RestController& controller_;
    int port_;
    int workers_;
    int maxQueuedConnections_;
};
