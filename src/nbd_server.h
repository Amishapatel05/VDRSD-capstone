#pragma once
// P2: NBD oldstyle server (Linux, stdlib+POSIX only, no extra deps).
// Thread-per-connection blocking sockets; P4 adds leader gate + quorum ACK.
// ponytail: thread-per-conn; pool only if many concurrent clients measured.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "block_store.h"

class NbdServer {
 public:
  NbdServer(BlockStore* store, int port) : store_(store), port_(port) {}

  // P4: quorum path. When set, followers RST at connect, every request
  // re-checks leadership (stepdown-safe), and writes commit via append().
  // Unset = P2 direct-to-store (keeps nbd_demo unchanged).
  void SetQuorum(std::function<bool()> is_leader,
                 std::function<bool(uint64_t, const char*)> append) {
    is_leader_ = std::move(is_leader);
    append_ = std::move(append);
  }

  // Blocks: accept loop. Returns only on fatal socket error.
  bool Run(std::string* err) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return Fail("socket", err);
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
      ::close(srv);
      return Fail("bind", err);
    }
    if (::listen(srv, 16) != 0) {
      ::close(srv);
      return Fail("listen", err);
    }
    for (;;) {
      int c = ::accept(srv, nullptr, nullptr);
      if (c < 0) {
        ::close(srv);
        return Fail("accept", err);
      }
      std::thread(&NbdServer::Session, this, c).detach();
    }
  }

 private:
  static constexpr uint64_t kInitPasswd = 0x4E42444D41474943ULL;  // "NBDMAGIC"
  static constexpr uint64_t kOldMagic = 0x0000420281861253ULL;  // (kept for reference; V1 serves fixed-newstyle)
  static constexpr uint64_t kOptMagic = 0x49484156454F5054ULL;  // "IHAVEOPT"
  static constexpr uint64_t kRepMagic = 0x3E889045565A9ULL;
  static constexpr uint32_t kReqMagic = 0x25609513;
  static constexpr uint32_t kNbdRepMagic = 0x67446698;
  static constexpr uint16_t kHsFixedNewstyle = 0x0003;  // FIXED_NEWSTYLE|NO_ZEROES
  static constexpr uint32_t kOptExportName = 1;
  static constexpr uint32_t kOptAbort = 2;
  static constexpr uint32_t kOptList = 3;
  static constexpr uint32_t kOptInfo = 6;
  static constexpr uint32_t kOptGo = 7;
  static constexpr uint32_t kRepAck = 1;
  static constexpr uint32_t kRepServer = 2;
  static constexpr uint32_t kRepInfo = 3;
  static constexpr uint32_t kRepErrUnsup = 0x80000002;  // no TLS / no other opts in V1
  static constexpr uint16_t kFlagHasFlags = 0x0001;
  static constexpr uint16_t kFlagSendFlush = 0x0004;
  static constexpr int kEio = 5;
  static constexpr uint32_t kMaxPayload = 1 << 20;  // cap per-request alloc (trust boundary).

  static bool SendAll(int fd, const char* p, std::size_t n) {
    while (n) {
      ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
      if (w <= 0) return false;
      p += w;
      n -= static_cast<std::size_t>(w);
    }
    return true;
  }

  static bool RecvAll(int fd, char* p, std::size_t n) {
    while (n) {
      ssize_t r = ::recv(fd, p, n, 0);
      if (r <= 0) return false;
      p += r;
      n -= static_cast<std::size_t>(r);
    }
    return true;
  }

  static void PutU64(char*& p, uint64_t v) {
    const uint64_t b = htobe64(v);
    std::memcpy(p, &b, 8);
    p += 8;
  }
  static void PutU32(char*& p, uint32_t v) {
    const uint32_t b = htobe32(v);
    std::memcpy(p, &b, 4);
    p += 4;
  }
  static void PutU16(char*& p, uint16_t v) {
    const uint16_t b = htobe16(v);
    std::memcpy(p, &b, 2);
    p += 2;
  }

  bool Reply(int fd, uint64_t handle, uint32_t error, const char* data, std::size_t n) {
    char h[16];
    char* p = h;
    PutU32(p, kNbdRepMagic);
    PutU32(p, error);
    PutU64(p, handle);
    return SendAll(fd, h, sizeof h) && (n == 0 || SendAll(fd, data, n));
  }

  static void AppendU16(std::string& s, uint16_t v) {
    const uint16_t b = htobe16(v);
    s.append(reinterpret_cast<const char*>(&b), 2);
  }
  static void AppendU32(std::string& s, uint32_t v) {
    const uint32_t b = htobe32(v);
    s.append(reinterpret_cast<const char*>(&b), 4);
  }
  static void AppendU64(std::string& s, uint64_t v) {
    const uint64_t b = htobe64(v);
    s.append(reinterpret_cast<const char*>(&b), 8);
  }

  bool SendOptReply(int fd, uint32_t opt, uint32_t type, const std::string& payload) {
    std::string h;
    AppendU64(h, kRepMagic);
    AppendU32(h, opt);
    AppendU32(h, type);
    AppendU32(h, static_cast<uint32_t>(payload.size()));
    return SendAll(fd, h.data(), h.size()) &&
           (payload.empty() || SendAll(fd, payload.data(), payload.size()));
  }

  // Fixed-newstyle negotiation: single export (any name accepted), no TLS.
  // Returns true once the connection enters the transmission phase.
  bool Negotiate(int fd) {
    char g[18];
    char* p = g;
    PutU64(p, kInitPasswd);
    PutU64(p, kOptMagic);
    PutU16(p, kHsFixedNewstyle);
    if (!SendAll(fd, g, sizeof g)) return false;
    for (;;) {
      char oh[16];
      if (!RecvAll(fd, oh, sizeof oh)) return false;
      uint64_t magic;
      uint32_t id, len;
      std::memcpy(&magic, oh, 8);
      std::memcpy(&id, oh + 8, 4);
      std::memcpy(&len, oh + 12, 4);
      magic = be64toh(magic);
      id = be32toh(id);
      len = be32toh(len);
      if (magic != kOptMagic) return false;
      if (len > 1 << 20) return false;
      std::string data(len, '\0');
      if (len && !RecvAll(fd, data.data(), len)) return false;
      if (id == kOptAbort) {
        SendOptReply(fd, id, kRepAck, "");
        return false;
      }
      if (id == kOptExportName) return true;  // legacy: straight to transmission.
      if (id == kOptInfo || id == kOptGo) {
        std::string info;  // NBD_INFO_EXPORT: type(0) + size + tx flags.
        AppendU16(info, 0);
        AppendU64(info, store_->num_blocks() * BlockStore::kBlock);
        AppendU16(info, kFlagHasFlags | kFlagSendFlush);
        if (!SendOptReply(fd, id, kRepInfo, info)) return false;
        if (!SendOptReply(fd, id, kRepAck, "")) return false;
        if (id == kOptGo) return true;
        continue;
      }
      if (id == kOptList) {  // one empty-named server, then ACK.
        std::string srv;
        AppendU32(srv, 0);
        if (!SendOptReply(fd, id, kRepServer, srv)) return false;
        if (!SendOptReply(fd, id, kRepAck, "")) return false;
        continue;
      }
      if (!SendOptReply(fd, id, kRepErrUnsup, "")) return false;  // STARTTLS + unknown.
    }
  }

  // Arbitrary (offset,len) over 4K blocks; write path is read-modify-write.
  bool ReadRange(uint64_t off, uint32_t len, std::string& out) {
    const uint64_t n = store_->num_blocks() * BlockStore::kBlock;
    if (len == 0 || len > kMaxPayload || off + len > n || off + len < off) return false;
    out.resize(len);
    char blk[BlockStore::kBlock];
    std::string err;
    std::size_t done = 0;
    while (done < len) {
      const uint64_t pos = off + done;
      const uint64_t b = pos / BlockStore::kBlock;
      const std::size_t bis = static_cast<std::size_t>(pos % BlockStore::kBlock);
      const std::size_t cp = std::min<std::size_t>(BlockStore::kBlock - bis, len - done);
      if (!store_->Read(b, blk, &err)) return false;
      std::memcpy(&out[done], blk + bis, cp);
      done += cp;
    }
    return true;
  }

  bool WriteRange(uint64_t off, uint32_t len, const char* data) {
    const uint64_t n = store_->num_blocks() * BlockStore::kBlock;
    if (len == 0 || len > kMaxPayload || off + len > n || off + len < off) return false;
    char blk[BlockStore::kBlock];
    std::string err;
    std::size_t done = 0;
    while (done < len) {
      const uint64_t pos = off + done;
      const uint64_t b = pos / BlockStore::kBlock;
      const std::size_t bis = static_cast<std::size_t>(pos % BlockStore::kBlock);
      const std::size_t cp = std::min<std::size_t>(BlockStore::kBlock - bis, len - done);
      if (cp != BlockStore::kBlock) {
        if (!store_->Read(b, blk, &err)) return false;
      } else {
        std::memset(blk, 0, sizeof blk);
      }
      std::memcpy(blk + bis, data + done, cp);
      if (!store_->Write(b, blk, &err)) return false;
      done += cp;
    }
    return store_->Fsync(&err);  // ponytail: fsync per write; group-commit when bench says so.
  }

  bool WriteBlocks(uint64_t off, uint32_t len, const char* data) {
    return quorum() ? QuorumWrite(off, len, data) : WriteRange(off, len, data);
  }

  // One Raft log per 4K block; commit applies+fsyncs on every node (incl. leader),
  // so no local write or fsync here. RMW reads hit leader's committed state.
  // ponytail: Append blocks until commit (quorum loss hangs the NBD op;
  // client timeout surfaces EIO). Timed-append only if that ever bites.
  bool QuorumWrite(uint64_t off, uint32_t len, const char* data) {
    const uint64_t n = store_->num_blocks() * BlockStore::kBlock;
    if (len == 0 || len > kMaxPayload || off + len > n || off + len < off) return false;
    char blk[BlockStore::kBlock];
    std::string err;
    std::size_t done = 0;
    while (done < len) {
      const uint64_t pos = off + done;
      const uint64_t b = pos / BlockStore::kBlock;
      const std::size_t bis = static_cast<std::size_t>(pos % BlockStore::kBlock);
      const std::size_t cp = std::min<std::size_t>(BlockStore::kBlock - bis, len - done);
      if (cp != BlockStore::kBlock && !store_->Read(b, blk, &err)) return false;
      std::memcpy(blk + bis, data + done, cp);
      if (!append_(b, blk)) return false;
      done += cp;
    }
    return true;
  }

  void Session(int fd) {
    if (quorum() && !is_leader_()) {
      ::close(fd);  // no follower forwarding: connector re-points nbd-client at leader.
      return;
    }
    if (!Negotiate(fd)) {
      ::close(fd);
      return;
    }
    for (;;) {
      char rh[28];
      if (!RecvAll(fd, rh, sizeof rh)) break;
      uint32_t magic, length;
      uint16_t flags, type;
      uint64_t handle, offset;
      std::memcpy(&magic, rh, 4);
      std::memcpy(&flags, rh + 4, 2);
      std::memcpy(&type, rh + 6, 2);
      std::memcpy(&handle, rh + 8, 8);
      std::memcpy(&offset, rh + 16, 8);
      std::memcpy(&length, rh + 24, 4);
      magic = be32toh(magic);
      type = be16toh(type);
      handle = be64toh(handle);
      offset = be64toh(offset);
      length = be32toh(length);
      (void)flags;
      if (magic != kReqMagic) break;
      if (quorum() && !is_leader_()) break;  // stepped down mid-connection: drop, don't serve stale.
      if (type == 2) break;  // DISC
      if (type == 3) {       // FLUSH
        std::string err;
        Reply(fd, handle, store_->Fsync(&err) ? 0 : kEio, nullptr, 0);
        continue;
      }
      if (type == 0) {  // READ
        std::string out;
        if (!ReadRange(offset, length, out)) {
          if (!Reply(fd, handle, kEio, nullptr, 0)) break;
        } else if (!Reply(fd, handle, 0, out.data(), out.size())) {
          break;
        }
        continue;
      }
      if (type == 1) {  // WRITE (drain-then-EIO on oversize to keep stream in sync)
        if (length == 0 || length > kMaxPayload) {
          char tmp[4096];
          uint32_t left = length;
          while (left) {
            const uint32_t c = std::min(left, static_cast<uint32_t>(sizeof tmp));
            if (!RecvAll(fd, tmp, c)) break;
            left -= c;
          }
          if (left || !Reply(fd, handle, kEio, nullptr, 0)) break;
          continue;
        }
        std::string payload(length, '\0');
        if (!RecvAll(fd, payload.data(), length)) break;
        if (!WriteBlocks(offset, length, payload.data())) {
          if (!Reply(fd, handle, kEio, nullptr, 0)) break;
        } else if (!Reply(fd, handle, 0, nullptr, 0)) {
          break;
        }
        continue;
      }
      break;  // unknown type
    }
    ::close(fd);
  }

  static bool Fail(const std::string& what, std::string* err) {
    if (err) *err = what + ": " + std::string(::strerror(errno));
    return false;
  }

  BlockStore* store_;
  int port_;
  std::function<bool()> is_leader_;
  std::function<bool(uint64_t, const char*)> append_;
  bool quorum() const { return static_cast<bool>(is_leader_); }
};
