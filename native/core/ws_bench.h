// Screen Share native core – round trips through a WebSocket and an echo server: how long a message
// of a video frame's size takes there and back. (CI: does a platform WebSocket hold the end of a
// message back – Nagle's algorithm waiting for the other side's delayed ACK?)
#pragma once

#include <string>

#include "json.h"
#include "websocket.h"

namespace ss {

/** Sends `count` binary messages to the echo server at `url`, 60 a second, 1 KB … 120 KB in turn,
 *  and times each one's round trip: {sent, back, all: {med, p90, max}, "1000": {…}, …} or {error}. */
Json wsBench(const WebSocketFactory& open, const std::string& url, int count);

}  // namespace ss
