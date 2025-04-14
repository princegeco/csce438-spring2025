/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>

#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
#include <semaphore.h>
#include <shared_mutex>
#include <string>
#include <stdlib.h>
#include <thread>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include "sns.grpc.pb.h"

// Coodinator Communication
#include "coordinator.grpc.pb.h"
using csce438::ID;
using csce438::Confirmation;
using csce438::CoordService;
using csce438::ServerInfo;


using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::ClientContext;
using grpc::ClientReaderWriter;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce438::Message;
using csce438::ListReply;
using csce438::Request;
using csce438::Reply;
using csce438::SNSService;

struct PostInfo {
  std::string timestamp;
  std::string username;
  std::string msg;
  PostInfo(std::string t, std::string u, std::string m)
    : timestamp(t), username(u), msg(m) {}
  // Makes it easy to sort posts by timestamp
  // More recent posts are prioritized in a priority queue
  bool operator<(const PostInfo& other) const {
    return timestamp < other.timestamp;
  }
};

struct Client {
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client*> client_followers;
  std::vector<Client*> client_following;
  ServerReaderWriter<Message, Message>* stream = 0;
  int messages_streamed = 0;
  bool operator==(const Client& c1) const{
    return (username == c1.username);
  }
};

//Vector that stores every client that has been created
std::vector<Client*> client_db;
std::shared_mutex db_mutex; // TODO: provide more efficient locking mechanism(s)

// func declarations
void sendHeartbeat(int serverId, std::string hostname,
  std::string serverPort);


class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {
    ///////////// Mirror the request to the slave server if it exists /////////////
    connectToSlave();
    if (slave_stub_) { // Only enters if server is the master server
      ClientContext context_slave;
      ListReply slave_reply;
      slave_stub_->List(&context_slave, *request, &slave_reply);
    }
    
    ///////////// Unpack request /////////////
    const std::string& username = request->username();

