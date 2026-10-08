#pragma once

#include <mw/error.hpp>

// Application results use libmw's expected type and error container.
using mw::E;
// Carries concrete error types through application layers.
using mw::Error;
// Describes failures without an HTTP status.
using mw::RuntimeError;
// Preserves the status and message of HTTP failures.
using mw::HTTPError;
// Creates an application runtime error.
using mw::runtimeError;
// Creates an application HTTP error.
using mw::httpError;
// Retrieves the message from any application error.
using mw::errorMsg;
