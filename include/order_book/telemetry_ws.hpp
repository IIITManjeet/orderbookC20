#pragma once

#include <memory>

#include "order_book/telemetry.hpp"

namespace ob {

// Create a TelemetrySink that runs a WebSocket server on `port` and broadcasts
// each telemetry line to every connected client (the web dashboard's live mode).
// Implemented in src/telemetry_ws.cpp (compiled into live_core) so the
// IXWebSocket dependency stays isolated there — this header pulls in no TLS/WS
// headers. Returns a sink whose write_line() fans the line out to all clients;
// if the port can't be bound it logs to stderr and silently drops (no clients).
std::unique_ptr<TelemetrySink> make_ws_sink(int port);

}  // namespace ob
