// NOTE: This starter code contains a primitive implementation using the default RabbitMQ protocol.
// You are recommended to look into how to make the communication more efficient,
// for example, modifying the type of exchange that publishes to one or more queues, or
// throttling how often a process consumes messages from a queue so other consumers are not starved for messages
// All the functions in this implementation are just suggestions and you can make reasonable changes as long as
// you continue to use the communication methods that the assignment requires between different processes

#include <bits/fs_fwd.h>
#include <ctime>
#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <stdio.h>
#include <cstdlib>
#include <unistd.h>
#include <algorithm>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include <glog/logging.h>
#include "sns.grpc.pb.h"
#include "sns.pb.h"
#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

#include <amqp.h>
#include <amqp_tcp_socket.h>
#include <jsoncpp/json/json.h>

#define log(severity, msg) \
    LOG(severity) << msg;  \
    google::FlushLogFiles(google::severity);

namespace fs = std::filesystem;

using csce438::AllUsers;
using csce438::Confirmation;
using csce438::CoordService;
using csce438::ID;
using csce438::ServerInfo;
using csce438::ServerList;
using csce438::SynchronizerListReply;
using csce438::SynchService;
using google::protobuf::Duration;
using google::protobuf::Timestamp;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
// tl = timeline, fl = follow list
using csce438::TLFL;

int synchID = 1;
int clusterID = 1;
bool isMaster = false;
int total_number_of_registered_synchronizers = 6; // update this by asking coordinator
std::string coordAddr;
std::string clusterSubdirectory;
std::vector<std::string> otherHosts;
std::mutex otherHostsMutex;
std::unordered_map<std::string, int> timelineLengths;

std::vector<std::string> get_lines_from_file(std::string);
std::vector<std::string> get_all_users_func(int);
std::vector<std::string> get_tl_or_fl(int, int, bool);
std::vector<std::string> getFollowersOfUser(int);
bool file_contains_user(std::string filename, std::string user);
bool postExistsInTimeline(const std::vector<std::string> &timeline, const Json::Value &postInfo);

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID);

std::unique_ptr<csce438::CoordService::Stub> coordinator_stub_;

class SynchronizerRabbitMQ
{
private:
    amqp_connection_state_t conn;
    amqp_channel_t channel;
    std::string hostname;
    int port;
    int synchID;

    void setupRabbitMQ()
    {
        conn = amqp_new_connection();
        amqp_socket_t *socket = amqp_tcp_socket_new(conn);
        amqp_socket_open(socket, hostname.c_str(), port);
        amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
        amqp_channel_open(conn, channel);
    }

    void declareQueue(const std::string &queueName)
    {
        amqp_queue_declare(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, 0, 0, amqp_empty_table);
    }