    ///////////// Add relevant data to reply /////////////
    {
      std::shared_lock<std::shared_mutex> read_lock(db_mutex);
      for (const Client* client : client_db) {
        list_reply->add_all_users(client->username);
        if (client->username == username) {
          for (const Client* follower : client->client_followers) {
            list_reply->add_followers(follower->username);
          }
        }
      }
    } // Release read lock
    std::sort(list_reply->mutable_all_users()->begin(), list_reply->mutable_all_users()->end());
    std::sort(list_reply->mutable_followers()->begin(), list_reply->mutable_followers()->end());
    log(INFO, "[List]: sent list info to user " + username);
    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {
    ///////////// Mirror the request to the slave server if it exists /////////////
    connectToSlave();
    if (slave_stub_) { // Only enters if server is the master server
      ClientContext context_slave;
      Reply slave_reply;
      slave_stub_->Follow(&context_slave, *request, &slave_reply);
    }

    ///////////// Unpack request /////////////
    const std::string& username = request->username();
    const std::string& followee_username = request->arguments(0);

    ///////////// Prevent users from following themselves /////////////
    if (username == followee_username) {
      reply->set_msg("FAILURE_ALREADY_EXISTS");
      return Status::OK;
    }

    ///////////// Find requesting client and client to be followed in the DB /////////////
    Client* requesting_client = getClient(username);
    Client* client_to_follow = getClient(followee_username);

    ///////////// Handle bad behavior /////////////
    if (requesting_client == nullptr) { // may not need this check
      reply->set_msg("FAILURE_NOT_EXISTS");
      return Status::OK;
    }
    if (client_to_follow == nullptr) { // Follow user who does not exist
      reply->set_msg("FAILURE_INVALID_USERNAME");
      return Status::OK;
    }

    { // READ LOCK
      std::shared_lock<std::shared_mutex> read_lock(db_mutex);

      auto it = std::find(
        requesting_client->client_following.begin(),
        requesting_client->client_following.end(),
        client_to_follow
      );
      if (it != requesting_client->client_following.end()) { // Already following the user
        reply->set_msg("FAILURE_ALREADY_EXISTS");
        return Status::OK;
      }
    }

    { // WRITE LOCK
      std::unique_lock<std::shared_mutex> write_lock(db_mutex);

      ///////////// Follow the client that the requesting client specified /////////////
      requesting_client->client_following.push_back(client_to_follow);

      ///////////// Add the requesting client to the other client's client_followers /////////////
      client_to_follow->client_followers.push_back(requesting_client);
    }

    ///////////// Store the new follower in the other client's followers file  /////////////
    appendToFile(directory_ + client_to_follow->username + followersFileExt_, 
      requesting_client->username);
    addPostsToTimelineFile(requesting_client, client_to_follow);

    reply->set_msg("SUCCESS");
    log(INFO, "[Follow]: user " + username + " has followed user " + followee_username);
    return Status::OK; 
  }
  
  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {
    ///////////// Mirror the request to the slave server if it exists /////////////
    connectToSlave();
    if (slave_stub_) { // Only enters if server is the master server
      ClientContext context_slave;
      Reply slave_reply;
      slave_stub_->UnFollow(&context_slave, *request, &slave_reply);
    }

    ///////////// Unpack request /////////////
    const std::string& username = request->username();
    const std::string& followee_username = request->arguments(0);

    // Prevent users from unfollowing themselves
    if (username == followee_username) {
      reply->set_msg("FAILURE_INVALID_USERNAME");
      return Status::OK;
    }

    ///////////// Find requesting client and client to be unfollowed in the DB /////////////
    Client* requesting_client = getClient(username);
    Client* client_to_unfollow = getClient(followee_username);

    ///////////// Handle bad behavior /////////////
    if (requesting_client == nullptr) { // may not need this check
      reply->set_msg("FAILURE_NOT_EXISTS");
      return Status::OK;
    }
    if (client_to_unfollow == nullptr) { // Unfollow user who does not exist
      reply->set_msg("FAILURE_INVALID_USERNAME");
      return Status::OK;
    }
    auto it = std::find(
      requesting_client->client_following.begin(),
      requesting_client->client_following.end(),
      client_to_unfollow
    );
    if (it == requesting_client->client_following.end()) { // Already not following the user
      reply->set_msg("FAILURE_NOT_A_FOLLOWER");
      return Status::OK;
    }
    
    ///////////// Unfollow the client that the requesting client specified /////////////
    auto followee_it = std::find(
      requesting_client->client_following.begin(), 
      requesting_client->client_following.end(), 
      client_to_unfollow);
    requesting_client->client_following.erase(followee_it);

    ///////////// Remove the requesting client from the other client's client_followers /////////////
    auto follower_it = std::find(
      client_to_unfollow->client_followers.begin(),
      client_to_unfollow->client_followers.end(),
      requesting_client
    );
    client_to_unfollow->client_followers.erase(follower_it);

    // TODO: remove unfollower from the other client's followers file

    reply->set_msg("SUCCESS");
    log(INFO, "[Unfollow]: user " + username + " has unfollowed " + followee_username);
    return Status::OK;
  }

  Status Login(ServerContext* context, const Request* request, Reply* reply) override {
    ///////////// Mirror the request to the slave server if it exists /////////////
    connectToSlave();
    if (slave_stub_) { // Only enters if server is the master server
      ClientContext context_slave;
      Reply slave_reply;
      slave_stub_->Login(&context_slave, *request, reply);
    }

    const std::string& username = request->username();

    ///////////// Query DB to see if user exists and that they are logged in /////////////
    Client* requesting_client = getClient(username);
    {
      std::unique_lock<std::shared_mutex> write_lock(db_mutex);

      if (requesting_client) {
        // REVIEW - remove this check for users to be able to login again?
        // if (requesting_client->connected) {
        //   reply->set_msg("FAILURE_INVALID_USERNAME");
        //   return Status::OK;
        // }
        requesting_client->connected = true;
        reply->set_msg("SUCCESS");
        log(INFO, "[Login]: user" + username + " has logged in again to Tiny SNS")
        return Status::OK;
      }

      ///////////// Create a new client for a first-time user /////////////
      Client* new_client = new Client; // implicitly connected
      new_client->username = username;
      client_db.push_back(new_client);
    }
    
    reply->set_msg("SUCCESS");
    log(INFO, "[Login]: user " + username + " has registered and logged into Tiny SNS");
    return Status::OK;
  }

