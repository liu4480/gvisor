// Copyright 2020 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at //
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <unordered_map>

#include "arpa/inet.h"
#include "include/grpcpp/security/server_credentials.h"
#include "include/grpcpp/server_builder.h"
#include "test/packetimpact/proto/posix_server.grpc.pb.h"
#include "test/packetimpact/proto/posix_server.pb.h"

// Converts a sockaddr_storage to a Sockaddr message.
::grpc::Status sockaddr_to_proto(const sockaddr_storage &addr,
                                 socklen_t addrlen,
                                 posix_server::Sockaddr *sockaddr_proto) {
  switch (addr.ss_family) {
    case AF_INET: {
      auto addr_in = reinterpret_cast<const sockaddr_in *>(&addr);
      auto response_in = sockaddr_proto->mutable_in();
      response_in->set_family(addr_in->sin_family);
      response_in->set_port(ntohs(addr_in->sin_port));
      response_in->mutable_addr()->assign(
          reinterpret_cast<const char *>(&addr_in->sin_addr.s_addr), 4);
      return ::grpc::Status::OK;
    }
    case AF_INET6: {
      auto addr_in6 = reinterpret_cast<const sockaddr_in6 *>(&addr);
      auto response_in6 = sockaddr_proto->mutable_in6();
      response_in6->set_family(addr_in6->sin6_family);
      response_in6->set_port(ntohs(addr_in6->sin6_port));
      response_in6->set_flowinfo(ntohl(addr_in6->sin6_flowinfo));
      response_in6->mutable_addr()->assign(
          reinterpret_cast<const char *>(&addr_in6->sin6_addr.s6_addr), 16);
      response_in6->set_scope_id(ntohl(addr_in6->sin6_scope_id));
      return ::grpc::Status::OK;
    }
  }
  return ::grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Unknown Sockaddr");
}

class PosixImpl final : public posix_server::Posix::Service {
  ::grpc::Status Accept(grpc_impl::ServerContext *context,
                        const ::posix_server::AcceptRequest *request,
                        ::posix_server::AcceptResponse *response) override {
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    response->set_fd(accept(request->sockfd(),
                            reinterpret_cast<sockaddr *>(&addr), &addrlen));
    response->set_errno_(errno);
    return sockaddr_to_proto(addr, addrlen, response->mutable_addr());
  }

  ::grpc::Status Bind(grpc_impl::ServerContext *context,
                      const ::posix_server::BindRequest *request,
                      ::posix_server::BindResponse *response) override {
    if (!request->has_addr()) {
      return ::grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "Missing address");
    }
    sockaddr_storage addr;

