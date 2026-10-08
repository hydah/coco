#include "coco/app/rtmp/client.hpp"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/tcp.hpp"
#include "coco/app/rtmp/codec/command.hpp"
#include "coco/app/rtmp/codec/handshake.hpp"

namespace coco {

RtmpClient::RtmpClient() : dialer_(TcpDialer()) {}

RtmpClient::~RtmpClient() {}

void RtmpClient::SetDialer(StreamDialer dialer) {
    dialer_ = dialer;
    dialer_set_ = true;
}

int RtmpClient::WriteCommand(uint32_t stream_id, const std::vector<Amf0Value>& values) {
    return RtmpWriteCommand(conn_.get(), stream_id, values);
}

int RtmpClient::WaitResult(double txid, Amf0Value* info, double* number) {
    for (int i = 0; i < 64; i++) {
        RtmpMessage msg;
        int ret = conn_->ReadMessage(&msg);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (!RtmpIsCommand(msg.type)) {
            continue;
        }
        RtmpCommand cmd;
        if ((ret = RtmpParseCommand(msg, &cmd)) != COCO_SUCCESS) {
            return ret;
        }
        if (cmd.name == "_error") {
            return ERROR_RTMP_PROTOCOL;
        }
        if (cmd.name != "_result" || cmd.tx != txid) {
            continue;
        }
        if (!cmd.args.empty()) {
            if (cmd.args[0].type == Amf0Value::kNumber && number) {
                *number = cmd.args[0].number;
            }
            if (info) {
                *info = cmd.args[0];
            }
        }
        return COCO_SUCCESS;
    }
    return ERROR_RTMP_PROTOCOL;
}

int RtmpClient::WaitStatus(const std::string& code) {
    for (int i = 0; i < 64; i++) {
        RtmpMessage msg;
        int ret = conn_->ReadMessage(&msg);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (!RtmpIsCommand(msg.type)) {
            continue;
        }
        RtmpCommand cmd;
        if ((ret = RtmpParseCommand(msg, &cmd)) != COCO_SUCCESS) {
            return ret;
        }
        if (cmd.name != "onStatus" && cmd.name != "onFCPublish") {
            continue;
        }
        std::string got = RtmpStatusCode(cmd);
        if (RtmpBadStatus(got)) {
            coco_error("rtmp: status %s", got.c_str());
            return ERROR_RTMP_PROTOCOL;
        }
        if (got.find(code) != std::string::npos) {
            return COCO_SUCCESS;
        }
    }
    return ERROR_RTMP_PROTOCOL;
}

int RtmpClient::Dial(const std::string& url, int64_t timeout_us) {
    conn_.reset();
    raw_.reset();
    stream_id_ = 0;
    tx_ = 0;
    int ret = ParseRtmpUrl(url, &url_);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    if (url_.tls && !dialer_set_) {
        coco_error("rtmps needs SetDialer");
        return ERROR_RTMP_URL;
    }
    if ((ret = dialer_(url_.host, url_.port, timeout_us, &raw_)) != COCO_SUCCESS) {
        return ret;
    }
    raw_->SetTimeout(timeout_us);
    if ((ret = RtmpHandshake(raw_.get(), raw_.get(), true)) != COCO_SUCCESS) {
        return ret;
    }
    conn_.reset(new RtmpConn(raw_.get()));
    if ((ret = conn_->SetChunkSize(kRtmpOutChunkSize)) != COCO_SUCCESS) {
        return ret;
    }
    if ((ret = conn_->WriteWindowAckSize(kRtmpAckWindow)) != COCO_SUCCESS) {
        return ret;
    }
    Amf0Value obj = Amf0Value::Object();
    obj.Put("app", Amf0Value::Str(url_.app));
    obj.Put("type", Amf0Value::Str("nonprivate"));
    obj.Put("flashVer", Amf0Value::Str("FMLE/3.0 (compatible; coco)"));
    obj.Put("tcUrl", Amf0Value::Str(url_.tc_url));
    obj.Put("objectEncoding", Amf0Value::Num(0));
    double tx = ++tx_;
    std::vector<Amf0Value> v;
    v.push_back(Amf0Value::Str("connect"));
    v.push_back(Amf0Value::Num(tx));
    v.push_back(obj);
    if ((ret = WriteCommand(0, v)) != COCO_SUCCESS) {
        return ret;
    }
    Amf0Value info;
    if ((ret = WaitResult(tx, &info, nullptr)) != COCO_SUCCESS) {
        return ret;
    }
    if (info.GetString("code") != "NetConnection.Connect.Success") {
        return ERROR_RTMP_PROTOCOL;
    }
    return COCO_SUCCESS;
}

int RtmpClient::Publish() {
    if (!conn_ || url_.stream.empty()) {
        return ERROR_RTMP_URL;
    }
    int ret = COCO_SUCCESS;
    double tx = ++tx_;
    std::vector<Amf0Value> rel;
    rel.push_back(Amf0Value::Str("releaseStream"));
    rel.push_back(Amf0Value::Num(tx));
    rel.push_back(Amf0Value::Null());
    rel.push_back(Amf0Value::Str(url_.stream));
    if ((ret = WriteCommand(0, rel)) != COCO_SUCCESS) {
        return ret;
    }
    tx = ++tx_;
    std::vector<Amf0Value> fc;
    fc.push_back(Amf0Value::Str("FCPublish"));
    fc.push_back(Amf0Value::Num(tx));
    fc.push_back(Amf0Value::Null());
    fc.push_back(Amf0Value::Str(url_.stream));
    if ((ret = WriteCommand(0, fc)) != COCO_SUCCESS) {
        return ret;
    }
    tx = ++tx_;
    std::vector<Amf0Value> cs;
    cs.push_back(Amf0Value::Str("createStream"));
    cs.push_back(Amf0Value::Num(tx));
    cs.push_back(Amf0Value::Null());
    if ((ret = WriteCommand(0, cs)) != COCO_SUCCESS) {
        return ret;
    }
    double sid = 0;
    if ((ret = WaitResult(tx, nullptr, &sid)) != COCO_SUCCESS) {
        return ret;
    }
    stream_id_ = (uint32_t)sid;
    if (stream_id_ == 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    std::vector<Amf0Value> pub;
    pub.push_back(Amf0Value::Str("publish"));
    pub.push_back(Amf0Value::Num(0));
    pub.push_back(Amf0Value::Null());
    pub.push_back(Amf0Value::Str(url_.stream));
    pub.push_back(Amf0Value::Str("live"));
    if ((ret = WriteCommand(stream_id_, pub)) != COCO_SUCCESS) {
        return ret;
    }
    return WaitStatus("NetStream.Publish.Start");
}

int RtmpClient::Play() {
    if (!conn_ || url_.stream.empty()) {
        return ERROR_RTMP_URL;
    }
    double tx = ++tx_;
    std::vector<Amf0Value> cs;
    cs.push_back(Amf0Value::Str("createStream"));
    cs.push_back(Amf0Value::Num(tx));
    cs.push_back(Amf0Value::Null());
    int ret = WriteCommand(0, cs);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    double sid = 0;
    if ((ret = WaitResult(tx, nullptr, &sid)) != COCO_SUCCESS) {
        return ret;
    }
    stream_id_ = (uint32_t)sid;
    if (stream_id_ == 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    std::vector<Amf0Value> play;
    play.push_back(Amf0Value::Str("play"));
    play.push_back(Amf0Value::Num(0));
    play.push_back(Amf0Value::Null());
    play.push_back(Amf0Value::Str(url_.stream));
    if ((ret = WriteCommand(stream_id_, play)) != COCO_SUCCESS) {
        return ret;
    }
    return WaitStatus("NetStream.Play.Start");
}

int RtmpClient::ReadMessage(RtmpMessage* msg) {
    if (!conn_) {
        return ERROR_SOCKET_CLOSED;
    }
    return conn_->ReadMessage(msg);
}

int RtmpClient::WriteMessage(const RtmpMessage& msg) {
    if (!conn_) {
        return ERROR_SOCKET_CLOSED;
    }
    if (msg.stream_id != 0) {
        return conn_->WriteMessage(msg);
    }
    RtmpMessage copy = msg;
    copy.stream_id = stream_id_;
    return conn_->WriteMessage(copy);
}

}  // namespace coco
