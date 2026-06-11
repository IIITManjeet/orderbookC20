#include "order_book/telemetry_ws.hpp"

#include <cstdio>
#include <mutex>
#include <string>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

namespace ob {
namespace {

// Broadcasts telemetry JSONL lines to all connected WebSocket clients. Runs the
// server on its own background thread (IXWebSocket-managed); write_line() is
// called from the TelemetryPublisher thread and pushes one text frame per line
// to every open connection. Low message rate (fills + 2s status), so a simple
// fan-out over the current client set is fine.
class WsSink final : public TelemetrySink {
public:
    explicit WsSink(int port) : server_(port, "0.0.0.0") {
        static std::once_flag net_init;
        std::call_once(net_init, [] { ix::initNetSystem(); });

        // Inbound messages are ignored — this is a one-way telemetry feed.
        server_.setOnClientMessageCallback(
            [](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&,
               const ix::WebSocketMessagePtr&) {});

        auto [ok, err] = server_.listen();
        if (!ok) {
            std::fprintf(stderr, "telemetry --serve: cannot bind port %d: %s\n",
                         port, err.c_str());
            return;
        }
        server_.disablePerMessageDeflate();
        server_.start();
        listening_ = true;
        std::printf("telemetry: serving WebSocket on ws://0.0.0.0:%d "
                    "(dashboard: index.html?ws=ws://localhost:%d)\n", port, port);
    }

    ~WsSink() override {
        if (listening_) server_.stop();
    }

    void write_line(std::string_view sv) override {
        if (!listening_) return;
        const std::string msg(sv);
        for (auto& client : server_.getClients()) {
            if (client->getReadyState() == ix::ReadyState::Open) {
                client->sendText(msg);
            }
        }
    }

private:
    ix::WebSocketServer server_;
    bool listening_{false};
};

}  // namespace

std::unique_ptr<TelemetrySink> make_ws_sink(int port) {
    return std::make_unique<WsSink>(port);
}

}  // namespace ob
