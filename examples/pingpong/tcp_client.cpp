#include "coco/net/tcp_stack.hpp"
#include "coco/net/server.hpp"
#include <iostream>
#include "coco/base/log.hpp"
#include <string>
#include "coco/base/st_socket.hpp"
#include "coco/base/error.hpp"

using namespace std;

string server_ip = "127.0.0.1";
int port = 8080;

class PingPongClient : public TcpClient
{
public:
    PingPongClient(std::string server_ip, int port, int timeout) : TcpClient(server_ip, port, timeout) {};
    virtual ~PingPongClient();
    int write(char *buf, int s);
    int read(char *buf, int s, ssize_t *nread);
};

int PingPongClient::write(char *buf, int s)
{
    ssize_t wbytes = 0;
    ssize_t nwrite = 0;
    int ret = 0;
    while(wbytes < s) {
        ret = skt->write(buf+wbytes, s-wbytes, &nwrite);
        if(ret != 0) {
            cout << "write error" << endl;
            break;
        }
        wbytes += nwrite;
    }

    return ret;
}

int PingPongClient::read(char *buf, int s, ssize_t *nread)
{
    return skt->read(buf, s, nread);
}

int main()
{
    st_init();

    PingPongClient *client = new PingPongClient(server_ip, port, 1000);
    client->connect();

    int ret = 0;
    char buf[1024] = "hello coco";
    ssize_t nread = 10;
    while(true) {
        ret = client->write(buf, nread);
        if (ret != 0) {
            cout << "write error" << endl;
            break;
        }
        ret = client->read(buf, sizeof(buf), &nread);
        if (ret != 0) {
            cout << "read error" << endl;
            break;
        }
    }

    delete client;
    return 0;
}