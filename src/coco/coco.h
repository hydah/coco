#pragma once

// Everything a program needs: the runtime and its threads, TCP, UDP and RUDP, TLS, the
// HTTP client and server, WebSocket and RTMP. Every name is in namespace coco; the macros start with COCO_ or coco_.

// IWYU pragma: begin_exports
#include "coco/base/coco_thread.hpp"
#include "coco/base/task_group.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/dns/resolver.hpp"
#include "coco/net/tcp.hpp"
#include "coco/net/tcp_server.hpp"
#include "coco/net/udp.hpp"
#include "coco/net/rudp/conn.hpp"
#include "coco/net/tls/conn.hpp"
#include "coco/app/http/client.hpp"
#include "coco/app/http/mux.hpp"
#include "coco/app/http/server.hpp"
#include "coco/app/ws/client.hpp"
#include "coco/app/ws/handler.hpp"
#include "coco/app/rtmp/client.hpp"
#include "coco/app/rtmp/server.hpp"
// IWYU pragma: end_exports
