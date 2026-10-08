#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include "coco/common/error.hpp"
#include "coco/net/rudp/codec/packet.hpp"

namespace coco {

// Fixed when a connection or listener is created; a connection keeps a copy. Values out
// of range are clamped.
struct RudpOptions {
    // How often the endpoint runs retransmission and liveness timers.
    int64_t interval_us = 10 * 1000;
    // Retransmission timeout before the first RTT sample, and its bounds.
    int64_t initial_rto_us = 200 * 1000;
    int64_t min_rto_us = 100 * 1000;
    int64_t max_rto_us = 2 * 1000 * 1000;
    // The link is dead when something sent stays unacknowledged and nothing at all is heard
    // from the peer for this long. Also bounds Close() and a Dial with kNoTimeout.
    int64_t link_timeout_us = 10 * 1000 * 1000;
    // Segments (of up to kRudpMss bytes) in flight, and buffered on the receive side.
    int send_window = 128;
    int recv_window = 128;
    // Listener only: half-open connections plus established ones not accepted yet.
    int backlog = 128;
};

struct RudpStats {
    uint64_t segments_sent = 0;      // first transmissions of DATA and FIN
    uint64_t rto_resends = 0;        // retransmissions on timeout
    uint64_t fast_resends = 0;       // retransmissions after kRudpFastResend later ACKs
    uint64_t segments_received = 0;  // DATA and FIN taken into the receive buffer
    uint64_t duplicates = 0;         // DATA and FIN received again
    uint64_t congestion_events = 0;  // rounds of loss that cut the congestion window
    int64_t srtt_us = 0;             // 0 until the first sample
    int64_t rto_us = 0;
    int cwnd = 0;                    // congestion window, in segments
    int ssthresh = 0;
    int in_flight = 0;               // segments sent and not yet acknowledged in order
};

// Later ACKs that make an unacknowledged segment be sent again before its timeout.
constexpr int kRudpFastResend = 3;
constexpr int kRudpMaxWindow = 1024;
constexpr int kRudpInitialCwnd = 4;
constexpr int kRudpMinSsthresh = 2;

RudpOptions ClampRudpOptions(const RudpOptions &o);

// The protocol state of one connection: handshake, sequencing, acknowledgement,
// retransmission, flow and congestion control, close. It is driven by Input (a packet
// for this connection), Update (timers) and the Send / Recv / Close calls, each given the
// current time, and leaves the datagrams to send in TakeOutput(). It does no I/O and reads
// no clock. See .harness/docs/rudp.md.
class RudpControl {
 public:
    enum class State { kSynSent, kSynRcvd, kEstablished, kClosed };

    // The client side: emits SYN with isn as its initial sequence number.
    RudpControl(const RudpOptions &options, uint32_t conn_id, uint32_t isn, int64_t now);
    // The server side of a received SYN: emits SYN_ACK with isn.
    RudpControl(const RudpOptions &options, const RudpPacket &syn, uint32_t isn, int64_t now);

    RudpControl(const RudpControl &) = delete;
    RudpControl &operator=(const RudpControl &) = delete;

    // p must carry this connection's conn_id.
    void Input(const RudpPacket &p, int64_t now);
    // Retransmissions and the link timeout.
    void Update(int64_t now);

    // Takes up to size bytes into the send buffer and returns how many; 0 once it is full,
    // or unless established and not closing. Flush() sends what the windows allow.
    size_t Send(const void *data, size_t size);
    void Flush(int64_t now);
    // Copies up to size bytes received in order and returns how many, possibly 0.
    size_t Recv(void *buf, size_t size);
    // Queues FIN after everything sent; the close completes as CloseDone() describes.
    void Close(int64_t now);
    // Emits RST and ends the connection at once.
    void Abort();

    // Datagrams produced since the last call, in the order they are to be sent.
    std::vector<std::string> TakeOutput();