  Status Timeline(ServerContext* context, 
		              ServerReaderWriter<Message, Message>* stream) override {
    ///////////// Retrieve the initial message which provides the username of the requesting client /////////////
    Message init_m;
    stream->Read(&init_m);
    std::string username = init_m.username();
    Client* client = getClient(username);
    log(INFO, "[Timeline]: user " + username + " has entered Timeline mode");

    ///////////// SLAVE /////////////
    if (isSlave()) {
      // Slave simply stores messages to timeline file and forwards them to followers
      return handleStreamAsSlave(stream, client);
    } 

    ///////////// MASTER /////////////

    // Initialize connection to slave (is slave exists) for mirroring messages
    ClientContext slave_context;
    std::shared_ptr<ClientReaderWriter<Message, Message>> slave_stream = nullptr;
    std::thread slave_reader;
    if (slave_stub_) {
      slave_stream = slave_stub_->Timeline(&slave_context);
      slave_reader = std::thread([&slave_stream]() {
        Message m;
        while (slave_stream->Read(&m)) {
          log(INFO, "[Timeline (master)]: slave acknowledged message from user" + 
            m.username());
        }
      });
      slave_stream->Write(init_m); // Echo initial message to slave
    }

    // Set up the client stream
    {
      std::unique_lock<std::shared_mutex> write_lock(db_mutex);
      client->stream = stream;
    }

    sendLast20Messages(client);
    log(INFO, "[Timeline (master)]: sent last " + std::to_string(NUM_LAST_MESSAGES_) + 
      " messages to user " + username);
    ///////////// Capture the client's posts, send to slave, store them, /////////////
    ///////////// and distribute them to followers /////////////
    handleStreamAsMaster(stream, slave_stream, client);

    ///////////// Clean up /////////////
    cleanupTimeline(client, slave_stream, slave_reader);
    
    return Status::OK;
  }

/**************************************/
/********** HELPER FUNCTIONS **********/
/**************************************/
public:
  SNSServiceImpl(std::string clusterId, std::string serverId, 
    std::string coordinatorIP, std::string coordinatorPort,
    std::string serverIP, std::string serverPort) {

    // Initialize server information
    clusterId_ = clusterId;
    serverId_ = serverId;
    serverIP_ = serverIP;
    serverPort_ = serverPort;

    // Initialize file paths for storing posts and followers
    directory_ = "./cluster_" + clusterId_ + "/" + serverId_ + "/";
    timelineFileExt_ = "_timeline.txt";
    followersFileExt_ = "_followers.txt";
    followingFileExt_ = "_follow_list.txt";
    allUsersFilename_ = directory_ + "all_users.txt";

    // Create server directory if it does not exist
    if (!std::filesystem::exists(directory_)) {
      std::filesystem::create_directories(directory_);
    }

    // Construct coordinator address
    std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
    // Create a gRPC channel to the coordinator
    auto channel = grpc::CreateChannel(coordinator_address, 
      grpc::InsecureChannelCredentials());
    // Instantiate the stub using the created channel
    coordinator_stub_ = CoordService::NewStub(channel);

    // Lazily connect to slave if it exists
    slave_stub_ = nullptr;

    ///////////// WORKER THREADS /////////////
    // Send heartbeats to coordinator independently
    std::thread hb([this]() {
      sendHeartbeat();
    });
    hb.detach();

    // Add new clients to client_db independently
    std::thread dbUpdate([this]() {
      addClientsToDB();
    });
    dbUpdate.detach();

    // Add followers to clients independently
    std::thread followerUpdate([this]() {
      addFollowersToClients();
    });
    followerUpdate.detach();

    // Stream unstreamed messages to clients independently
    // This will stream messages to clients who are in Timeline mode and have 
    // not yet received all their messages
    std::thread streamUpdate([this]() {
      streamTimelineMessages();
    });
    streamUpdate.detach();
  }

private:
  ///////////// Variables /////////////
  std::string clusterId_;
  std::string serverId_;
  std::string serverIP_;
  std::string serverPort_;

  std::unique_ptr<CoordService::Stub> coordinator_stub_;
  // If server is master, lazily initialize slave stub
  std::unique_ptr<SNSService::Stub> slave_stub_;

  std::string directory_;
  std::string timelineFileExt_;
  std::string followersFileExt_;
  std::string followingFileExt_;
  std::string allUsersFilename_;

  std::size_t NUM_LAST_MESSAGES_ = 20;
  std::size_t POST_LENGTH_ = 3; // 3 lines per post
  std::size_t URL_LENGTH_ = 19;

