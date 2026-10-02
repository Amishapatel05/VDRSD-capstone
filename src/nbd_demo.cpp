// P2 self-check: NbdServer over loopback TCP (no kernel nbd needed).
// Run: ./nbd_demo — asserts handshake, aligned + unaligned R/W, OOB EIO, FLUSH.
#include <arpa/inet.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <iostream>
#include <string>
#include <thread>

#include "block_store.h"
#include "nbd_server.h"

namespace {

bool SendAll(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t w = ::send(fd, p, n, 0);
    if (w <= 0) return false;
    p += w;
    n -= static_cast<size_t>(w);
  }
  return true;
}

bool RecvAll(int fd, char* p, size_t n) {
  while (n) {
    ssize_t r = ::recv(fd, p, n, 0);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

int Dial(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(static_cast<uint16_t>(port));
  for (int i = 0; i < 50 && ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  return fd;
}

void Req(int fd, uint16_t type, uint64_t handle, uint64_t off, uint32_t len, const char* data = nullptr) {
  char h[28];
  uint32_t m = htobe32(0x25609513), l = htobe32(len);
  uint16_t f = 0, t = htobe16(type);
  uint64_t hh = htobe64(handle), o = htobe64(off);
  memcpy(h, &m, 4);
  memcpy(h + 4, &f, 2);
  memcpy(h + 6, &t, 2);
  memcpy(h + 8, &hh, 8);
  memcpy(h + 16, &o, 8);
  memcpy(h + 24, &l, 4);
  assert(SendAll(fd, h, sizeof h));
  if (data) assert(SendAll(fd, data, len));
}

uint32_t RepErr(int fd, uint64_t want_handle, std::string* data = nullptr, uint32_t want_len = 0) {
  char h[16];
  assert(RecvAll(fd, h, sizeof h));
  uint32_t magic, err;
  uint64_t handle;
  memcpy(&magic, h, 4);
  memcpy(&err, h + 4, 4);
  memcpy(&handle, h + 8, 8);
  assert(be32toh(magic) == 0x67446698);
  assert(be64toh(handle) == want_handle);
  if (data) {
    data->resize(want_len);
    assert(RecvAll(fd, data->data(), want_len));
  }
  return be32toh(err);
}

}  // namespace

int main() {
  const int kPort = 18099;
  std::remove("/tmp/vdr_nbd_demo.blk");
  BlockStore store;
  std::string err;
  assert(store.Open("/tmp/vdr_nbd_demo.blk", 16, &err));
  NbdServer srv(&store, kPort);
  std::thread([&] { std::string e;
    srv.Run(&e); }).detach();

  int fd = Dial(kPort);
  char hs[8 + 8 + 8 + 4 + 124];  // handshake
  assert(RecvAll(fd, hs, sizeof hs));
  uint64_t pw, mg, size;
  memcpy(&pw, hs, 8);
  memcpy(&mg, hs + 8, 8);
  memcpy(&size, hs + 16, 8);
  assert(be64toh(pw) == 0x4E42444D41474943ULL);
  assert(be64toh(mg) == 0x0000420281861253ULL);
  assert(be64toh(size) == 16 * 4096);

  std::string blk(4096, 'A');  // aligned write + read
  Req(fd, 1, 7, 0, 4096, blk.data());
  assert(RepErr(fd, 7) == 0);
  std::string got;
  Req(fd, 0, 8, 0, 4096);
  assert(RepErr(fd, 8, &got, 4096) == 0 && got == blk);

  std::string frag(200, 'q');  // unaligned write straddling blocks 1->2
  Req(fd, 1, 9, 4096 - 100, 200, frag.data());
  assert(RepErr(fd, 9) == 0);
  Req(fd, 0, 10, 4096 - 100, 200);
  assert(RepErr(fd, 10, &got, 200) == 0 && got == frag);

  Req(fd, 0, 11, 16 * 4096, 512);  // OOB read -> EIO
  assert(RepErr(fd, 11) == 5);
  Req(fd, 3, 12, 0, 0);  // FLUSH
  assert(RepErr(fd, 12) == 0);
  Req(fd, 2, 13, 0, 0);  // DISC closes
  ::close(fd);
  std::remove("/tmp/vdr_nbd_demo.blk");
  std::cout << "nbd_demo: OK\n";
  return 0;
}