    State state() const { return state_; }
    uint32_t conn_id() const { return conn_id_; }
    bool Established() const { return state_ == State::kEstablished; }
    // Bytes are waiting for Recv().
    bool Readable() const { return !rcv_queue_.empty(); }
    // The peer's FIN came in order and every byte before it has been read.
    bool Eof() const { return peer_fin_ && rcv_queue_.empty(); }
    // Send() would take at least one byte.
    bool CanSend() const;
    bool CloseRequested() const { return close_requested_; }
    // Closed after Close(), with every byte written acknowledged by the peer and the peer
    // told: our FIN acknowledged, the peer's FIN received, or the peer's RST.
    bool CloseDone() const {
        return state_ == State::kClosed && result_ == COCO_SUCCESS && close_requested_;
    }
    // Why the connection ended: ERROR_RUDP_RESET, ERROR_RUDP_TIMEOUT, ERROR_RUDP_CLOSED
    // (aborted); COCO_SUCCESS while it lives and after a completed close.
    int Error() const { return state_ == State::kClosed ? result_ : COCO_SUCCESS; }
    RudpStats Stats() const;

 private:
    struct Segment {
        uint32_t seq = 0;
        bool fin = false;
        std::string data;
        int xmit = 0;
        int64_t first_sent_us = 0;
        int64_t sent_us = 0;
        int64_t rto_us = 0;
        int64_t resend_us = 0;
        int skipped = 0;
        bool fast_resent = false;
        bool acked = false;
    };
    struct Slot {
        bool used = false;
        uint32_t seq = 0;
        bool fin = false;
        std::string data;
    };

    void Init(uint32_t isn, int64_t now);
    void InputEstablished(const RudpPacket &p, int64_t now);
    void OnAck(const RudpPacket &p, int64_t now);
    void OnData(const RudpPacket &p);
    void OnRst();
    void OnAcked(int segments);
    void OnLoss(const Segment &seg, bool timeout);
    void UpdateRtt(int64_t rtt_us);
    void Transmit(Segment &seg, int64_t now);
    void EmitHandshake(int64_t now);
    void CheckCloseDone();
    void CheckLink(int64_t now);
    bool DataAcked() const;
    int AdvWindow() const;
    void Out(RudpType type, uint32_t seq, const std::string &payload = std::string());
    void OutAck(uint32_t seq) { Out(RudpType::kAck, seq); }
    void OutPureAck() { Out(RudpType::kAck, rcv_nxt_ - 1); }
    void OutRst();
    void End(int result);

    RudpOptions opt_;
    bool client_;
    uint32_t conn_id_;
    State state_;
    int result_ = COCO_SUCCESS;
    bool close_requested_ = false;
    int64_t close_us_ = 0;
    int64_t heard_us_ = 0;

    uint32_t local_isn_ = 0;
    uint32_t peer_isn_ = 0;
    // SYN or SYN_ACK retransmission.
    int hs_xmit_ = 0;
    int64_t hs_first_sent_us_ = 0;
    int64_t hs_sent_us_ = 0;
    int64_t hs_rto_us_ = 0;
    int64_t hs_resend_us_ = 0;

    std::deque<Segment> snd_queue_;
    // Exactly the sequence numbers [snd_una_, snd_nxt_), in order.
    std::deque<Segment> snd_buf_;
    uint32_t snd_una_ = 0;
    uint32_t snd_nxt_ = 0;
    uint16_t rmt_wnd_ = 0;
    int cwnd_ = 0;
    int cwnd_cnt_ = 0;
    int ssthresh_ = 0;
    // snd_nxt_ when the window was last cut: losses before it belong to the same round.
    uint32_t recover_ = 0;
    int64_t srtt_us_ = 0;
    int64_t rttvar_us_ = 0;
    int64_t rto_us_ = 0;

    // Segment seq waits in rcv_ring_[seq % size] until everything before it came; the size
    // is recv_window rounded up to a power of two.
    std::vector<Slot> rcv_ring_;
    std::deque<std::string> rcv_queue_;
    size_t rcv_front_off_ = 0;
    uint32_t rcv_nxt_ = 0;
    bool fin_known_ = false;
    uint32_t fin_seq_ = 0;
    bool peer_fin_ = false;
    uint16_t last_adv_wnd_ = 0;

    RudpStats stats_;
    std::vector<std::string> out_;
};

}  // namespace coco
