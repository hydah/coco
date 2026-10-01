#include "coco/net/layer7/rtmp/coco_rtmp.hpp"

#include <stdlib.h>
#include <string.h>

#include "st.h"

#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/layer4/coco_tcp.hpp"
#include "coco/net/layer7/rtmp/rtmp_bytes.hpp"
#include "coco/net/layer7/rtmp/rtmp_handshake.hpp"

namespace coco {

namespace {

struct Cmd {
    std::string name;
    double tx = 0;
    Amf0Value object;
    std::vector<Amf0Value> args;
};

bool IsCommand(uint8_t type) {
    return type == RTMP_MSG_COMMAND_AMF0 || type == RTMP_MSG_COMMAND_AMF3;
}

uint32_t DefaultCsid(const RtmpMessage& msg) {
    switch (msg.type) {
        case RTMP_MSG_SET_CHUNK_SIZE:
        case RTMP_MSG_ABORT:
        case RTMP_MSG_ACK:
        case RTMP_MSG_USER_CONTROL:
        case RTMP_MSG_WINDOW_ACK_SIZE:
        case RTMP_MSG_SET_PEER_BANDWIDTH:
            return 2;
        case RTMP_MSG_AUDIO:
            return 6;
        case RTMP_MSG_VIDEO:
            return 7;
        case RTMP_MSG_DATA_AMF0:
        case RTMP_MSG_DATA_AMF3:
            return 5;
        case RTMP_MSG_COMMAND_AMF0:
        case RTMP_MSG_COMMAND_AMF3:
            return msg.stream_id == 0 ? 3 : 4;
        default:
            return 3;
    }
}

void StoreBe32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int ParseCommand(const RtmpMessage& msg, Cmd* cmd) {
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

int WriteValues(RtmpConn* conn, uint32_t stream_id, uint8_t type,
                const std::vector<Amf0Value>& values) {
    RtmpMessage msg;
    msg.type = type;
    msg.stream_id = stream_id;
    msg.payload = Amf0Encode(values);
    return conn->WriteMessage(msg);
}

int WriteCommand(RtmpConn* conn, uint32_t stream_id, const std::vector<Amf0Value>& values) {
    return WriteValues(conn, stream_id, RTMP_MSG_COMMAND_AMF0, values);
}

std::vector<Amf0Value> Result(double tx, const Amf0Value& arg) {
    std::vector<Amf0Value> v;
    v.push_back(Amf0Value::Str("_result"));
    v.push_back(Amf0Value::Num(tx));
    v.push_back(Amf0Value::Null());
    v.push_back(arg);
    return v;
}

int WriteOnStatus(RtmpConn* conn, uint32_t stream_id, const std::string& code,
                  const std::string& desc) {
    Amf0Value info = Amf0Value::Object();
    info.Put("level", Amf0Value::Str("status"));
    info.Put("code", Amf0Value::Str(code));
    info.Put("description", Amf0Value::Str(desc));
    std::vector<Amf0Value> v;
    v.push_back(Amf0Value::Str("onStatus"));
    v.push_back(Amf0Value::Num(0));
    v.push_back(Amf0Value::Null());
    v.push_back(info);
    return WriteCommand(conn, stream_id, v);
}

int WriteStreamBegin(RtmpConn* conn, uint32_t stream_id) {
    uint8_t b[6] = {0, 0};
    StoreBe32(b + 2, stream_id);
    RtmpMessage msg;
    msg.type = RTMP_MSG_USER_CONTROL;
    msg.csid = 2;
    msg.payload.assign((char*)b, 6);
    return conn->WriteMessage(msg);
}

std::string StatusCode(const Cmd& cmd) {
    if (cmd.args.empty()) {
        return "";
    }
    if (cmd.args[0].type == Amf0Value::kObject || cmd.args[0].type == Amf0Value::kEcmaArray) {
        return cmd.args[0].GetString("code");
    }
    return "";
}

bool BadStatus(const std::string& code) {
    return code.find("Error") != std::string::npos || code.find("Fail") != std::string::npos ||
           code.find("Bad") != std::string::npos || code.find("Rejected") != std::string::npos;
}

long ParsePort(const std::string& s) {
    if (s.empty()) {
        return -1;
    }
    char* end = nullptr;
    long port = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || port <= 0 || port > 65535) {
        return -1;
    }
    return port;
}

}  // namespace

struct RtmpConn::State {
    explicit State(StreamConn* conn)
        : reader(conn), write_lock(st_mutex_new()), ack_window(kRtmpAckWindow) {}
    ~State() {
        if (write_lock) {
            st_mutex_destroy(write_lock);
        }
    }

