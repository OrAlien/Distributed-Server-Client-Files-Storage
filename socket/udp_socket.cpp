#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <cstring>

#include "udp_socket.h"


namespace cp = communication_protocol;

udp_socket::udp_socket()
    : inet_socket()
    {}

void udp_socket::create_socket() {
    if ((sock = ::socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        throw socket_failure("socket creation");
    closed = false;
    struct sockaddr_in local_addr {};
    socklen_t addrlen = sizeof(local_addr);
    memset(&local_addr, 0, sizeof(local_addr));
    if (::getsockname(sock, (struct sockaddr*) &local_addr, &addrlen) < 0)
        throw socket_failure("getsockname");
    port = local_addr.sin_port;
}

void udp_socket::create_multicast_socket() {
    int optval;
    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
        throw socket_failure("socket");
    optval = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *) &optval, sizeof(optval)) < 0)
        throw socket_failure("setsockopt broadcast");
    optval = TTL;
    if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (void *) &optval, sizeof(optval)) < 0)
        throw socket_failure("setsockopt multicast ttl");
}

void udp_socket::join_multicast_group(const std::string& multicast_address) {
    struct ip_mreq ip_mreq{};
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(multicast_address.c_str(), &ip_mreq.imr_multiaddr) == 0)
        throw socket_failure("inet_aton");
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&ip_mreq, sizeof ip_mreq) < 0)
        throw socket_failure("setsockopt IP_ADD_MEMBERSHIP");
}

void udp_socket::leave_multicast_group(const std::string& multicast_address) {
    struct ip_mreq ip_mreq{};
    ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (inet_aton(multicast_address.c_str(), &ip_mreq.imr_multiaddr) == 0)
        throw socket_failure("inet_aton");
    if (setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (void*)&ip_mreq, sizeof ip_mreq) < 0)
        throw socket_failure("setsockopt IP_DROP_MEMBERSHIP");
}

void udp_socket::bind(in_port_t port) {
    struct sockaddr_in local_address{};
    local_address.sin_family = AF_INET;
    local_address.sin_addr.s_addr = htonl(INADDR_ANY);
    local_address.sin_port = port;
    if (::bind(sock, (struct sockaddr*) &local_address, sizeof(local_address)) < 0)
        throw socket_failure("bind");
}

void udp_socket::send(const SimpleMessage& message, const std::string& destination_ip, in_port_t destination_port, uint16_t data_length) {
    uint16_t message_length = cp::simple_message_no_data_size + data_length;
    struct sockaddr_in destination_address{};
    destination_address.sin_family = AF_INET;
    destination_address.sin_port = destination_port;
    if (inet_aton(destination_ip.c_str(), &destination_address.sin_addr) == 0)
        throw socket_failure("inet_aton");
    if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
        throw socket_failure("sendto");
}

void udp_socket::send(const ComplexMessage& message, const std::string& destination_ip, in_port_t destination_port, uint16_t data_length) {
    uint16_t message_length = cp::complex_message_no_data_size + data_length;
    struct sockaddr_in destination_address{};
    destination_address.sin_family = AF_INET;
    destination_address.sin_port = destination_port;
    if (inet_aton(destination_ip.c_str(), &destination_address.sin_addr) == 0)
        throw socket_failure("inet_aton");
    if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
        throw socket_failure("sendto");
}


void udp_socket::send(const SimpleMessage& message, const struct sockaddr_in& destination_address, uint16_t data_length) {
    uint16_t message_length = cp::simple_message_no_data_size + data_length;
    if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
        throw socket_failure("sendto");
}

void udp_socket::send(const ComplexMessage& message, const struct sockaddr_in& destination_address, uint16_t data_length) {
    uint16_t message_length = cp::complex_message_no_data_size + data_length;
    if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
        throw socket_failure("sendto");
}

std::tuple<ComplexMessage, ssize_t, struct sockaddr_in> udp_socket::recvfrom_complex() {
    ComplexMessage message;
    ssize_t recv_len;
    struct sockaddr_in source_address{};
    socklen_t addrlen = sizeof(struct sockaddr_in);

    recv_len = ::recvfrom(sock, &message, sizeof(message), 0, (struct sockaddr*) &source_address, &addrlen);
    if (recv_len < 0)
        throw socket_failure("read");

    return {message, recv_len, source_address};
}

std::tuple<SimpleMessage, ssize_t, struct sockaddr_in> udp_socket::recvfrom_simple() {
    SimpleMessage message;
    ssize_t recv_len;
    struct sockaddr_in source_address{};
    socklen_t addrlen = sizeof(struct sockaddr_in);

    recv_len = ::recvfrom(sock, &message, sizeof(message), 0, (struct sockaddr*) &source_address, &addrlen);
    if (recv_len < 0)
        throw socket_failure("read");

    return {message, recv_len, source_address};
}