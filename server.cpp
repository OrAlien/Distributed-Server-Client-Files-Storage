#include <iostream>
#include <utility>
#include <vector>
#include <random>
#include <tuple>
#include <thread>
#include <regex>
#include <atomic>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>

#include <unistd.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "helper.h"
#include "err.h"

#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)
#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif	/* __APPLE__ */

constexpr uint16_t default_timeout = 5;
constexpr uint16_t max_timeout = 300;
constexpr uint64_t default_max_disc_space = 52428800;

using std::string;
using std::cout;
using std::endl;
using std::cerr;
using std::to_string;
using std::vector;
using std::string_view;
using std::tuple;
using std::ostream;
using boost::format;
using std::exception;
using std::thread;

namespace fs = boost::filesystem;
namespace po = boost::program_options;
namespace cp = communication_protocol;
namespace logging = boost::log;

struct ServerParameters {
    string mcast_addr;
    in_port_t cmd_port;
    uint64_t max_space;
    string shared_folder;
    uint16_t timeout;

    ServerParameters() = default;
};

ServerParameters parse_program_arguments(int argc, const char *argv[]) {
    ServerParameters server_parameters;

    po::options_description description {"Program options"};
    description.add_options()
            ("help,h", "Help screen")
            ("MCAST_ADDR,g", po::value<string>(&server_parameters.mcast_addr)->required(), "Multicast address")
            ("CMD_PORT,p", po::value<in_port_t>(&server_parameters.cmd_port)->required(), "UDP port used for sending and receiving messages")
            ("MAX_SPACE,b", po::value<uint64_t>(&server_parameters.max_space)->default_value(default_max_disc_space),
                    ("Maximum disc space in this server node\n"
                     "  Default value: " + to_string(default_max_disc_space)).c_str())
            ("SHRD_FLDR,f", po::value<string>(&server_parameters.shared_folder)->required(), "Path to the directory in which files are stored")
            ("TIMEOUT,t", po::value<uint16_t>(&server_parameters.timeout)->default_value(default_timeout)->notifier([](uint16_t timeout) {
                 if (timeout > max_timeout) {
                     cerr << "TIMEOUT out of range\n";
                     exit(1);
                 }
             }),
             (("Maximum waiting time for connection from client\n"
               "  Min value: 0\n"
               "  Max value: " + to_string(max_timeout)) + "\n" +
               "  Default value: " + to_string(default_timeout)).c_str());

    po::variables_map var_map;
    try {
        po::store(po::parse_command_line(argc, argv, description), var_map);
        if (var_map.count("help")) {
            cout << description << endl;
            exit(0);
        }

        po::notify(var_map);
    } catch (po::required_option &e) {
        cerr << e.what() << endl;
        exit(1);
    }

    return server_parameters;
}

class Server {
    const string multicast_address;
    in_port_t cmd_port;
    std::mutex used_space_mutex;
    std::atomic<uint64_t> used_space = 0;
    const uint64_t max_available_space;
    const string shared_folder;
    const uint16_t timeout;

    vector<fs::path> files_in_storage;

    /*** Receiving ***/
    int recv_socket;

    // TODO: Ask whether directories should be listed
    void generate_files_in_storage() {
        if (!fs::exists(this->shared_folder))
            throw std::invalid_argument("Shared folder doesn't exists");
        if (!fs::is_directory(this->shared_folder))
            throw std::invalid_argument("Shared folder is not a directory");
        for (const auto& path: fs::directory_iterator(this->shared_folder)) {
          if (fs::is_regular_file(path)) {
                files_in_storage.emplace_back(path.path());
                this->used_space += fs::file_size(path.path());
           }
        }
    }

