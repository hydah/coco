#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "coco/app/rtmp/codec/amf0.hpp"
#include "coco/app/rtmp/codec/chunk.hpp"

namespace coco {

// A command message: name, transaction id, command object, then the arguments.
struct RtmpCommand {
    std::string name;
    double tx = 0;
    Amf0Value object;
    std::vector<Amf0Value> args;
};

bool RtmpIsCommand(uint8_t type);
// Decodes an AMF0 or AMF3 command message. ERROR_RTMP_AMF unless it starts with a name and
// a transaction id.
int RtmpParseCommand(const RtmpMessage& msg, RtmpCommand* cmd);

// The values of a "_result" for transaction tx.
std::vector<Amf0Value> RtmpResult(double tx, const Amf0Value& arg);
// The values of an "onStatus" with level "status".
std::vector<Amf0Value> RtmpOnStatus(const std::string& code, const std::string& desc);
// The User Control message that tells the peer stream_id has begun.
RtmpMessage RtmpStreamBegin(uint32_t stream_id);
// The code of the info object an onStatus carries, or "".
std::string RtmpStatusCode(const RtmpCommand& cmd);
// Whether a status code reports a failure, e.g. NetStream.Publish.BadName.
bool RtmpBadStatus(const std::string& code);

}  // namespace coco