    RtmpChunkReader reader;
    RtmpChunkWriter writer;
    st_mutex_t write_lock = nullptr;
    uint32_t bytes_in = 0;
    uint32_t last_ack = 0;
    uint32_t ack_window = 0;
};

RtmpConn::RtmpConn(StreamConn* conn) : conn_(conn), st_(new State(conn)) {}

RtmpConn::~RtmpConn() {}

int RtmpConn::WriteControl(uint8_t type, const void* data, size_t n) {
    RtmpMessage msg;
    msg.type = type;
    msg.csid = 2;
    if (n) {
        msg.payload.assign((const char*)data, n);
    }
    return WriteMessage(msg);
}

int RtmpConn::SetChunkSize(uint32_t size) {
    if (size < 1 || size > 0x7FFFFFFF) {
        return ERROR_RTMP_PROTOCOL;
    }
    uint8_t b[4];
    StoreBe32(b, size);
    int ret = WriteControl(RTMP_MSG_SET_CHUNK_SIZE, b, 4);
    if (ret == COCO_SUCCESS) {
        st_->writer.SetChunkSize(size);
    }
    return ret;
}

int RtmpConn::WriteWindowAckSize(uint32_t size) {
    uint8_t b[4];
    StoreBe32(b, size);
    return WriteControl(RTMP_MSG_WINDOW_ACK_SIZE, b, 4);
}

int RtmpConn::WritePeerBandwidth(uint32_t size, uint8_t limit_type) {
    uint8_t b[5];
    StoreBe32(b, size);
    b[4] = limit_type;
    return WriteControl(RTMP_MSG_SET_PEER_BANDWIDTH, b, 5);
}

int RtmpConn::MaybeAck() {
    if (st_->ack_window == 0 || st_->bytes_in - st_->last_ack < st_->ack_window) {
        return COCO_SUCCESS;
    }
    uint8_t b[4];
    StoreBe32(b, st_->bytes_in);
    int ret = WriteControl(RTMP_MSG_ACK, b, 4);
    if (ret == COCO_SUCCESS) {
        st_->last_ack = st_->bytes_in;
    }
    return ret;
}

int RtmpConn::OnProtocol(const RtmpMessage& msg, bool* handled) {
    *handled = true;
    const uint8_t* p = (const uint8_t*)msg.payload.data();
    size_t n = msg.payload.size();
    switch (msg.type) {
        case RTMP_MSG_SET_CHUNK_SIZE: {
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint32_t size = RtmpBe32(p) & 0x7FFFFFFF;
            if (size < 1) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->reader.SetChunkSize(size);
            return COCO_SUCCESS;
        }
        case RTMP_MSG_ABORT:
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->reader.Abort(RtmpBe32(p));
            return COCO_SUCCESS;
        case RTMP_MSG_ACK:
            return COCO_SUCCESS;
        case RTMP_MSG_USER_CONTROL: {
            if (n < 2) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint16_t ev = RtmpBe16(p);
            if (ev == 6 && n >= 6) {
                uint8_t b[6];
                b[0] = 0;
                b[1] = 7;
                memcpy(b + 2, p + 2, 4);
                return WriteControl(RTMP_MSG_USER_CONTROL, b, 6);
            }
            return COCO_SUCCESS;
        }
        case RTMP_MSG_WINDOW_ACK_SIZE:
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            st_->ack_window = RtmpBe32(p);
            return COCO_SUCCESS;
        case RTMP_MSG_SET_PEER_BANDWIDTH: {
            if (n < 4) {
                return ERROR_RTMP_PROTOCOL;
            }
            uint32_t size = RtmpBe32(p);
            st_->ack_window = size;
            uint8_t b[4];
            StoreBe32(b, size);
            return WriteControl(RTMP_MSG_WINDOW_ACK_SIZE, b, 4);
        }
        default:
            *handled = false;
            return COCO_SUCCESS;
    }
}

int RtmpConn::ReadMessage(RtmpMessage* msg) {
    while (true) {
        size_t wire = 0;
        int ret = st_->reader.ReadMessage(msg, &wire);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        st_->bytes_in += (uint32_t)wire;
        if ((ret = MaybeAck()) != COCO_SUCCESS) {
            return ret;
        }
        bool handled = false;
        if ((ret = OnProtocol(*msg, &handled)) != COCO_SUCCESS) {
            return ret;
        }
        if (!handled) {
            return COCO_SUCCESS;
        }
    }
}

int RtmpConn::WriteMessage(const RtmpMessage& msg) {
    if (!st_->write_lock || st_mutex_lock(st_->write_lock) != 0) {
        return ERROR_RTMP_PROTOCOL;
    }
    uint32_t csid = msg.csid ? msg.csid : DefaultCsid(msg);
    std::string bytes;
    int ret = st_->writer.Encode(csid, msg, &bytes);
    if (ret == COCO_SUCCESS && !bytes.empty()) {
        ssize_t n = 0;
        ret = conn_->Write((void*)bytes.data(), bytes.size(), &n);
    }
    st_mutex_unlock(st_->write_lock);
    return ret;
}

int ParseRtmpUrl(const std::string& url, RtmpUrl* out) {
    const std::string rtmp = "rtmp://";
    const std::string rtmps = "rtmps://";
    RtmpUrl u;
    size_t off = 0;
    if (url.compare(0, rtmp.size(), rtmp) == 0) {
        off = rtmp.size();
    } else if (url.compare(0, rtmps.size(), rtmps) == 0) {
        u.tls = true;
        off = rtmps.size();
    } else {
        return ERROR_RTMP_URL;
    }
    std::string rest = url.substr(off);
    size_t slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash + 1);
    size_t q = path.find('?');
    if (q != std::string::npos) {
        path.erase(q);
    }
    if (hostport.empty()) {
        return ERROR_RTMP_URL;
    }
    if (hostport[0] == '[') {
        size_t rb = hostport.find(']');
        if (rb == std::string::npos) {
            return ERROR_RTMP_URL;
        }
        u.host = hostport.substr(1, rb - 1);
        if (rb + 1 < hostport.size()) {
            if (hostport[rb + 1] != ':') {
                return ERROR_RTMP_URL;
            }
            long port = ParsePort(hostport.substr(rb + 2));
            if (port < 0) {
                return ERROR_RTMP_URL;
            }
            u.port = (int)port;
        }
    } else {
        size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            u.host = hostport.substr(0, colon);
            long port = ParsePort(hostport.substr(colon + 1));
            if (port < 0) {
                return ERROR_RTMP_URL;
            }
            u.port = (int)port;
        } else {
            u.host = hostport;
        }
    }
    if (u.host.empty()) {
        return ERROR_RTMP_URL;
    }
    while (!path.empty() && path[path.size() - 1] == '/') {
        path.erase(path.size() - 1);
    }
    size_t sep = path.find('/');
    if (path.empty()) {
        return ERROR_RTMP_URL;
    }
    if (sep == std::string::npos) {
        u.app = path;
    } else {
        u.app = path.substr(0, sep);
        u.stream = path.substr(sep + 1);
    }
    if (u.app.empty()) {
        return ERROR_RTMP_URL;
    }
    u.tc_url = u.tls ? "rtmps://" : "rtmp://";
    if (hostport[0] == '[') {
        u.tc_url += "[" + u.host + "]";
    } else {
        u.tc_url += u.host;
    }
    if (u.port != (int)kRtmpDefaultPort) {
        u.tc_url += ":" + std::to_string(u.port);
    }
    u.tc_url += "/" + u.app;
    *out = u;
    return COCO_SUCCESS;
}