    void setupConsumers() {
        std::vector<std::string> queueSuffixes = {
            "_users_queue",
            "_clients_relations_queue",
            "_timeline_queue"
        };
    
        for (const auto& suffix : queueSuffixes) {
            std::string queueName = "synch" + std::to_string(synchID) + suffix;
            amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
                               amqp_empty_bytes, 0, 1, 0, amqp_empty_table);
        }
    }    

    void publishMessage(const std::string &queueName, const std::string &message)
    {
        amqp_basic_publish(conn, channel, amqp_empty_bytes, amqp_cstring_bytes(queueName.c_str()),
                           0, 0, NULL, amqp_cstring_bytes(message.c_str()));
    }

    // std::string consumeMessage(const std::string &queueName, int timeout_ms = 5000)
    // {
    //     amqp_basic_consume(conn, channel, amqp_cstring_bytes(queueName.c_str()),
    //                        amqp_empty_bytes, 0, 1, 0, amqp_empty_table);

    //     amqp_envelope_t envelope;
    //     amqp_maybe_release_buffers(conn);

    //     struct timeval timeout;
    //     timeout.tv_sec = timeout_ms / 1000;
    //     timeout.tv_usec = (timeout_ms % 1000) * 1000;

    //     amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &timeout, 0);

    //     if (res.reply_type != AMQP_RESPONSE_NORMAL)
    //     {
    //         return "";
    //     }

    //     std::string message(static_cast<char *>(envelope.message.body.bytes), envelope.message.body.len);
    //     amqp_destroy_envelope(&envelope);
    //     return message;
    // }

    /**************************************/
    /********** HELPER FUNCTIONS **********/
    /**************************************/

    void publishMessageToAllSynchronizers(const std::string& message, const std::string& queueSuffix) {
        // first, ensure list of synchronizers is up to date
        updateAllSynchronizers();
        std::lock_guard<std::mutex> lock(otherHostsMutex);
        for (int i = 0; i < total_number_of_registered_synchronizers; i++)
        {
            if (otherHosts.at(i) == std::to_string(synchID)) // don't send to self
            {
                continue;
            }
            std::string queueName = "synch" + otherHosts.at(i) + queueSuffix;
            publishMessage(queueName, message);
        }
    }

    /********** COORDINATOR COMMUNICATION **********/
    std::vector<std::string> getAllUsersOnCluster() const {
        std::vector<std::string> allUsers;
        ClientContext context;
        ID id;
        id.set_id(synchID);
        AllUsers clusterUsers;
        coordinator_stub_->GetUsersOnCluster(&context, id, &clusterUsers);
        for (int i = 0; i < clusterUsers.users_size(); i++)
        {
            allUsers.push_back(clusterUsers.users(i));
        }
        return allUsers;
    }

    void updateAllSynchronizers() const {
        grpc::ClientContext context;
        ServerList followerServers;
        ID id;
        id.set_id(synchID);
        // making a request to the coordinator to see count of follower synchronizers
        coordinator_stub_->GetAllFollowerServers(&context, id, &followerServers);
        // clear and update the count of how many follower sychronizer processes 
        // the coordinator has registered
        std::lock_guard<std::mutex> lock(otherHostsMutex);
        otherHosts.clear();
        total_number_of_registered_synchronizers = followerServers.serverid_size();
        for (int i = 0; i < total_number_of_registered_synchronizers; i++) {
            otherHosts.push_back(std::to_string(followerServers.serverid(i)));
        }
        // log(INFO, "Total number of registered synchronizers: " +
        //     std::to_string(total_number_of_registered_synchronizers));
    }

