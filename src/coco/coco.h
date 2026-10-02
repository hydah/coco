#pragma once

// Everything a program needs: the runtime and its threads, TCP and UDP, TLS, the HTTP
// client and server, WebSocket and RTMP. Every name is in namespace coco; the macros start with COCO_ or coco_.

// IWYU pragma: begin_exports
#include "coco/base/coco_thread.hpp"
#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/layer4/coco_tcp.hpp"
#include "coco/net/layer4/coco_udp.hpp"
#include "coco/net/layer7/http/coco_http.hpp"
#include "coco/net/layer7/rtmp/coco_rtmp.hpp"
#include "coco/net/layer7/ws/coco_ws.hpp"
#include "coco/net/tls/coco_tls.hpp"
#include "coco/server/coco_http_server.hpp"
#include "coco/server/coco_rtmp_server.hpp"
#include "coco/server/coco_tcp_server.hpp"
// IWYU pragma: end_exports
