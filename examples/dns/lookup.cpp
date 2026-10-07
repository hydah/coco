#include <iostream>
#include <string>
#include <vector>

#include "coco/coco.h"

using namespace coco;

// Resolves every name given, all at once, each on a coroutine of its own: a lookup only
// suspends its coroutine while it waits for the name server, so they overlap.
//
//   lookup example.com localhost 127.0.0.1
int main(int argc, char **argv) {
    std::vector<std::string> hosts(argv + 1, argv + argc);
    if (hosts.empty()) {
        hosts = {"localhost", "example.com"};
    }
    return CocoRun([&hosts]() {
        TaskGroup lookups;
        for (const std::string &host : hosts) {
            lookups.Spawn([host]() {
                std::vector<std::string> addrs;
                int ret = LookupHost(host, &addrs);
                std::cout << host << ":";
                if (ret != COCO_SUCCESS) {
                    std::cout << " error " << ret;
                }
                for (const std::string &a : addrs) {
                    std::cout << " " << a;
                }
                std::cout << std::endl;
                return ret;
            });
        }
        return lookups.Wait() == COCO_SUCCESS ? 0 : 1;
    });
}
