#pragma once
// P4: sequential HTTP for GET /leader /health /metrics. Plain text, no JSON lib:
// /leader body is "host:port" (what connect.sh feeds to nbd-client), 503 when unknown.
// ponytail: sequential accept loop; curl polls 1/s, concurrency buys nothing.
#include <cstring>
#include <functional>
#include <string>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class MgmtServer {
 public:
  MgmtServer(int port, std::function<std::string()> leader_ep,
             std::function<std::string()> health, std::function<std::string()> metrics)
      : port_(port), leader_ep_(std::move(leader_ep)), health_(std::move(health)), metrics_(std::move(metrics)) {}

  bool Run(std::string* err) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return Fail("socket", err);
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(srv, 8) != 0) {
      ::close(srv);
      return Fail("bind/listen", err);
    }
    for (;;) {
      int c = ::accept(srv, nullptr, nullptr);
      if (c < 0) {
        ::close(srv);
        return Fail("accept", err);
      }
      Serve(c);
      ::close(c);
    }
  }

 private:
  void Serve(int fd) {
    char req[4096];
    const ssize_t n = ::recv(fd, req, sizeof req - 1, 0);
    if (n <= 0) return;
    req[n] = '\0';
    const std::string r(req, static_cast<size_t>(n));
    const size_t eol = r.find("\r\n");
    const std::string line = r.substr(0, eol);
    int code = 200;
    std::string body;
    if (line.rfind("GET /leader", 0) == 0) {
      body = leader_ep_();
      if (body.empty() || body == "none") {
        code = 503;
        body = "none\n";
      } else {
        body += "\n";
      }
    } else if (line.rfind("GET /health", 0) == 0) {
      body = health_() + "\n";
    } else if (line.rfind("GET /metrics", 0) == 0) {
      body = metrics_();
    } else {
      code = 404;
      body = "not found\n";
    }
    const std::string status = (code == 200) ? "200 OK" : (code == 503 ? "503 Unavailable" : "404 Not Found");
    const std::string head = "HTTP/1.0 " + status + "\r\nContent-Type: text/plain\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    const std::string out = head + body;
    size_t done = 0;
    while (done < out.size()) {
      const ssize_t w = ::send(fd, out.data() + done, out.size() - done, MSG_NOSIGNAL);
      if (w <= 0) break;
      done += static_cast<size_t>(w);
    }
  }

  static bool Fail(const std::string& what, std::string* err) {
    if (err) *err = what + ": " + std::string(::strerror(errno));
    return false;
  }

  int port_;
  std::function<std::string()> leader_ep_;
  std::function<std::string()> health_;
  std::function<std::string()> metrics_;
};