public:
    // SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname(host), port(p), channel(1), synchID(id)
    SynchronizerRabbitMQ(const std::string &host, int p, int id) : hostname("rabbitmq"), port(p), channel(1), synchID(id)
    {
        setupRabbitMQ();
        declareQueue("synch" + std::to_string(synchID) + "_users_queue");
        declareQueue("synch" + std::to_string(synchID) + "_clients_relations_queue");
        declareQueue("synch" + std::to_string(synchID) + "_timeline_queue");
        // TODO: add or modify what kind of queues exist in your clusters based on your needs
        setupConsumers();
    }

    std::optional<std::pair<std::string, std::string>> consumeAnyMessageFromQueues(int timeout_ms = 5000) {

        amqp_envelope_t envelope;
        amqp_maybe_release_buffers(conn);

        struct timeval timeout;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        amqp_rpc_reply_t res = amqp_consume_message(conn, &envelope, &timeout, 0);

        if (res.reply_type != AMQP_RESPONSE_NORMAL)
        {
            return std::nullopt;
        }

        std::string queueName(static_cast<char *>(envelope.routing_key.bytes), envelope.routing_key.len);
        std::string message(static_cast<char *>(envelope.message.body.bytes), envelope.message.body.len);

        amqp_destroy_envelope(&envelope);

        return std::make_pair(queueName, message);
    }

    void publishUserList()
    {
        std::vector<std::string> users = get_all_users_func(synchID);
        std::sort(users.begin(), users.end());
        Json::Value userList;
        for (const auto &user : users)
        {
            userList["users"].append(user);
        }
        Json::FastWriter writer;
        std::string message = writer.write(userList);

        // post to the queue of all other synchronizers
        publishMessageToAllSynchronizers(message, "_users_queue");
    }

    void consumeUserLists(const std::string &message)
    {
        // request to get any new clients logged into the cluster and
        // update the all_users file with any new users
        std::vector<std::string> allUsers = getAllUsersOnCluster();

        std::string queueName = "synch" + std::to_string(synchID) + "_users_queue";

        log(INFO, "[consumeUserLists]: message received from queue " + queueName + ": " + message);
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(message, root))
        {
            for (const auto &user : root["users"])
            {
                // log(INFO, "User found: " + user.asString());
                allUsers.push_back(user.asString());
            }
        }

        updateAllUsersFile(allUsers);
    }

    void publishClientRelations()
    {
        Json::Value relations;
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            int clientId = std::stoi(client);
            std::vector<std::string> followers = getFollowersOfUser(clientId);

            Json::Value followerList(Json::arrayValue);
            for (const auto &follower : followers)
            {
                followerList.append(follower);
            }

            if (!followerList.empty())
            {
                relations[client] = followerList;
            }
        }

        Json::FastWriter writer;
        std::string message = writer.write(relations);
        
        // post to the queue of all other synchronizers
        publishMessageToAllSynchronizers(message, "_clients_relations_queue");
    }

    void consumeClientRelations(const std::string& message)
    {
        std::vector<std::string> allUsers = get_all_users_func(synchID);

        std::string queueName = "synch" + std::to_string(synchID) + "_clients_relations_queue";

        log(INFO, "[consumeClientRelations]: message received from queue " + queueName + ": " + message);
        Json::Value root;
        Json::Reader reader;
        if (reader.parse(message, root))
        {
            for (const auto &client : allUsers)
            {
                std::string followerFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + client + "_followers.txt";
                std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + client + "_followers.txt";
                sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

                std::ofstream followerStream(followerFile, std::ios::app | std::ios::out | std::ios::in);
                if (root.isMember(client))
                {
                    for (const auto &follower : root[client])
                    {
                        if (!file_contains_user(followerFile, follower.asString()))
                        {
                            followerStream << follower.asString() << std::endl;
                        }
                    }
                }
                sem_close(fileSem);
            }
        }
    }

    // for every client in your cluster, update all their followers' timeline files
    // by publishing your user's timeline file (or just the new updates in them)
    // periodically to the message queue of the synchronizer responsible for that client
    void publishTimelines()
    {
        std::vector<std::string> users = get_all_users_func(synchID);

        for (const auto &client : users)
        {
            int clientId = std::stoi(client);
            int client_cluster = ((clientId - 1) % 3) + 1;
            // only do this for clients in your own cluster
            if (client_cluster != clusterID)
            {
                continue;
            }

            std::vector<std::string> timeline = get_tl_or_fl(synchID, clientId, true);
            std::vector<std::string> followers = getFollowersOfUser(clientId);

            // log(INFO, "[publishTimelines]: client: " + client);

            // Get timeline posts from the client only posted by the client 
            // to send to followers
            Json::Value root;
            Json::Value clientTimeline;
            clientTimeline[client] = Json::Value(Json::arrayValue);
            for (int i = 0; i < timeline.size(); i += 3) {
                int URL_LENGTH_ = 19; // length of "http://twitter.com/"
                std::string username = timeline.at(i + 1).substr(URL_LENGTH_);
                if (username != client) {
                    continue; 
                }
                // Initialize array of postInfo, which takes 3 lines of post metadata
                Json::Value postInfo(Json::arrayValue);
                postInfo.append(timeline.at(i)); // timestamp
                postInfo.append(timeline.at(i+1)); // URL
                postInfo.append(timeline.at(i+2)); // post content
                clientTimeline[client].append(postInfo);
            }

            if (clientTimeline[client].empty())
            {
                // log(INFO, "[publishTimelines]: no new timeline updates for client " + std::to_string(clientId));
                continue;
            }

            root["timeline"] = clientTimeline;
            Json::FastWriter writer;
            std::string message = writer.write(root);

            log(INFO, "[publishTimelines]: message: " + message);

            // Get all available synchronizers from the coordinator
            updateAllSynchronizers();

            for (const auto &follower : followers)
            {
                // send the timeline updates of your current user to all its followers
                // log(INFO, "[publishTimelines]: follower: " + follower);
                // YOUR CODE HERE
                // publish the timeline of the current user to the queue of the synchronizer
                // responsible for the follower
                int followerId = std::stoi(follower);
                int follower_cluster = ((followerId - 1) % 3) + 1;

                // NOTE: Only publish to the synchronizers on OTHER clusters
                if (follower_cluster == clusterID)
                {
                    continue;
                }

                std::lock_guard<std::mutex> lock(otherHostsMutex);

                // Publish client's timeline to all synchronizers on follower's cluster
                for (int i = 0; i < total_number_of_registered_synchronizers; i++) {
                    int followerServerId = std::stoi(otherHosts.at(i));
                    int followerServerCluster = ((followerServerId - 1) % 3) + 1;
                    // Only publish to the synchronizers on the follower's cluster
                    if (follower_cluster == followerServerCluster) {
                        std::string queueName = "synch" + otherHosts.at(i) + "_timeline_queue";
                        publishMessage(queueName, message);
                        log(INFO, "[publishTimelines]: published timeline updates from user " + 
                            std::to_string(clientId) + " to follower synchronizer " + 
                            otherHosts.at(i));
                    }
                }                
            }
        }
    }

    // For each client in your cluster, consume messages from your timeline queue and modify your client's timeline files based on what the users they follow posted to their timeline
    void consumeTimelines(const std::string &message)
    {
        std::string queueName = "synch" + std::to_string(synchID) + "_timeline_queue";
        // consume the message from the queue and update the timeline file of the appropriate client with
        // the new updates to the timeline of the user it follows

        // YOUR CODE HERE
        log(INFO, "[consumeTimelines]: message received from queue " + queueName + ": " + message);

        Json::Value root;
        Json::Reader reader;
        if (reader.parse(message, root))
        {
            if (root.isMember("timeline"))
            {
                log(INFO, "[consumeTimelines (synch " + std::to_string(synchID) +")]: message received from queue " + queueName + ": " + message);
                for (const std::string &client : root["timeline"].getMemberNames())
                {
                    std::vector<std::string> followers = getFollowersOfUser(std::stoi(client));
                    for (const std::string &follower : followers)
                    {
                        // We only want to update the timeline of a follower
                        // in the same cluster we are in
                        int followerId = std::stoi(follower);
                        int followerCluster = ((followerId - 1) % 3) + 1;
                        if (followerCluster != clusterID)
                        {
                            continue;
                        }

                        std::string timelineFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + follower + "_timeline.txt";
                        std::vector<std::string> followerTimeline = get_lines_from_file(timelineFile);
                        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + follower + "_timeline.txt";
                        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

                        std::ofstream timelineStream(timelineFile, std::ios::app | std::ios::out | std::ios::in);
                        if (root["timeline"][client].isArray())
                        {
                            for (const auto &postInfo : root["timeline"][client])
                            {
                                log(INFO, "[consumeTimelines (synch " + std::to_string(synchID) + "]: checking if post " + postInfo[2].asString() + " already exists in follower's timeline");
                                if (postExistsInTimeline(followerTimeline, postInfo))
                                {
                                    continue; // skip if post already exists in follower's timeline
                                }
                                log(INFO, "[consumeTimelines (synch " + std::to_string(synchID) + "]: post " + postInfo[2].asString() + " does not exist in follower's timeline, adding it");

                                // postInfo is an array of 3 elements: timestamp, URL, and post content
                                if (postInfo.size() == 3)
                                {
                                    log(INFO, "[consumeTimelines (synch " + std::to_string(synchID) + "]: adding post to " + timelineFile + ": (" + 
                                        postInfo[0].asString() + ", " + postInfo[1].asString() + ", " + postInfo[2].asString() + ")");
                                    timelineStream << postInfo[0].asString() << std::endl; // timestamp
                                    timelineStream << postInfo[1].asString() << std::endl; // URL
                                    timelineStream << postInfo[2].asString() << std::endl; // post content
                                    timelineStream << std::endl;
                                }
                            }
                        }
                        sem_close(fileSem);
                        log(INFO, "[consumeTimelines (synch " + std::to_string(synchID) +"]: updated timeline " + timelineFile + " with posts from client " + client);
                    }
                }
            }
        }
    }

