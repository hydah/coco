#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coco/coco.h"

using namespace coco;

namespace {

struct Subscriber {
    std::deque<RtmpMessage> q;
    bool closed = false;
};

struct Stream {
    bool publishing = false;
    bool has_meta = false;
    bool has_aac = false;
    bool has_avc = false;
    RtmpMessage meta;
    RtmpMessage aac;
    RtmpMessage avc;
    std::deque<RtmpMessage> gop;
    std::vector<Subscriber*> subs;
};

bool AudioSeq(const std::string& p) {
    return p.size() >= 2 && (uint8_t)p[0] >> 4 == 10 && (uint8_t)p[1] == 0;
}

bool VideoSeq(const std::string& p) {
    if (p.size() < 2) {
        return false;
    }
    uint8_t codec = (uint8_t)p[0] & 0x0f;
    return (codec == 7 || codec == 12) && (uint8_t)p[1] == 0;
}

bool Keyframe(const std::string& p) { return !p.empty() && ((uint8_t)p[0] & 0xf0) == 0x10; }

class Hub {
 public:
    int Serve(RtmpConn& conn, const RtmpRequest& req) {
        Stream* s = Open(req.app + "/" + req.stream);
        if (req.publish) {
            return Publish(conn, s);
        }
        return Play(conn, req.stream_id, s);
    }

 private:
    Stream* Open(const std::string& key) {
        std::unique_ptr<Stream>& slot = streams_[key];
        if (!slot) {
            slot.reset(new Stream());
        }
        return slot.get();
    }

    static void Remember(Stream* s, const RtmpMessage& msg) {
        if (msg.type == RTMP_MSG_DATA_AMF0 || msg.type == RTMP_MSG_DATA_AMF3) {
            s->meta = msg;
            s->has_meta = true;
            return;
        }
        if (msg.type == RTMP_MSG_AUDIO && AudioSeq(msg.payload)) {
            s->aac = msg;
            s->has_aac = true;
            return;
        }
        if (msg.type == RTMP_MSG_VIDEO && VideoSeq(msg.payload)) {
            s->avc = msg;
            s->has_avc = true;
            return;
        }
        if (msg.type == RTMP_MSG_VIDEO && Keyframe(msg.payload)) {
            s->gop.clear();
        }
        if (msg.type == RTMP_MSG_AUDIO || msg.type == RTMP_MSG_VIDEO) {
            s->gop.push_back(msg);
            while (s->gop.size() > 512) {
                s->gop.pop_front();
            }
        }
    }

    static void Fanout(Stream* s, const RtmpMessage& msg) {
        for (size_t i = 0; i < s->subs.size();) {
            Subscriber* sub = s->subs[i];
            if (sub->q.size() >= 256) {
                sub->closed = true;
                s->subs.erase(s->subs.begin() + (long)i);
                continue;
            }
            sub->q.push_back(msg);
            i++;
        }
    }

    static int WriteOne(RtmpConn& conn, uint32_t stream_id, const RtmpMessage& in) {
        RtmpMessage msg = in;
        msg.stream_id = stream_id;
        msg.csid = 0;
        return conn.WriteMessage(msg);
    }

    int Publish(RtmpConn& conn, Stream* s) {
        if (s->publishing) {
            coco_warn("rtmp: stream already has a publisher");
            return ERROR_RTMP_PROTOCOL;
        }
        s->publishing = true;
        s->has_meta = s->has_aac = s->has_avc = false;
        s->gop.clear();
        RtmpMessage msg;
        int ret = COCO_SUCCESS;
        while ((ret = conn.ReadMessage(&msg)) == COCO_SUCCESS) {
            if (msg.type != RTMP_MSG_AUDIO && msg.type != RTMP_MSG_VIDEO &&
                msg.type != RTMP_MSG_DATA_AMF0 && msg.type != RTMP_MSG_DATA_AMF3) {
                continue;
            }
            Remember(s, msg);
            Fanout(s, msg);
        }
        s->publishing = false;
        for (size_t i = 0; i < s->subs.size(); i++) {
            s->subs[i]->closed = true;
        }
        s->subs.clear();
        return COCO_SUCCESS;
    }

    int Play(RtmpConn& conn, uint32_t stream_id, Stream* s) {
        Subscriber sub;
        s->subs.push_back(&sub);
        std::vector<RtmpMessage> head;
        if (s->has_meta) {
            head.push_back(s->meta);
        }
        if (s->has_aac) {
            head.push_back(s->aac);
        }
        if (s->has_avc) {
            head.push_back(s->avc);
        }
        head.insert(head.end(), s->gop.begin(), s->gop.end());

        int ret = COCO_SUCCESS;
        for (size_t i = 0; i < head.size() && ret == COCO_SUCCESS; i++) {
            ret = WriteOne(conn, stream_id, head[i]);
        }
        while (ret == COCO_SUCCESS && !CocoShouldStop()) {
            if (sub.q.empty()) {
                if (sub.closed) {
                    Amf0Value info = Amf0Value::Object();
                    info.Put("level", Amf0Value::Str("status"));
                    info.Put("code", Amf0Value::Str("NetStream.Play.UnpublishNotify"));
                    info.Put("description", Amf0Value::Str("publisher stopped"));
                    std::vector<Amf0Value> v;
                    v.push_back(Amf0Value::Str("onStatus"));
                    v.push_back(Amf0Value::Num(0));
                    v.push_back(Amf0Value::Null());
                    v.push_back(info);
                    RtmpMessage bye;
                    bye.type = RTMP_MSG_COMMAND_AMF0;
                    bye.payload = Amf0Encode(v);
                    WriteOne(conn, stream_id, bye);
                    break;
                }
                CocoSleepMs(1);
                continue;
            }
            RtmpMessage msg = sub.q.front();
            sub.q.pop_front();
            ret = WriteOne(conn, stream_id, msg);
        }
        for (size_t i = 0; i < s->subs.size(); i++) {
            if (s->subs[i] == &sub) {
                s->subs.erase(s->subs.begin() + (long)i);
                break;
            }
        }
        return ret == COCO_SUCCESS || sub.closed ? COCO_SUCCESS : ret;
    }

    std::map<std::string, std::unique_ptr<Stream>> streams_;
};

}  // namespace

// Live relay. Publish and play the same path:
//   ffmpeg -re -i in.flv -c copy -f flv rtmp://127.0.0.1:1935/live/stream
//   ffplay rtmp://127.0.0.1:1935/live/stream
int main() {
    CocoInit();
    Hub hub;
    RtmpServer server([&hub](RtmpConn& conn, const RtmpRequest& req) {
        std::cout << (req.publish ? "publish " : "play ") << req.app << "/" << req.stream
                  << " from " << conn.RemoteAddr() << std::endl;
        return hub.Serve(conn, req);
    });
    std::cout << "rtmp://0.0.0.0:1935/{app}/{stream}" << std::endl;
    if (server.ListenAndServe("0.0.0.0", 1935) != COCO_SUCCESS) {
        return 1;
    }
    return 0;
}