  ///////////// METHODS /////////////
  // Send heartbeat to coordinator every 5 seconds
  void sendHeartbeat() {
    // Build the server info message
    ServerInfo serverinfo = buildServerInfoGRPC();

    // Send heartbeat to coordinator every 5 seconds
    bool firstHeartbeat = true;
    while (true) {
      // Send heartbeat to coordinator
      Confirmation confirmation;
      ClientContext context;
      Status status = coordinator_stub_->Heartbeat(&context, serverinfo, &confirmation);
      if (!status.ok()) {
        log(ERROR, "[sendHeartbeat]: failed to send heartbeat: " + status.error_message());
      }
      else {
        if (firstHeartbeat) {
          log(INFO, std::string("[sendHeartbeat]: ") + "registration heartbeat sent to coordinator");
          firstHeartbeat = false;
        }
        else {
          firstHeartbeat = false;
          log(INFO, std::string("[sendHeartbeat]: ") + "heartbeat sent to coordinator");
        } 
      }
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }

  // Add clients to client_db every 5 seconds
  void addClientsToDB() {
    while (true) {
      log(INFO, "[addClientsToDB]: reading through all_users file to add new clients to client_db");
      
      std::string log_msg = "[addClientsToDB]: users in client_db(" + std::to_string(client_db.size()) + "): ";
      std::vector<std::string> users = get_lines_from_file(allUsersFilename_);

      for (const std::string& username : users) {
        // User not found in the client_db, create a new client
        if (!getClient(username)) {
          std::unique_lock<std::shared_mutex> write_lock(db_mutex);
          Client* new_client = new Client;
          new_client->username = username;
          client_db.push_back(new_client);
        }
        if (getClient(username)) {
          log_msg += username + ", ";
        }
      }

      log(INFO, log_msg);
      
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }

  // Add followers to clients every 5 seconds
  void addFollowersToClients() {
    while (true) {
      log(INFO, "[addFollowersToClients]: reading through followers files to add followers to clients");

      { // WRITE LOCK
        // Lock here to have full control over iteration and modification of client_db
        std::unique_lock<std::shared_mutex> write_lock(db_mutex);

        for (Client* client : client_db) {
          std::string filename = directory_ + client->username + followersFileExt_;
          auto followers = get_lines_from_file(filename);
          for (const auto& follower_username : followers) {
            Client* follower = getClient(follower_username, false); // Do NOT acquire read lock
            if (follower && !follows(follower, client, false)) {    // Do NOT acquire read lock
              follower->client_following.push_back(client);
              client->client_followers.push_back(follower);
            }
          }
        }
      }

      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }

  // Stream unstreamed messages to clients every 5 seconds
  void streamTimelineMessages() {
    while (true) {
      log(INFO, "[streamTimelineMessages]: streaming unstreamed messages to clients every 5 seconds");

      { // WRITE LOCK
        // Lock here to have full control over iteration and modification of client_db
        std::unique_lock<std::shared_mutex> write_lock(db_mutex);

        // Go through existing clients to update
        for (Client* client : client_db) {
          // NOTE - this check removes any interaction with clients on 
          // other clusters (the other servers will take care of that)
          int client_cluster = ((std::stoi(client->username) - 1) % 3) + 1;
          if (client_cluster != std::stoi(clusterId_)) {
            continue;
          }

          if (client->connected && client->stream) {

            std::string filename = directory_ + client->username + timelineFileExt_;
            std::vector<std::string> timeline = get_lines_from_file(filename);
            int start_index = client->messages_streamed * POST_LENGTH_;

            // Stream messages starting from the last streamed message + 1
            for (int i = start_index; i < timeline.size(); i += POST_LENGTH_) {
              const std::string& timestamp = timeline.at(i);
              const std::string& username = timeline.at(i + 1).substr(URL_LENGTH_); // remove "http://twitter.com/"
              const std::string& msg = timeline.at(i + 2);

              // Do NOT stream a client's post back to them
              if (username == client->username) {
                client->messages_streamed++;
                continue;
              }

              Message m = buildMessageGRPC(timestamp, username, msg);

              client->stream->Write(m);
              client->messages_streamed++;
              log(INFO, "[streamTimelineMessages]: streamed message \"" + msg + 
                "\" to client " + client->username);
            }
            log(INFO, "[streamTimelineMessages]: total timeline messages for client " + 
              client->username + ": " + std::to_string(client->messages_streamed));
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  }

  // Builds a ServerInfo object for gRPC communication
  csce438::ServerInfo buildServerInfoGRPC() {
    ServerInfo server_info;
    server_info.set_serverid(std::stoi(serverId_));
    server_info.set_hostname(serverIP_);
    server_info.set_port(serverPort_);
    server_info.set_type("server");
    server_info.set_clusterid(std::stoi(clusterId_));
    return server_info;
  }

  // Builds a Message object for gRPC communication
  csce438::Message buildMessageGRPC(const std::string& timestamp, 
    const std::string& username, const std::string& msg) {
    Message m;
    m.set_allocated_timestamp(messageTimeToProtoTimestamp(timestamp));
    m.set_username(username);
    m.set_msg(msg);
    return m;
  }

  // Requests the coordinator for the slave server, if it exists.
  // If the requester is a slave, it will not connect to itself.
  // No slave stub does not mean a server is a master or slave. 
  // However, a slave stub means a server is a master.
  void connectToSlave() {
    if (!slave_stub_) {
      // Setup gRPC call to coordinator to get slave server info
      ClientContext context;
      ID id;
      id.set_id(std::stoi(clusterId_));
      ServerInfo serverinfo;
      Status status = coordinator_stub_->GetSlave(&context, id, &serverinfo);

      // Ensure gRPC is valid, a slave exists, and the caller is not the slave
      if (status.ok() && 
      serverinfo.serverid() != -1 && 
      serverinfo.serverid() != std::stoi(serverId_)) {
        std::string server_address = serverinfo.hostname() + ":" + serverinfo.port();
        auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
        slave_stub_ = SNSService::NewStub(channel);
      }
    }
  }

  bool isSlave() {
    ClientContext coord_context;
    ID id;
    id.set_id(std::stoi(clusterId_));
    ServerInfo serverinfo;
    Status status = coordinator_stub_->GetSlave(&coord_context, id, &serverinfo);
    return (
      status.ok() && 
      serverinfo.serverid() != -1 && 
      serverinfo.serverid() == std::stoi(serverId_)
    );
  }

  ///////////// File I/O /////////////
  std::vector<std::string> get_lines_from_file(std::string filename) {
    std::vector<std::string> users;
    std::string user;
    std::ifstream file;
    std::string semName = "/" + clusterId_ + "_" + serverId_ + "_" + filename;
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

  void appendToFile(std::string fileName, std::string fileOutput) {
    std::string semName = "/" + clusterId_ + "_" + serverId_ + "_" + fileName;
    sem_t *fileSem = sem_open(semName.c_str(), O_CREAT);

    std::ofstream of{fileName, std::ios::app};
    if (!of) {
      std::cerr << "Failed to open file: " << fileName << std::endl;
      sem_close(fileSem);
      return;
    }

    of << fileOutput << std::endl; // Append the output to the file
    of.close();

    sem_close(fileSem);
  }

  ///////////// Timeline helper functions ///////////
  void handleStreamAsMaster(ServerReaderWriter<Message, Message>* stream, 
                            std::shared_ptr<ClientReaderWriter<Message, Message>> slave_stream, 
                            Client* client) {
    Message m;
    while (stream->Read(&m)) {
      std::string fileOutput = formatFileOutput(m);
      
      // Forward message to the slave if it exists
      if (slave_stream) {
        log(INFO, "[Timeline (master)]: Sending message " + m.msg() + " to slave");
        slave_stream->Write(m);
      }

      // Store post for persistency in the timeline file
      appendToFile(directory_ + client->username + timelineFileExt_, fileOutput); 

      // Send the message to all followers' timelines
      sendMessageToFollowers(client, m, fileOutput);
      
      log(INFO, "[Timeline (master)]: user " + m.username() + 
        " has posted a message: \"" + m.msg() + "\"");
    }
  }

  Status handleStreamAsSlave(ServerReaderWriter<Message, Message>* stream, Client* client) {
    Message m;
    while (stream->Read(&m)) {
      std::string fileOutput = formatFileOutput(m);

      // Store post for persistency in the timeline file
      appendToFile(directory_ + client->username + timelineFileExt_, fileOutput); 

      log(INFO, "[Timeline (slave)]: stored message from user " + m.username() + 
        ": " + m.msg());

      // Send the message to all followers' timelines (if any)
      sendMessageToFollowers(client, m, fileOutput);
      stream->Write(m); // Echo back to master
    }
    log(INFO, "[Timeline (slave)]: user " + client->username + " has disconnected from Timeline mode");
    return Status::OK;
  }

  void cleanupTimeline(Client* client, 
                       std::shared_ptr<ClientReaderWriter<Message, Message>> slave_stream, 
                       std::thread& slave_reader) {
    if (slave_stream) {
      // Close the slave stream and join the thread
      slave_stream->WritesDone(); // Notify slave that no more messages will be sent
      slave_stream->Finish(); // Finish the stream on the slave side
      if (slave_reader.joinable()) {
        slave_reader.join();
      }
      log(INFO, "[Timeline (master)]: finished reading from slave stream");
    }

    {
      std::unique_lock<std::shared_mutex> write_lock(db_mutex);
      client->stream = nullptr; // Clear the stream when the client disconnects
      // Reset the number of messages streamed to 0 (will allow for messages to 
      // be streamed upon re-entering Timeline mode)
      client->messages_streamed = 0;
    }

    log(INFO, "[Timeline (master)]: user " + client->username + " has disconnected from Timeline mode");
  }

  /*
    This function will add the posts from the followee to the follower's timeline file.
    This is used when a new follower is added to ensure they get the posts from the followee.
  */
  void addPostsToTimelineFile(Client* follower, Client* followee) {
    std::string filename = directory_ + followee->username + timelineFileExt_;
    std::vector<std::string> timeline = get_lines_from_file(filename);

    for (std::size_t i = 0; i < timeline.size(); i += POST_LENGTH_) {
      const std::string& timestamp = timeline.at(i);
      const std::string& username = timeline.at(i + 1).substr(URL_LENGTH_);
      const std::string& msg = timeline.at(i + 2);

      if (username != followee->username) {
        // Skip if the post does not belong to the followee, as we only want to add their posts
        continue;
      }

      std::string fileOutput = formatFileOutput(buildMessageGRPC(timestamp, username, msg));
      appendToFile(directory_ + follower->username + timelineFileExt_, fileOutput);
    }
  }

  // NOTE: This function CAN be thread-safe!
  Client* getClient(const std::string& username, bool acquireReadLock = true) const {
    if (acquireReadLock) {
      std::shared_lock<std::shared_mutex> read_lock(db_mutex);
      return getClientHelper(username);
    }
    else {
      return getClientHelper(username);
    }

  }

  Client* getClientHelper(const std::string& username) const {
    for (Client* client : client_db) {
      if (client->username == username) {
        return client;
      }
    }
    return nullptr;
  }

  // NOTE: This function CAN be thread-safe!
  bool follows(const Client* follower, const Client* followee, 
               bool acquireReadLock = true) const {
    if (acquireReadLock) {
      std::shared_lock<std::shared_mutex> read_lock(db_mutex);
      return followsHelper(follower, followee);
    }
    else {
      return followsHelper(follower, followee);
    }
  }

  bool followsHelper(const Client* follower, const Client* followee) const {
    for (const Client* client : follower->client_following) {
      if (client == followee) {
        return true;
      }
    }
    return false;
  }

  std::string protoTimestampToString(const google::protobuf::Timestamp& timestamp) {
    // Convert seconds to std::time_t
    std::time_t raw_time = timestamp.seconds();
    // Convert to struct tm
    std::tm* time_info = std::gmtime(&raw_time);
    // Format time as a string using strftime
    char buffer[20];  // Enough to hold "YYYY-MM-DD HH:MM:SS"
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", time_info);
    std::ostringstream time;
    time << buffer;
    return time.str();
  }

  std::string formatFileOutput(const Message& m) {
    return (
      protoTimestampToString(m.timestamp()) + "\n" +
      "http://twitter.com/" + m.username() + "\n" +
      m.msg()
    );
  }

  google::protobuf::Timestamp* messageTimeToProtoTimestamp(const std::string& date_str) {
    std::istringstream ss(date_str);
    std::tm tm = {};
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");

    if (ss.fail()) {
        throw std::runtime_error("Failed to parse date string");
    }

    // Convert std::tm to std::time_t
    std::time_t time = std::mktime(&tm);
    if (time == -1) {
        throw std::runtime_error("Failed to convert time to std::time_t");
    }

    // Create a Google Protocol Buffers Timestamp
    google::protobuf::Timestamp* timestamp = new google::protobuf::Timestamp();
    timestamp->set_seconds(time);
    timestamp->set_nanos(0);  // You can adjust the nanoseconds if needed

    return timestamp;
  }

  void sendLast20Messages(Client* client) {
    log(INFO, "[Timeline]: Sending last 20 messages to user " + client->username);

    std::string timelineFile = directory_ + client->username + timelineFileExt_;
    std::vector<std::string> timeline = get_lines_from_file(timelineFile);
    std::size_t TIMELINE_POSTS = timeline.size() / POST_LENGTH_;
    std::priority_queue<PostInfo> posts; // max heap (check < operator implementation)

    log(INFO, "[Timeline]: Found " + std::to_string(TIMELINE_POSTS) + 
      " messages in the timeline file " + timelineFile);

    for (int i = 0; i < timeline.size(); i += POST_LENGTH_) {
      // Retrive the post from the timeline file
      const std::string& timestamp = timeline.at(i);
      const std::string& username = timeline.at(i + 1).substr(URL_LENGTH_); // omit "http://twitter.com/"
      const std::string& msg = timeline.at(i + 2);
  
      // *** INTERESTING SCENARIO ***
      // The client may have unfollowed the followee prior to entering timeline mode,
      // so ensure they are still following the followee to show a post by them.
      Client* followee = getClient(username);
      if (followee && !follows(client, followee)) {
        continue;
      }

      posts.push(PostInfo(timestamp, username, msg));
    }

    std::unique_lock<std::shared_mutex> write_lock(db_mutex);

    client->messages_streamed = TIMELINE_POSTS; // Do not want to re-stream these messages
    
    // Stream the last NUM_LAST_MESSAGES_ messages to the client
    int messagesLeftToStream = std::min(NUM_LAST_MESSAGES_, posts.size());
    while (messagesLeftToStream--) {
      const PostInfo& post = posts.top();
      if (client->stream) {
        // Craft the message to stream
        Message m = buildMessageGRPC(post.timestamp, post.username, post.msg);
        // Stream the message to the client
        client->stream->Write(m);
        // Log the entire post that will be streamed to the user
        log(INFO, "[Timeline]: Sending message to user " + client->username + 
          ": (" + post.timestamp + "," + post.username + "," + post.msg + ")");
      }
      posts.pop();
    }
  }

  void sendMessageToFollowers(Client* client,
                              const Message& m, 
                              const std::string& fileOutput) {
    std::unique_lock<std::shared_mutex> write_lock(db_mutex);
    for (Client* follower : client->client_followers) {
      // NOTE - this check removes any follower interaction with followers on 
      // other clusters (the synchronizers will take care of this)
      int followerId = std::stoi(follower->username);
      int follower_cluster = ((followerId - 1) % 3) + 1;
      if (follower_cluster != std::stoi(clusterId_)) {
        continue; // skip followers from other clusters
      }

      if (follower->connected && follower->stream) {
        follower->stream->Write(m);
        follower->messages_streamed++;
      }
      // Store post for persistency
      appendToFile(directory_ + follower->username + timelineFileExt_, 
        fileOutput);
    }
  }

};


void RunServer(std::string clusterId, std::string serverId, 
    std::string coordinatorIP, std::string coordinatorPort,
    std::string serverPort) {
  std::string serverIP = "0.0.0.0";
  std::string server_address = serverIP + ":" + serverPort;
  SNSServiceImpl service(clusterId, serverId, 
    coordinatorIP, coordinatorPort, 
    serverIP, serverPort);

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  log(INFO, "Server listening on " + server_address);

  server->Wait();
}

int main(int argc, char** argv) {

  std::string clusterId = "1";
  std::string serverId = "1";
  std::string coordinatorIP = "localhost";
  std::string coordinatorPort = "9090";
  std::string serverPort = "10000";
  
  int opt = 0;
  while ((opt = getopt(argc, argv, "c:s:h:k:p:")) != -1){
    switch(opt) {
      case 'c':
          clusterId = optarg;break;
      case 's':
          serverId = optarg;break;
      case 'h':
          coordinatorIP = optarg;break;
      case 'k':
          coordinatorPort = optarg;break;
      case 'p':
          serverPort = optarg;break;
      default:
	  std::cerr << "Invalid Command Line Argument\n";
    }
  }
  
  std::string log_file_name = std::string("server-") + serverPort;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Server starting...");
  RunServer(clusterId, serverId, coordinatorIP, coordinatorPort, serverPort);

  return 0;
}