int ServeRtmpConn(StreamConn& conn, const RtmpHandler& handler) {
    if (!handler) {
        return ERROR_RTMP_PROTOCOL;
    }
    int ret = RtmpHandshake(&conn, &conn, false);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    RtmpConn rtmp(&conn);
    if ((ret = rtmp.WriteWindowAckSize(kRtmpAckWindow)) != COCO_SUCCESS ||
        (ret = rtmp.WritePeerBandwidth(kRtmpAckWindow, 2)) != COCO_SUCCESS ||
        (ret = rtmp.SetChunkSize(kRtmpOutChunkSize)) != COCO_SUCCESS) {
        return ret;
    }

    std::string app, tc_url, flash_ver;
    bool connected = false;
    uint32_t next_sid = 1;
    while (true) {
        RtmpMessage msg;
        if ((ret = rtmp.ReadMessage(&msg)) != COCO_SUCCESS) {
            return ret;
        }
        if (!IsCommand(msg.type)) {
            continue;
        }
        Cmd cmd;
        if ((ret = ParseCommand(msg, &cmd)) != COCO_SUCCESS) {
            return ret;
        }
        if (cmd.name == "connect") {
            if (connected) {
                continue;
            }
            app = cmd.object.GetString("app");
            size_t aq = app.find('?');
            if (aq != std::string::npos) {
                app.erase(aq);
            }
            while (!app.empty() && app[app.size() - 1] == '/') {
                app.erase(app.size() - 1);
            }
            tc_url = cmd.object.GetString("tcUrl");
            flash_ver = cmd.object.GetString("flashVer");
            double enc = cmd.object.GetNumber("objectEncoding", 0);
            if (enc != 0 && enc != 3) {
                return ERROR_RTMP_PROTOCOL;
            }
            Amf0Value props = Amf0Value::Object();
            props.Put("fmsVer", Amf0Value::Str("FMS/3,0,1,123"));
            props.Put("capabilities", Amf0Value::Num(31));
            props.Put("mode", Amf0Value::Num(1));
            Amf0Value info = Amf0Value::Object();
            info.Put("level", Amf0Value::Str("status"));
            info.Put("code", Amf0Value::Str("NetConnection.Connect.Success"));
            info.Put("description", Amf0Value::Str("Connection succeeded."));
            info.Put("objectEncoding", Amf0Value::Num(0));
            std::vector<Amf0Value> v;
            v.push_back(Amf0Value::Str("_result"));
            v.push_back(Amf0Value::Num(cmd.tx));
            v.push_back(props);
            v.push_back(info);
            if ((ret = WriteCommand(&rtmp, 0, v)) != COCO_SUCCESS) {
                return ret;
            }
            if ((ret = WriteStreamBegin(&rtmp, 0)) != COCO_SUCCESS) {
                return ret;
            }
            connected = true;
            coco_info("rtmp: connect app=%s tcUrl=%s", app.c_str(), tc_url.c_str());
        } else if (!connected) {
            return ERROR_RTMP_PROTOCOL;
        } else if (cmd.name == "releaseStream" || cmd.name == "FCUnpublish" ||
                   cmd.name == "_checkbw") {
            if ((ret = WriteCommand(&rtmp, 0, Result(cmd.tx, Amf0Value::Undef()))) !=
                COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "FCPublish") {
            Amf0Value info = Amf0Value::Object();
            info.Put("level", Amf0Value::Str("status"));
            info.Put("code", Amf0Value::Str("NetStream.Publish.Start"));
            info.Put("description", Amf0Value::Str(""));
            std::vector<Amf0Value> fc;
            fc.push_back(Amf0Value::Str("onFCPublish"));
            fc.push_back(Amf0Value::Num(0));
            fc.push_back(Amf0Value::Null());
            fc.push_back(info);
            if ((ret = WriteCommand(&rtmp, 0, fc)) != COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "createStream") {
            uint32_t sid = next_sid++;
            if ((ret = WriteCommand(&rtmp, 0, Result(cmd.tx, Amf0Value::Num(sid)))) !=
                COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "getStreamLength") {
            if ((ret = WriteCommand(&rtmp, 0, Result(cmd.tx, Amf0Value::Num(0)))) != COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "publish" || cmd.name == "play") {
            RtmpRequest req;
            req.app = app;
            req.tc_url = tc_url;
            req.flash_ver = flash_ver;
            req.publish = cmd.name == "publish";
            req.stream_id = msg.stream_id;
            if (!cmd.args.empty() && cmd.args[0].type == Amf0Value::kString) {
                req.stream = cmd.args[0].str;
            }
            if (req.publish && cmd.args.size() >= 2 && cmd.args[1].type == Amf0Value::kString) {
                req.publish_type = cmd.args[1].str;
            }
            if (req.publish && req.publish_type.empty()) {
                req.publish_type = "live";
            }
            if (req.stream.empty() || req.stream_id == 0) {
                WriteOnStatus(&rtmp, req.stream_id, "NetStream.Publish.BadName", "bad stream");
                return ERROR_RTMP_PROTOCOL;
            }
            if ((ret = WriteStreamBegin(&rtmp, req.stream_id)) != COCO_SUCCESS) {
                return ret;
            }
            if (req.publish) {
                ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Publish.Start",
                                    "Start publishing");
            } else {
                if ((ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Play.Reset", "Reset")) !=
                    COCO_SUCCESS) {
                    return ret;
                }
                ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Play.Start", "Start playback");
            }
            if (ret != COCO_SUCCESS) {
                return ret;
            }
            coco_info("rtmp: %s %s/%s stream_id=%u", req.publish ? "publish" : "play",
                      req.app.c_str(), req.stream.c_str(), req.stream_id);
            return handler(rtmp, req);
        } else if (cmd.name == "closeStream" || cmd.name == "deleteStream") {
            return COCO_SUCCESS;
        }
    }
}

RtmpClient::RtmpClient() : dialer_(TcpDialer()) {}

RtmpClient::~RtmpClient() {}

void RtmpClient::SetDialer(StreamDialer dialer) {
    dialer_ = dialer;
    dialer_set_ = true;
}

int RtmpClient::WriteCommand(uint32_t stream_id, const std::vector<Amf0Value>& values) {
    return ::coco::WriteCommand(conn_.get(), stream_id, values);
}

int RtmpClient::WaitResult(double txid, Amf0Value* info, double* number) {
    for (int i = 0; i < 64; i++) {
        RtmpMessage msg;
        int ret = conn_->ReadMessage(&msg);
        if (ret != COCO_SUCCESS) {
            return ret;
        }
        if (!IsCommand(msg.type)) {
            continue;
        }
        Cmd cmd;
        if ((ret = ParseCommand(msg, &cmd)) != COCO_SUCCESS) {
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
        if (!IsCommand(msg.type)) {
            continue;
        }
        Cmd cmd;
        if ((ret = ParseCommand(msg, &cmd)) != COCO_SUCCESS) {
            return ret;
        }
        if (cmd.name != "onStatus" && cmd.name != "onFCPublish") {
            continue;
        }
        std::string got = StatusCode(cmd);
        if (BadStatus(got)) {
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