    switch (request->addr().sockaddr_case()) {
      case posix_server::Sockaddr::SockaddrCase::kIn: {
        auto request_in = request->addr().in();
        if (request_in.addr().size() != 4) {
          return ::grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "IPv4 address must be 4 bytes");
        }
        auto addr_in = reinterpret_cast<sockaddr_in *>(&addr);
        addr_in->sin_family = request_in.family();
        addr_in->sin_port = htons(request_in.port());
        request_in.addr().copy(
            reinterpret_cast<char *>(&addr_in->sin_addr.s_addr), 4);
        break;
      }
      case posix_server::Sockaddr::SockaddrCase::kIn6: {
        auto request_in6 = request->addr().in6();
        if (request_in6.addr().size() != 16) {
          return ::grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "IPv6 address must be 16 bytes");
        }
        auto addr_in6 = reinterpret_cast<sockaddr_in6 *>(&addr);
        addr_in6->sin6_family = request_in6.family();
        addr_in6->sin6_port = htons(request_in6.port());
        addr_in6->sin6_flowinfo = htonl(request_in6.flowinfo());
        request_in6.addr().copy(
            reinterpret_cast<char *>(&addr_in6->sin6_addr.s6_addr), 16);
        addr_in6->sin6_scope_id = htonl(request_in6.scope_id());
        break;
      }
      case posix_server::Sockaddr::SockaddrCase::SOCKADDR_NOT_SET:
      default:
        return ::grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "Unknown Sockaddr");
    }
    response->set_ret(bind(request->sockfd(),
                           reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status Close(grpc_impl::ServerContext *context,
                       const ::posix_server::CloseRequest *request,
                       ::posix_server::CloseResponse *response) override {
    response->set_ret(close(request->fd()));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status GetSockName(
      grpc_impl::ServerContext *context,
      const ::posix_server::GetSockNameRequest *request,
      ::posix_server::GetSockNameResponse *response) override {
    sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    response->set_ret(getsockname(
        request->sockfd(), reinterpret_cast<sockaddr *>(&addr), &addrlen));
    response->set_errno_(errno);
    return sockaddr_to_proto(addr, addrlen, response->mutable_addr());
  }

  ::grpc::Status Listen(grpc_impl::ServerContext *context,
                        const ::posix_server::ListenRequest *request,
                        ::posix_server::ListenResponse *response) override {
    response->set_ret(listen(request->sockfd(), request->backlog()));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status SetSockOpt(
      grpc_impl::ServerContext *context,
      const ::posix_server::SetSockOptRequest *request,
      ::posix_server::SetSockOptResponse *response) override {
    response->set_ret(setsockopt(request->sockfd(), request->level(),
                                 request->optname(), request->optval().c_str(),
                                 request->optval().size()));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status SetSockOptTimeval(
      ::grpc::ServerContext *context,
      const ::posix_server::SetSockOptTimevalRequest *request,
      ::posix_server::SetSockOptTimevalResponse *response) override {
    timeval tv = {.tv_sec = static_cast<__time_t>(request->timeval().seconds()),
                  .tv_usec = static_cast<__suseconds_t>(
                      request->timeval().microseconds())};
    response->set_ret(setsockopt(request->sockfd(), request->level(),
                                 request->optname(), &tv, sizeof(tv)));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status Socket(grpc_impl::ServerContext *context,
                        const ::posix_server::SocketRequest *request,
                        ::posix_server::SocketResponse *response) override {
    response->set_fd(
        socket(request->domain(), request->type(), request->protocol()));
    response->set_errno_(errno);
    return ::grpc::Status::OK;
  }

  ::grpc::Status Recv(::grpc::ServerContext *context,
                      const ::posix_server::RecvRequest *request,
                      ::posix_server::RecvResponse *response) override {
    std::vector<char> buf(request->len());
    response->set_ret(
        recv(request->sockfd(), buf.data(), buf.size(), request->flags()));
    response->set_errno_(errno);
    response->set_buf(buf.data(), response->ret());
    return ::grpc::Status::OK;
  }
};

// Parse command line options. Returns a pointer to the first argument beyond
// the options.
void parse_command_line_options(int argc, char *argv[], std::string *ip,
                                int *port) {
  static struct option options[] = {{"ip", required_argument, NULL, 1},
                                    {"port", required_argument, NULL, 2},
                                    {0, 0, 0, 0}};

  // Parse the arguments.
  int c;
  while ((c = getopt_long(argc, argv, "", options, NULL)) > 0) {
    if (c == 1) {
      *ip = optarg;
    } else if (c == 2) {
      *port = std::stoi(std::string(optarg));
    }
  }
}

void run_server(const std::string &ip, int port) {
  PosixImpl posix_service;
  grpc::ServerBuilder builder;
  std::string server_address = ip + ":" + std::to_string(port);
  // Set the authentication mechanism.
  std::shared_ptr<grpc::ServerCredentials> creds =
      grpc::InsecureServerCredentials();
  builder.AddListeningPort(server_address, creds);
  builder.RegisterService(&posix_service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  std::cerr << "Server listening on " << server_address << std::endl;
  server->Wait();
  std::cerr << "posix_server is finished." << std::endl;
}

int main(int argc, char *argv[]) {
  std::cerr << "posix_server is starting." << std::endl;
  std::string ip;
  int port;
  parse_command_line_options(argc, argv, &ip, &port);

  std::cerr << "Got IP " << ip << " and port " << port << "." << std::endl;
  run_server(ip, port);
}