    void init_recv_socket() {
        if ((recv_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            syserr("socket");
        join_multicast_group();

        int reuse = 1; // TODO wywalić domyślnie?
        if (setsockopt(recv_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
            syserr("setsockopt(SO_REUSEADDR) failed");

        struct sockaddr_in local_address{};
        local_address.sin_family = AF_INET;
        local_address.sin_addr.s_addr = htonl(INADDR_ANY);
        local_address.sin_port = htons(cmd_port);
        if (bind(recv_socket, (struct sockaddr*) &local_address, sizeof(local_address)) < 0)
            syserr("bind");
    }

    void join_multicast_group() {
        struct ip_mreq ip_mreq;
        ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (inet_aton(this->multicast_address.c_str(), &ip_mreq.imr_multiaddr) == 0)
            syserr("inet_aton");
        if (setsockopt(recv_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, (void*)&ip_mreq, sizeof ip_mreq) < 0)
            syserr("setsockopt");
    }

    void leave_multicast_group() {
        struct ip_mreq ip_mreq;
        ip_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (inet_aton(this->multicast_address.c_str(), &ip_mreq.imr_multiaddr) == 0)
            syserr("inet_aton");
        if (setsockopt(recv_socket, IPPROTO_IP, IP_DROP_MEMBERSHIP, (void*)&ip_mreq, sizeof ip_mreq) < 0)
            syserr("setsockopt");
    }

    static tuple<int, in_port_t> create_tcp_socket() {
        int tcp_socket;
        struct sockaddr_in local_addr {};
        socklen_t addrlen = sizeof(local_addr);
        memset(&local_addr, 0, sizeof(local_addr)); // sin_port set to 0, therefore it will be set random free port
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if ((tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
            syserr("socket");
        if (bind(tcp_socket, (struct sockaddr*) &local_addr, sizeof(local_addr)) < 0)
            syserr("bind");
        if (listen(tcp_socket, TCP_QUEUE_LENGTH) < 0)
            syserr("listen");
        if (getsockname(tcp_socket, (struct sockaddr*) &local_addr, &addrlen) < 0)
            syserr("getsockname");
        in_port_t tcp_port = be16toh(local_addr.sin_port);

        BOOST_LOG_TRIVIAL(info) << "TCP socket created, port chosen = " << tcp_port;
        return {tcp_socket, tcp_port};
    }

    static void send_message_udp(const SimpleMessage &message, const struct sockaddr_in& destination_address, uint16_t data_length = 0) {
        uint16_t message_length = const_variables::simple_message_no_data_size + data_length;
        int sock;
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            syserr("socket");
        if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
            syserr("sendto");
        if (close(sock) < 0)
            syserr("sock");
        BOOST_LOG_TRIVIAL(info) << "UDP command sent";
    }

    static void send_message_udp(const ComplexMessage &message, const struct sockaddr_in& destination_address, uint16_t data_length = 0) {
        uint16_t message_length = const_variables::complex_message_no_data_size + data_length;
        int sock;
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            syserr("socket");
        if (sendto(sock, &message, message_length, 0, (struct sockaddr*) &destination_address, sizeof(destination_address)) != message_length)
            syserr("sendto");
        if (close(sock) < 0)
            syserr("sock");
        BOOST_LOG_TRIVIAL(info) << "UDP command sent";
    }

    uint64_t get_available_space() {
        uint64_t tmp = this->used_space; // threads...
        return tmp > this->max_available_space ? 0 : this->max_available_space - tmp;
    }

//    bool is_enough_space(uint64_t file_size) {
//        return file_size <= this->get_available_space();
//    }

    /**
     * Attempts to reserve given amount of space
     * @param size size of space to be reserved
     * @return True if space was reserved successfuly, false otherwise.
     */
    bool check_and_reserve_space(uint64_t size) {
        std::lock_guard<std::mutex> lock(used_space_mutex);
        if (size > max_available_space || used_space > max_available_space - size)
            return false;
        used_space += size;
        return true;
    }

    /**
     * Use it correctly!
     */
    void free_space(uint64_t size) {
        std::lock_guard<std::mutex> lock(used_space_mutex);
        used_space -= size;
    }

    vector<fs::path>::iterator find_file(const string& filename) {
        for (auto it = this->files_in_storage.begin(); it != this->files_in_storage.end(); it++)
            if (it->filename() == filename)
                return it;

        return this->files_in_storage.end();
    }

public:
    Server(string mcast_addr, in_port_t cmd_port, uint64_t max_available_space, string shared_folder_path, uint16_t timeout)
        : multicast_address(std::move(mcast_addr)),
        cmd_port(cmd_port),
        max_available_space(max_available_space),
        shared_folder(move(shared_folder_path)),
        timeout(timeout)
        {}

    Server(const struct ServerParameters& server_parameters)
        : Server(server_parameters.mcast_addr, server_parameters.cmd_port, server_parameters.max_space,
                server_parameters.shared_folder, server_parameters.timeout)
        {}

    void init() {
        BOOST_LOG_TRIVIAL(trace) << "Starting server initialization...";
        generate_files_in_storage();
        init_recv_socket();
        BOOST_LOG_TRIVIAL(trace) << "Server initialization ended";
    }


    /*************************************************** DISCOVER *****************************************************/

    // TODO multicast ma być w jakim formacie przesłany??
    void handle_discover_request(const struct sockaddr_in& destination_address, uint64_t message_seq) {
        struct ComplexMessage message {htobe64(message_seq), cp::discover_response,
                this->multicast_address.c_str(), htobe64(this->get_available_space())};
        BOOST_LOG_TRIVIAL(info) << format("Sending to: %1%:%2%")
                                        %inet_ntoa(destination_address.sin_addr) %be16toh(destination_address.sin_port);
        send_message_udp(message, destination_address,
                const_variables::complex_message_no_data_size + this->multicast_address.length());
        BOOST_LOG_TRIVIAL(info) << "Message sent: " << message;
    }

    /************************************************** FILES LIST ****************************************************/

    static void fill_message_with_filename(struct SimpleMessage& msg_send, uint64_t* start_index, const string& filename) {
        if (*start_index > 0)
            msg_send.data[(*start_index)++] = '\n';
        strcpy(msg_send.data + *start_index, filename.c_str());
        *start_index += filename.length();
    }

    void handle_files_list_request(const struct sockaddr_in& destination_address, uint64_t message_seq, const char *pattern) {
        BOOST_LOG_TRIVIAL(trace) << "Files list request for pattern: " << pattern;
        int sock;
        if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
            syserr("socket");

        uint64_t curr_data_len = 0;
        string filename;
        struct SimpleMessage message {htobe64(message_seq), cp::files_list_response};
        for (const fs::path& file : this->files_in_storage) {
            filename = file.filename().string();
            if (is_substring(pattern, filename)) {
                if (const_variables::max_simple_data_size > curr_data_len + filename.length()) {
                    fill_message_with_filename(message, &curr_data_len, file.filename().generic_string());
                } else {
                    send_message_udp(message, destination_address, curr_data_len);
                    curr_data_len = 0;
                    memset(&message.data, '\0', sizeof(message.data));
                }
            }
        }
        if (curr_data_len > 0)
            send_message_udp(message, destination_address, curr_data_len);

        if (close(sock) < 0)
            syserr("close");
        BOOST_LOG_TRIVIAL(trace) << format("Handled files list request, pattern = ") %pattern;
    }


    /************************************************* DOWNLOAD FILE **************************************************/

    /**
     * 1. Select free port
     * 2. Send chosen port to the client
     * 3. Wait on this port for TCP connection with the client
     */
    void handle_file_request(struct sockaddr_in &destination_address, uint64_t message_seq, const string& filename) {
        BOOST_LOG_TRIVIAL(info)  << "Starting file request, filename = " << filename;
        if (find_file(filename) != this->files_in_storage.end()) {
            auto [tcp_socket, tcp_port] = create_tcp_socket();

            ComplexMessage message{htobe64(message_seq), cp::file_get_response, filename.c_str(), htobe64(tcp_port)};
            send_message_udp(message, destination_address, filename.length());
            BOOST_LOG_TRIVIAL(info) << format("TCP port info sent, port chosen = %1%") %tcp_port;
            send_file_via_tcp(tcp_socket, tcp_port, filename);

            if (close(tcp_socket) < 0)
                syserr("close");
        } else {
            cout << "Incorrect file request received" << endl; // TODO co wypisywać?
        }
        BOOST_LOG_TRIVIAL(info)  << "Handled file request, filename = " << filename;
    }

    void send_file_via_tcp(int tcp_socket, uint16_t tcp_port, const string& filename) {
        BOOST_LOG_TRIVIAL(trace) << "Starting sending file via tcp...";

        fd_set set;
        struct timeval timeval{};
        FD_ZERO(&set);
        FD_SET(tcp_socket, &set);

        timeval.tv_sec = this->timeout;
        int rv = select(tcp_socket + 1, &set, nullptr, nullptr, &timeval); // Wait for client up to timeout sec.
        if (rv == -1) {
            BOOST_LOG_TRIVIAL(error) << format("select, port = ") %tcp_port;
        } else if (rv == 0) {
            BOOST_LOG_TRIVIAL(info) << format("Timeout on port:%1% no message received") %tcp_port;
        } else { // Client is waiting
            struct sockaddr_in source_address{};
            memset(&source_address, 0, sizeof(source_address));
            socklen_t len = sizeof(source_address);

            int sock = accept(tcp_socket, (struct sockaddr *) &source_address, &len);

            fs::path file_path{this->shared_folder + filename};
            std::ifstream file_stream{file_path.c_str(), std::ios::binary};

            if (file_stream.is_open()) {
                char buffer[MAX_BUFFER_SIZE];
                while (file_stream) {
                    file_stream.read(buffer, MAX_BUFFER_SIZE);
                    ssize_t length = file_stream.gcount();
                    if (write(sock, buffer, length) != length)
                        syserr("partial write");
                }
                file_stream.close(); // TODO can throw
            } else {
                cerr << "File opening error" << endl; // TODO
            }

            if (close(sock) < 0)
                syserr("close");
            BOOST_LOG_TRIVIAL(info) << format("Sending file via tcp finished,"
                                              "filename = %1%, port = %2%") %filename %tcp_port;
        }
    }


    /**************************************************** UPLOAD ******************************************************/

    /* TODO file_size = 0?
     *
     *
     * */

    void handle_upload_request(struct sockaddr_in &destination_address, uint64_t message_seq,
            const string& filename, uint64_t file_size) {
        BOOST_LOG_TRIVIAL(info) << format("File upload request, filename = %1%, filesize = %2%") %filename %file_size;
        bool can_upload;

        if (!(can_upload = is_valid_filename(filename))) {
            BOOST_LOG_TRIVIAL(info) << format("Rejecting file upload request, incorrect filename,"
                                              "received_filename = %1%") %filename;
        } else if (!(can_upload = check_and_reserve_space(file_size))) {
            BOOST_LOG_TRIVIAL(info) << format("Rejecting file upload request, not enough space,"
                                              "requested_space = %1%") %file_size;
        }

        if (can_upload) { // TODO some failure, free space... + free filename here somewhere reserve this filename?
            BOOST_LOG_TRIVIAL(info) << "Accepting file upload request";
            auto[tcp_socket, tcp_port] = create_tcp_socket();

            // Send info about tcp port
            ComplexMessage message{htobe64(message_seq), cp::file_add_acceptance, filename.c_str(), htobe64(tcp_port)};
            send_message_udp(message, destination_address);
            upload_file_via_tcp(tcp_socket, tcp_port, filename);
        } else {
            SimpleMessage message{htobe64(message_seq), cp::file_add_refusal};
            send_message_udp(message, destination_address);
        }
    }

    void upload_file_via_tcp(int tcp_socket, uint16_t port, const string& filename) {
        BOOST_LOG_TRIVIAL(trace) << format("Uploading file via tcp, port:%1%") %port;

        fd_set set;
        struct timeval timeval{};
        FD_ZERO(&set);
        FD_SET(tcp_socket, &set);

        timeval.tv_sec = this->timeout;
        int rv = select(tcp_socket + 1, &set, nullptr, nullptr, &timeval);
        if (rv == -1) {
            BOOST_LOG_TRIVIAL(error) << "select";
        } else if (rv == 0) {
            BOOST_LOG_TRIVIAL(info) << format("Timeout occured on port:%1% no message received") %port;
        } else {
            struct sockaddr_in source_address{};
            memset(&source_address, 0, sizeof(source_address));
            socklen_t len = sizeof(source_address);
            int sock = accept(tcp_socket, (struct sockaddr *) &source_address, &len);

            fs::path file_path(this->shared_folder + filename);
            fs::ofstream ofs(file_path, std::ofstream::binary);

            char buffer[MAX_BUFFER_SIZE];
            while ((len = read(sock, buffer, sizeof(buffer))) > 0) {
                ofs.write(buffer, len);
            }
            ofs.close();

            if (close(sock) < 0)
                syserr("close");

            BOOST_LOG_TRIVIAL(trace) << format("Ending uploading file via tcp, port:%1%") %port;
        }
    }

    /**************************************************** REMOVE ******************************************************/

    // TODO What is someone is downloading this file in this moment?
    // TODO validate filename or just skip it? ~ info ? incorrect filanem or sth? Or just don;t do anything if file not in filelist
    void handle_remove_request(const string& filename) {
        auto it = find_file(filename);
        string file = this->shared_folder + filename;
        if (it != this->files_in_storage.end()) {
            BOOST_LOG_TRIVIAL(info) << "Deleting file, filename = " << filename;
            this->files_in_storage.erase(it);
            this->used_space -= fs::file_size(file);
            fs::remove(file);
            BOOST_LOG_TRIVIAL(info) << "File successfully deleted, filename = " << filename;
        } else {
            BOOST_LOG_TRIVIAL(info) << "Skipping deleting file, no such file in storage";
        }
    }

    /************************************************** VALIDATION ****************************************************/

    static bool is_valid_filename(const string& filename) { // TODO max filename length?
        std::regex filename_regex("[^-_.A-Za-z0-9]");
        return std::regex_match(filename, filename_regex);
    }

    /**
     * Basic message validation:
     * Assures that message's command is correct and data if required was attached. Doesn't validate data.
     * 1. Checks whether received command is valid.
     * 2. Checks whether message length is in acceptable range.
     * @param message - message to be validated.
     * @param message_length - length of given message (bytes read from socket).
     */
    static void message_validation(const ComplexMessage& message, ssize_t message_length) {
        if (is_valid_string(message.command, const_variables::max_command_length)) {
            if (message.command == cp::file_add_request) {
                if (message_length < const_variables::complex_message_no_data_size)
                    throw invalid_message("Message too small");
            } else {
                if (message_length < const_variables::simple_message_no_data_size)
                    throw invalid_message("Message too small");

                if (message.command == cp::discover_request) {
                    if (message_length > const_variables::simple_message_no_data_size)
                        throw invalid_message("Message too big");
                } else if (message.command == cp::files_list_request) {

                } else if (message.command == cp::file_add_request) {
                    if (message_length == const_variables::simple_message_no_data_size)
                        throw invalid_message("Upload request, filename not specified.");
                } else if (message.command == cp::file_get_request) {
                    if (message_length == const_variables::simple_message_no_data_size)
                        throw invalid_message("Fetch request, filename not specified.");
                } else if (message.command == cp::file_remove_request) {
                    if (message_length == const_variables::simple_message_no_data_size)
                        throw invalid_message("Remove request, filename not specified.");
                } else {
                    throw invalid_message("Unknown command");
                }
            }
        } else {
            throw invalid_message("Invalid command");
        }
    }

    /****************************************************** RUN *******************************************************/

    /**
     *
     * @return (message received, length of received message in bytes, source address)
     */
    tuple<ComplexMessage, ssize_t, struct sockaddr_in> receive_next_message() {
        ComplexMessage message;
        ssize_t recv_len;
        struct sockaddr_in source_address{};
        socklen_t addrlen = sizeof(struct sockaddr_in);

        recv_len = recvfrom(recv_socket, &message, sizeof(message), 0, (struct sockaddr*) &source_address, &addrlen);
        if (recv_len < 0)
            syserr("read");

        return {message, recv_len, source_address};
    }

    template<typename... A>
    void handler(A&&... args) {
        thread handler {std::forward<A>(args)...};
        handler.detach();
    }

    void run() {
        string source_ip;
        string source_port;

        while (true) {
            BOOST_LOG_TRIVIAL(info) << "Waiting for client...";
            auto [message_complex, message_length, source_address] = receive_next_message();
            source_ip = inet_ntoa(source_address.sin_addr);
            source_port = be16toh(source_address.sin_port);

            try {
                message_validation(message_complex, message_length);
            } catch (const invalid_message& e) {
                cerr << format("[PCKG ERROR] Skipping invalid package from %1%:%2%. %3%") %source_ip %source_port %e.what() << endl;
                BOOST_LOG_TRIVIAL(info) << format("[PCKG ERROR] Skipping invalid package from %1%:%2%. %3%") %source_ip %source_port %e.what();
                continue;
            }

            /// For each message received main thread creates handler and passes to it full reponsibility for handling request.
            /// That is handling inccorect data, informing client about this etc.
            /// The only thing main thread guarantees is that message passed is correct size-wise and command-wise
            if (message_complex.command == cp::file_add_request) {
                BOOST_LOG_TRIVIAL(info) << "Message received: " << message_complex;
                handler(&Server::handle_upload_request, this, std::ref(source_address),
                        be64toh(message_complex.message_seq), message_complex.data, be64toh(message_complex.param));
//                std::thread handler {&Server::handle_upload_request, this, std::ref(source_address),
//                        be64toh(message_complex.message_seq), message_complex.data, be64toh(message_complex.param)};
//                handler.detach();
//                handle_upload_request(source_address, be64toh(message_complex.message_seq), message_complex.data,
//                                      be64toh(message_complex.param));
            } else {
                auto *message_simple = (SimpleMessage*) &message_complex;
                BOOST_LOG_TRIVIAL(info) << "Message received: " << *message_simple;

                if (message_simple->command == cp::discover_request) {
                    handler(&Server::handle_discover_request, this,
                            std::ref(source_address), be64toh(message_simple->message_seq));
//                    thread handler {&Server::handle_discover_request, this,
//                                       std::ref(source_address), be64toh(message_simple->message_seq)};
//                    handler.detach();
//                    handle_discover_request(source_address, be64toh(message_simple->message_seq));
                } else if (message_simple->command == cp::files_list_request) {
                    handler(&Server::handle_files_list_request, this, std::ref(source_address),
                            be64toh(message_simple->message_seq), message_simple->data);
//                    thread handler {&Server::handle_files_list_request, this, std::ref(source_address),
//                            be64toh(message_simple->message_seq), message_simple->data};
//                    handler.detach();
//                    handle_files_list_request(source_address, be64toh(message_simple->message_seq), message_simple->data);
                } else if (message_simple->command == cp::file_get_request) {
                    handler(&Server::handle_file_request, this, std::ref(source_address),
                            be64toh(message_simple->message_seq), message_simple->data);
//                    thread handler {&Server::handle_file_request, this, std::ref(source_address),
//                            be64toh(message_simple->message_seq), message_simple->data};
//                    handler.detach();
//                    handle_file_request(source_address, be64toh(message_simple->message_seq), message_simple->data);
                } else if (message_simple->command == cp::file_remove_request) {
                    handler(&Server::handle_remove_request, this, message_simple->data);
//                    thread handler {&Server::handle_remove_request, this, message_simple->data};
//                    handler.detach();
//                    handle_remove_request(message_simple->data);
                }
            }
        }
    }

    friend ostream& operator << (ostream &out, const Server &server) {
        out << "\nSERVER INFO:";
        out << "\nMCAST_ADDR = " << server.multicast_address;
        out << "\nCMD_PORT = " << server.cmd_port;
        out << "\nMAX_SPACE = " << server.max_available_space;
        out << "\nFOLDER = " << server.shared_folder;
        out << "\nTIMEOUT = " << server.timeout;

        return out;
    }
};

void init() {
    logging::register_simple_formatter_factory<logging::trivial::severity_level, char>("Severity");
    logging::add_file_log
            (
                    logging::keywords::file_name = "logger_server.log",
                    logging::keywords::rotation_size = 10 * 1024 * 1024,
                    logging::keywords::time_based_rotation = logging::sinks::file::rotation_at_time_point(0, 0, 0),
                    logging::keywords::format = "[%TimeStamp%] [tid=%ThreadID%] [%Severity%]: %Message%",
                    logging::keywords::auto_flush = true
            );
    logging::add_common_attributes();

  //  logging::core::get()->set_logging_enabled(false);
}

//    boost::log::core::get()->set_filter
//            (
//                    logging::trivial::severity >= logging::trivial::info
//            );


int main(int argc, const char *argv[]) {
    init();
    struct ServerParameters server_parameters = parse_program_arguments(argc, argv);
    Server server {server_parameters};
    try {
        server.init();
        BOOST_LOG_TRIVIAL(trace) << "Starting server...";
        BOOST_LOG_TRIVIAL(trace) << server << endl;
        server.run();
    } catch (const exception& e) {
        cerr << e.what() << endl;
    }

    return 0;
}
