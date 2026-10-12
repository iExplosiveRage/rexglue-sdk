/**
 * @file        system/net/http_client_stub.cpp
 * @brief       No HTTP client on this platform (see http_client.h).
 */

#include "http_client.h"

namespace rex::net::online {

HttpResponse HttpRequest(const std::string&, const std::string&, const std::string&, int) {
  return {};
}

}  // namespace rex::net::online
