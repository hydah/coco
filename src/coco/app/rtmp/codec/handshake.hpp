#pragma once

#include "coco/utils/io.hpp"

namespace coco {

// C0/C1/C2 as a client, or S0/S1/S2 as a server. Complex (HMAC-SHA256 digest) when the
// peer's version field is non-zero and the digest checks out, otherwise the simple
// handshake: a zero version and an echo of the peer's 1536 bytes. in and out are usually
// the same StreamConn.
int RtmpHandshake(IoReader* in, IoWriter* out, bool client);

// SHA-256 and HMAC-SHA256 against published test vectors. Handshake digests use the same
// code; this is what the tests call.
bool RtmpDigestSelfCheck();

}  // namespace coco
