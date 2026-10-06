#pragma once
#include <httplib.h>
#include "http_client.hpp"

// Copies the client request payload and headers to an httplib request.
void copyToHttplibReq(const HTTPRequest& src, httplib::Request& dest);
