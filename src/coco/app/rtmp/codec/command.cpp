#include "coco/app/rtmp/codec/command.hpp"

#include "coco/common/error.hpp"
#include "coco/app/rtmp/codec/bytes.hpp"

namespace coco {

bool RtmpIsCommand(uint8_t type) {
    return type == RTMP_MSG_COMMAND_AMF0 || type == RTMP_MSG_COMMAND_AMF3;
}

int RtmpParseCommand(const RtmpMessage& msg, RtmpCommand* cmd) {
    const uint8_t* p = (const uint8_t*)msg.payload.data();
    size_t n = msg.payload.size();
    if (msg.type == RTMP_MSG_COMMAND_AMF3 || msg.type == RTMP_MSG_DATA_AMF3) {
        if (n < 1) {
            return ERROR_RTMP_AMF;
        }
        p++;
        n--;
    }
    std::vector<Amf0Value> values;
    int ret = Amf0DecodeAll(p, n, &values);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (values.size() < 3 || values[0].type != Amf0Value::kString ||
        values[1].type != Amf0Value::kNumber) {
        return ERROR_RTMP_AMF;
    }
    cmd->name = values[0].str;
    cmd->tx = values[1].number;
    cmd->object = values[2];
    cmd->args.assign(values.begin() + 3, values.end());
    return COCO_SUCCESS;
}

std::vector<Amf0Value> RtmpResult(double tx, const Amf0Value& arg) {
    std::vector<Amf0Value> v;
    v.push_back(Amf0Value::Str("_result"));
    v.push_back(Amf0Value::Num(tx));
    v.push_back(Amf0Value::Null());
    v.push_back(arg);
    return v;
}

std::vector<Amf0Value> RtmpOnStatus(const std::string& code, const std::string& desc) {
    Amf0Value info = Amf0Value::Object();
    info.Put("level", Amf0Value::Str("status"));
    info.Put("code", Amf0Value::Str(code));
    info.Put("description", Amf0Value::Str(desc));
    std::vector<Amf0Value> v;
    v.push_back(Amf0Value::Str("onStatus"));
    v.push_back(Amf0Value::Num(0));
    v.push_back(Amf0Value::Null());
    v.push_back(info);
    return v;
}

RtmpMessage RtmpStreamBegin(uint32_t stream_id) {
    uint8_t b[6] = {0, 0};
    RtmpStoreBe32(b + 2, stream_id);
    RtmpMessage msg;
    msg.type = RTMP_MSG_USER_CONTROL;
    msg.csid = 2;
    msg.payload.assign((char*)b, 6);
    return msg;
}

std::string RtmpStatusCode(const RtmpCommand& cmd) {
    if (cmd.args.empty()) {
        return "";
    }
    if (cmd.args[0].type == Amf0Value::kObject || cmd.args[0].type == Amf0Value::kEcmaArray) {
        return cmd.args[0].GetString("code");
    }
    return "";
}

bool RtmpBadStatus(const std::string& code) {
    return code.find("Error") != std::string::npos || code.find("Fail") != std::string::npos ||
           code.find("Bad") != std::string::npos || code.find("Rejected") != std::string::npos;
}

}  // namespace coco
