#pragma once

// Every call returns an error code: COCO_SUCCESS, or one of the ERROR_* values below.
#define COCO_SUCCESS 0

namespace coco {

constexpr int ERROR_SOCKET_CREATE = 1000;
constexpr int ERROR_SOCKET_SETREUSE = 1001;
constexpr int ERROR_SOCKET_BIND = 1002;
constexpr int ERROR_SOCKET_LISTEN = 1003;
constexpr int ERROR_SOCKET_CLOSED = 1004;
constexpr int ERROR_SOCKET_GET_PEER_NAME = 1005;
constexpr int ERROR_SOCKET_GET_PEER_IP = 1006;
constexpr int ERROR_SOCKET_READ = 1007;
constexpr int ERROR_SOCKET_READ_FULLY = 1008;
constexpr int ERROR_SOCKET_WRITE = 1009;
constexpr int ERROR_SOCKET_WAIT = 1010;
constexpr int ERROR_SOCKET_TIMEOUT = 1011;
constexpr int ERROR_SOCKET_CONNECT = 1012;
constexpr int ERROR_ST_SET_EPOLL = 1013;
constexpr int ERROR_ST_INITIALIZE = 1014;
constexpr int ERROR_ST_OPEN_SOCKET = 1015;
constexpr int ERROR_ST_CREATE_LISTEN_THREAD = 1016;
constexpr int ERROR_ST_CREATE_CYCLE_THREAD = 1017;
constexpr int ERROR_ST_CONNECT = 1018;
constexpr int ERROR_SYSTEM_PACKET_INVALID = 1019;
constexpr int ERROR_SYSTEM_CLIENT_INVALID = 1020;
constexpr int ERROR_SYSTEM_ASSERT_FAILED = 1021;
constexpr int ERROR_READER_BUFFER_OVERFLOW = 1022;
constexpr int ERROR_SYSTEM_CONFIG_INVALID = 1023;
constexpr int ERROR_SYSTEM_CONFIG_DIRECTIVE = 1024;
constexpr int ERROR_SYSTEM_CONFIG_BLOCK_START = 1025;
constexpr int ERROR_SYSTEM_CONFIG_BLOCK_END = 1026;
constexpr int ERROR_SYSTEM_CONFIG_EOF = 1027;
constexpr int ERROR_SYSTEM_STREAM_BUSY = 1028;
constexpr int ERROR_SYSTEM_IP_INVALID = 1029;
constexpr int ERROR_SYSTEM_FORWARD_LOOP = 1030;
constexpr int ERROR_SYSTEM_WAITPID = 1031;
constexpr int ERROR_SYSTEM_BANDWIDTH_KEY = 1032;
constexpr int ERROR_SYSTEM_BANDWIDTH_DENIED = 1033;
constexpr int ERROR_SYSTEM_PID_ACQUIRE = 1034;
constexpr int ERROR_SYSTEM_PID_ALREADY_RUNNING = 1035;
constexpr int ERROR_SYSTEM_PID_LOCK = 1036;
constexpr int ERROR_SYSTEM_PID_TRUNCATE_FILE = 1037;
constexpr int ERROR_SYSTEM_PID_WRITE_FILE = 1038;
constexpr int ERROR_SYSTEM_PID_GET_FILE_INFO = 1039;
constexpr int ERROR_SYSTEM_PID_SET_FILE_INFO = 1040;
constexpr int ERROR_SYSTEM_FILE_ALREADY_OPENED = 1041;
constexpr int ERROR_SYSTEM_FILE_OPENE = 1042;
constexpr int ERROR_SYSTEM_FILE_CLOSE = 1043;
constexpr int ERROR_SYSTEM_FILE_READ = 1044;
constexpr int ERROR_SYSTEM_FILE_WRITE = 1045;
constexpr int ERROR_SYSTEM_FILE_EOF = 1046;
constexpr int ERROR_SYSTEM_FILE_RENAME = 1047;
constexpr int ERROR_SYSTEM_CREATE_PIPE = 1048;
constexpr int ERROR_SYSTEM_FILE_SEEK = 1049;
constexpr int ERROR_SYSTEM_IO_INVALID = 1050;
constexpr int ERROR_ST_EXCEED_THREADS = 1051;
constexpr int ERROR_SYSTEM_SECURITY = 1052;
constexpr int ERROR_SYSTEM_SECURITY_DENY = 1053;
constexpr int ERROR_SYSTEM_SECURITY_ALLOW = 1054;
constexpr int ERROR_SYSTEM_TIME = 1055;
constexpr int ERROR_SYSTEM_DIR_EXISTS = 1056;
constexpr int ERROR_SYSTEM_CREATE_DIR = 1057;
constexpr int ERROR_SYSTEM_KILL = 1058;
constexpr int ERROR_SYSTEM_DNS_RESOLVE = 1059;
constexpr int ERROR_THREAD_INTERRUPED = 1070;
constexpr int ERROR_THREAD_TERMINATED = 1071;
constexpr int ERROR_THREAD_DISPOSED = 1069;
constexpr int ERROR_THREAD_DUMMY = 1072;
constexpr int ERROR_ASPROCESS_PPID = 1073;
constexpr int ERROR_EXCEED_CONNECTIONS = 1074;
constexpr int ERROR_SOCKET_SETKEEPALIVE = 1075;
constexpr int ERROR_SOCKET_NO_NODELAY = 1076;
constexpr int ERROR_SOCKET_SNDBUF = 1077;
constexpr int ERROR_THREAD_STARTED = 1078;
constexpr int ERROR_SOCKET_SETREUSEADDR = 1079;
constexpr int ERROR_SOCKET_SETCLOSEEXEC = 1080;
constexpr int ERROR_SOCKET_ACCEPT = 1081;
constexpr int ERROR_THREAD_BUSY = 1082;

constexpr int ERROR_HTTP_PARSE_URI = 3007;
constexpr int ERROR_HTTP_DATA_INVALID = 3008;
constexpr int ERROR_HTTP_PARSE_HEADER = 3009;
constexpr int ERROR_HTTP_HANDLER_MATCH_URL = 3010;
constexpr int ERROR_HTTP_HANDLER_INVALID = 3011;
constexpr int ERROR_HTTP_API_LOGS = 3012;
constexpr int ERROR_HTTP_REMUX_SEQUENCE_HEADER = 3013;
constexpr int ERROR_HTTP_REMUX_OFFSET_OVERFLOW = 3014;

constexpr int ERROR_HTTP_PATTERN_EMPTY = 4000;
constexpr int ERROR_HTTP_PATTERN_DUPLICATED = 4001;
constexpr int ERROR_HTTP_URL_NOT_CLEAN = 4002;
constexpr int ERROR_HTTP_CONTENT_LENGTH = 4003;
constexpr int ERROR_HTTP_LIVE_STREAM_EXT = 4004;
constexpr int ERROR_HTTP_STATUS_INVALID = 4005;
constexpr int ERROR_HTTP_RESPONSE_EOF = 4025;
constexpr int ERROR_HTTP_INVALID_CHUNK_HEADER = 4026;
constexpr int ERROR_HTTP_REQUEST_EOF = 4029;
// the whole message body has been read.
constexpr int ERROR_HTTP_BODY_EOF = 4030;
// the header block is larger than the configured limit.
constexpr int ERROR_HTTP_HEADER_TOO_LARGE = 4031;
// the response writer's connection was taken over with Hijack.
constexpr int ERROR_HTTP_HIJACKED = 4032;
// a body was written for a status that does not allow one, e.g. 204 or 304.
constexpr int ERROR_HTTP_BODY_NOT_ALLOWED = 4033;
constexpr int ERROR_HTTP_TOO_MANY_REDIRECTS = 4034;
constexpr int ERROR_HTTPS_NOT_SUPPORTED = 4041;
constexpr int ERROR_HTTPS_HANDSHAKE = 4042;
constexpr int ERROR_HTTPS_READ = 4043;
constexpr int ERROR_HTTPS_WRITE = 4044;
constexpr int ERROR_HTTPS_KEY_CRT = 4045;
// the peer broke RFC 6455 framing rules.
constexpr int ERROR_WS_PROTOCOL = 4051;
// a frame or reassembled message is larger than MAX_WS_PACKET.
constexpr int ERROR_WS_MESSAGE_TOO_LARGE = 4052;
// the peer sent a close frame and it has been answered.
constexpr int ERROR_WS_CLOSED = 4053;
// the peer broke the RTMP chunk stream, command or handshake rules.
constexpr int ERROR_RTMP_PROTOCOL = 4061;
constexpr int ERROR_RTMP_HANDSHAKE = 4062;
// one reassembled RTMP message is larger than kRtmpMaxMessage.
constexpr int ERROR_RTMP_MESSAGE_TOO_LARGE = 4063;
constexpr int ERROR_RTMP_AMF = 4064;
constexpr int ERROR_RTMP_URL = 4065;
// the name does not exist (NXDOMAIN), or has no address of the requested family.
constexpr int ERROR_DNS_NOT_FOUND = 4071;
// every name server failed the query (SERVFAIL, REFUSED or another error code).
constexpr int ERROR_DNS_SERVER = 4072;
// a malformed DNS message.
constexpr int ERROR_DNS_PROTOCOL = 4073;
// the host is not a valid domain name.
constexpr int ERROR_DNS_BAD_NAME = 4074;
// no name server answered within the timeout and attempts.
constexpr int ERROR_DNS_TIMEOUT = 4075;
// the RUDP peer refused the connection or no longer has it (RST).
constexpr int ERROR_RUDP_RESET = 4081;
// the RUDP handshake got no answer, or the peer stayed silent past link_timeout_us.
constexpr int ERROR_RUDP_TIMEOUT = 4082;
// the RUDP connection was closed or aborted on this side.
constexpr int ERROR_RUDP_CLOSED = 4083;

bool coco_is_client_gracefully_close(int error_code);

}  // namespace coco