private:
    void updateAllUsersFile(const std::vector<std::string> &users)
    {

        std::string usersFile = "./cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/all_users.txt";
        std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_all_users.txt";
        sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

        std::ofstream userStream(usersFile, std::ios::app | std::ios::out | std::ios::in);
        for (std::string user : users)
        {
            if (!file_contains_user(usersFile, user))
            {
                userStream << user << std::endl;
            }
        }
        sem_close(fileSem);
    }
};

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ);

class SynchServiceImpl final : public SynchService::Service
{
    // You do not need to modify this in any way
};

void RunServer(std::string coordIP, std::string coordPort, std::string port_no, int synchID)
{
    // localhost = 127.0.0.1
    std::string server_address("127.0.0.1:" + port_no);
    log(INFO, "Starting synchronizer server at " + server_address);
    SynchServiceImpl service;
    // grpc::EnableDefaultHealthCheckService(true);
    // grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Initialize RabbitMQ connection
    // SynchronizerRabbitMQ rabbitMQ("localhost", 5672, synchID);
    SynchronizerRabbitMQ rabbitMQ("rabbitmq", 5672, synchID);

    std::thread t1(run_synchronizer, coordIP, coordPort, port_no, synchID, std::ref(rabbitMQ));

    // Create a consumer thread
    std::thread consumerThread([&rabbitMQ]() {
        while (true) {
            auto result = rabbitMQ.consumeAnyMessageFromQueues(5000); // 5s timeout
    
            if (result) {
                const auto& [queueName, message] = *result;
                
                if (queueName.find("_users_queue") != std::string::npos) {
                    rabbitMQ.consumeUserLists(message);
                } else if (queueName.find("_clients_relations_queue") != std::string::npos) {
                    rabbitMQ.consumeClientRelations(message);
                } else if (queueName.find("_timeline_queue") != std::string::npos) {
                    rabbitMQ.consumeTimelines(message);
                }
            } else {
                // No message received, optional sleep
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
    

    server->Wait();

    //   t1.join();
    //   consumerThread.join();
}

int main(int argc, char **argv)
{
    int opt = 0;
    std::string coordIP;
    std::string coordPort;
    std::string port = "3029";

    while ((opt = getopt(argc, argv, "h:k:p:i:")) != -1)
    {
        switch (opt)
        {
        case 'h':
            coordIP = optarg;
            break;
        case 'k':
            coordPort = optarg;
            break;
        case 'p':
            port = optarg;
            break;
        case 'i':
            synchID = std::stoi(optarg);
            break;
        default:
            std::cerr << "Invalid Command Line Argument\n";
        }
    }

    std::string log_file_name = std::string("synchronizer-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Server starting...");

    coordAddr = coordIP + ":" + coordPort;
    clusterID = ((synchID - 1) % 3) + 1;
    ServerInfo serverInfo;
    serverInfo.set_hostname("localhost");
    serverInfo.set_port(port);
    serverInfo.set_type("synchronizer");
    serverInfo.set_serverid(synchID);
    serverInfo.set_clusterid(clusterID);
    Heartbeat(coordIP, coordPort, serverInfo, synchID);

    RunServer(coordIP, coordPort, port, synchID);
    return 0;
}

void run_synchronizer(std::string coordIP, std::string coordPort, std::string port, int synchID, SynchronizerRabbitMQ &rabbitMQ)
{
    // setup coordinator stub
    std::string target_str = coordIP + ":" + coordPort;
    std::unique_ptr<CoordService::Stub> coord_stub_;
    coord_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(
        grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials())
    ));

    while (true)
    {
        // the synchronizers sync files every 5 seconds
        sleep(5);

        // send heartbeat to coordinator
        ServerInfo serverInfo;
        serverInfo.set_hostname("localhost");
        serverInfo.set_port(port);
        serverInfo.set_type("synchronizer");
        serverInfo.set_serverid(synchID);
        serverInfo.set_clusterid(clusterID);
        Heartbeat(coordIP, coordPort, serverInfo, synchID);

        // below here, you run all the update functions that synchronize the state across all the clusters
        // make any modifications as necessary to satisfy the assignments requirements
        
        // Only the master synchronizer sends out updates to other clusters
        if (isMaster) {
            // Publish user list
            rabbitMQ.publishUserList();

            // Publish client relations
            rabbitMQ.publishClientRelations();

            // Publish timelines
            rabbitMQ.publishTimelines();
        }
    }
    return;
}

std::vector<std::string> get_lines_from_file(std::string filename)
{
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    file.open(filename);
    if (file.peek() == std::ifstream::traits_type::eof())
    {
        // return empty vector if empty file
        // std::cout<<"returned empty vector bc empty file"<<std::endl;
        file.close();
        sem_close(fileSem);
        return users;
    }
    while (file)
    {
        getline(file, user);

        if (!user.empty())
            users.push_back(user);
    }

    file.close();
    sem_close(fileSem);

    return users;
}

void Heartbeat(std::string coordinatorIp, std::string coordinatorPort, ServerInfo serverInfo, int syncID)
{
    // For the synchronizer, a single initial heartbeat RPC acts as an initialization method which
    // servers to register the synchronizer with the coordinator and determine whether it is a master

    std::string coordinatorInfo = coordinatorIp + ":" + coordinatorPort;
    coordinator_stub_ = std::unique_ptr<CoordService::Stub>(CoordService::NewStub(
        grpc::CreateChannel(coordinatorInfo, grpc::InsecureChannelCredentials())
    ));

    // send a heartbeat to the coordinator, which registers your follower synchronizer as either a master or a slave
    // YOUR CODE HERE
    grpc::ClientContext context;
    Confirmation confirmation;
    coordinator_stub_->Heartbeat(&context, serverInfo, &confirmation);
    if (confirmation.status())
    {
        log(INFO, "[Heartbeat]: synchronizer sent heartbeat to coordinator");
    }
    else
    {
        log(ERROR, "[Heartbeat]: synchronizer failed to send heartbeat to coordinator");
    }

    // Ask coordinator for our entry to determine if we are master or slave
    ClientContext coord_context;
    ID id;
    id.set_id(synchID);
    ServerInfo slaveSynchInfo;
    Status status = coordinator_stub_->GetFollowerServer(&coord_context, id, &slaveSynchInfo);

    if (slaveSynchInfo.ismaster()) {
        isMaster = true; // Synchronizer is a master
        if (clusterSubdirectory.empty()) {
            clusterSubdirectory = "1"; // Upon assignment, always maintain the same subdirectory
        }
        log(INFO, "[Heartbeat]: synchronizer is a master");
    } else {
        isMaster = false; // Synchronizer is a slave
        if (clusterSubdirectory.empty()) {
            clusterSubdirectory = "2"; // Upon assignment, always maintain the same subdirectory
        }
        log(INFO, "[Heartbeat]: synchronizer is a slave");
    }
}

bool file_contains_user(std::string filename, std::string user)
{
    std::vector<std::string> users;
    // check username is valid
    std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + filename;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
    users = get_lines_from_file(filename);
    for (int i = 0; i < users.size(); i++)
    {
        // std::cout<<"Checking if "<<user<<" = "<<users[i]<<std::endl;
        if (user == users[i])
        {
            // std::cout<<"found"<<std::endl;
            sem_close(fileSem);
            return true;
        }
    }
    // std::cout<<"not found"<<std::endl;
    sem_close(fileSem);
    return false;
}

std::vector<std::string> get_all_users_func(int synchID)
{
    // read all_users file master and client for correct serverID
    std::string clusterID = std::to_string(((synchID - 1) % 3) + 1);
    std::string master_users_file = "./cluster_" + clusterID + "/1/all_users.txt";
    std::string slave_users_file = "./cluster_" + clusterID + "/2/all_users.txt";
    // take longest list and package into AllUsers message
    std::vector<std::string> master_user_list = get_lines_from_file(master_users_file);
    std::vector<std::string> slave_user_list = get_lines_from_file(slave_users_file);

    if (master_user_list.size() >= slave_user_list.size())
        return master_user_list;
    else
        return slave_user_list;
}

std::vector<std::string> get_tl_or_fl(int synchID, int clientID, bool tl)
{
    std::string master_fn = "cluster_" + std::to_string(clusterID) + "/1/" + std::to_string(clientID);
    std::string slave_fn = "cluster_" + std::to_string(clusterID) + "/2/" + std::to_string(clientID);
    if (tl)
    {
        master_fn.append("_timeline.txt");
        slave_fn.append("_timeline.txt");
    }
    else
    {
        master_fn.append("_followers.txt");
        slave_fn.append("_followers.txt");
    }

    std::vector<std::string> m = get_lines_from_file(master_fn);
    std::vector<std::string> s = get_lines_from_file(slave_fn);

    if (m.size() >= s.size())
    {
        return m;
    }
    else
    {
        return s;
    }
}

std::vector<std::string> getFollowersOfUser(int ID) {
    std::string clientID = std::to_string(ID);

    std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + clientID + "_followers.txt";

    std::vector<std::string> followers = get_lines_from_file(file);

    return followers;

}

bool postExistsInTimeline(const std::vector<std::string> &timeline, const Json::Value &postInfo) {
    for (size_t i = 0; i < timeline.size(); i += 3) {
        // log all timeline and post info
        log(INFO, "[postExistsInTimeline]: checking timeline post: (" + 
            timeline[i] + ", " + timeline[i + 1] + ", " + timeline[i + 2] + 
            ") against postInfo: (" + postInfo[0].asString() + ", " + 
            postInfo[1].asString() + ", " + postInfo[2].asString() + ")");
        if (timeline[i] == postInfo[0].asString() && // timestamp
            timeline[i + 1] == postInfo[1].asString() && // URL
            timeline[i + 2] == postInfo[2].asString()) { // post content
            return true; // Post already exists
        }
    }
    return false; // Post does not exist
}

// std::vector<std::string> getFollowersOfUser(int ID)
// {
//     std::vector<std::string> followers;
//     std::string clientID = std::to_string(ID);
//     std::vector<std::string> usersInCluster = get_all_users_func(synchID);

//     for (auto userID : usersInCluster)
//     { // Examine each user's following file
//         std::string file = "cluster_" + std::to_string(clusterID) + "/" + clusterSubdirectory + "/" + userID + "_follow_list.txt";
//         std::string semName = "/" + std::to_string(clusterID) + "_" + clusterSubdirectory + "_" + userID + "_follow_list.txt";
//         sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);
//         // std::cout << "Reading file " << file << std::endl;
//         if (file_contains_user(file, clientID))
//         {
//             followers.push_back(userID);
//         }
//         sem_close(fileSem);
//     }

//     return followers;
// }
