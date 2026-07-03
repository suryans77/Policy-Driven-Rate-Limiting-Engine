#include "StateStore.h"

bool StateStore::eval(const std::string&,
                      const std::vector<std::string>&,
                      const std::vector<std::string>&,
                      std::string&,
                      std::string& error) {
    error = "atomic scripts are not supported by this state store";
    return false;
}
