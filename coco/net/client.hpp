#ifndef NET_CLIENT_HPP
#define NET_CLIENT_HPP

class Client
{
public:
    Client();
    virtual ~Client();
    virtual int connect()=0;
};

#endif