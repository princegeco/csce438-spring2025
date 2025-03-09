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
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
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
using csce438::ServerInfo;
using csce438::Confirmation;
using csce438::CoordService;


using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::ClientContext;
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


struct Client {
  std::string username;
  bool connected = true;
  int following_file_size = 0;
  std::vector<Client*> client_followers;
  std::vector<Client*> client_following;
  ServerReaderWriter<Message, Message>* stream = 0;
  bool operator==(const Client& c1) const{
    return (username == c1.username);
  }
};

//Vector that stores every client that has been created
std::vector<Client*> client_db;

// func declarations
void sendHeartbeat(int serverId, std::string hostname,
  std::string serverPort);


class SNSServiceImpl final : public SNSService::Service {
  
  Status List(ServerContext* context, const Request* request, ListReply* list_reply) override {
    const std::string& username = request->username();
    for (const Client* client : client_db) {
      list_reply->add_all_users(client->username);
      if (client->username == username) {
        for (const Client* follower : client->client_followers) {
          list_reply->add_followers(follower->username);
        }
      }
      log(INFO, "[List]: sent list info to user " + username);
    }
    return Status::OK;
  }

  Status Follow(ServerContext* context, const Request* request, Reply* reply) override {
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
    auto it = std::find(
      requesting_client->client_following.begin(),
      requesting_client->client_following.end(),
      client_to_follow
    );
    if (it != requesting_client->client_following.end()) { // Already following the user
      reply->set_msg("FAILURE_ALREADY_EXISTS");
      return Status::OK;
    }

    ///////////// Follow the client that the requesting client specified /////////////
    requesting_client->client_following.push_back(client_to_follow);

    ///////////// Add the requesting client to the other client's client_followers /////////////
    client_to_follow->client_followers.push_back(requesting_client);

    ///////////// Store the new follower in the other client's followers file /////////////
    appendToFile(directory + client_to_follow->username + followersFileExt, 
      requesting_client->username);

    reply->set_msg("SUCCESS");
    log(INFO, "[Follow]: user " + username + " has followed " + followee_username);
    return Status::OK; 
  }
  
  Status UnFollow(ServerContext* context, const Request* request, Reply* reply) override {
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
    const std::string& username = request->username();

    ///////////// Query DB to see if user exists and that they are logged in /////////////
    Client* requesting_client = getClient(username);
    if (requesting_client) {
      if (requesting_client->connected) {
        reply->set_msg("FAILURE_INVALID_USERNAME");
        return Status::OK;
      }
      requesting_client->connected = true;
      reply->set_msg("SUCCESS");
      log(INFO, "[Login]: user" + username + " has logged into Tiny SNS")
      return Status::OK;
    }

    ///////////// Create a new client for a first-time user /////////////
    Client* new_client = new Client; // implicitly connected
    new_client->username = username;
    client_db.push_back(new_client);

    reply->set_msg("SUCCESS");
    log(INFO, "[Login]: user " + username + " has registered and logged into Tiny SNS")
    return Status::OK;
  }

  Status Timeline(ServerContext* context, 
		ServerReaderWriter<Message, Message>* stream) override {
    ///////////// Retrieve the initial message which provides the username of the requesting client /////////////
    Message init_m;
    stream->Read(&init_m);
    std::string username = init_m.username();
    log(INFO, "[Timeline]: user " + username + " has entered Timeline mode");
    Client* client = getClient(username);
    client->stream = stream;
    sendLast20Messages(client);

    ///////////// Capture the client's posts, store them, and distribute them to followers /////////////
    Message m;
    while (stream->Read(&m)) {
      Client* client = getClient(username);
      std::string fileOutput = formatFileOutput(m);
      // Store post for persistency
      // appendToFile(username + ".txt", fileOutput); // to store ONLY one's own posts
      appendToFile(directory + username + timelineFileExt, 
        fileOutput); // timeline can have one's own posts
      // Send the message to all followers' timelines
      sendMessageToFollowers(client, m, fileOutput);

      log(INFO, "[Timeline]: user " + username + " has posted a message: " + m.msg());
    }
    
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
    directory = "./server_" + clusterId + "_" + serverId_ + "/";
    timelineFileExt = "_timeline.txt";
    followersFileExt = "_followers.txt";

    // Create server directory if it does not exist
    if (!std::filesystem::exists(directory)) {
      std::filesystem::create_directory(directory);
    }

    // Construct coordinator address
    std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
    // Create a gRPC channel to the coordinator
    auto channel = grpc::CreateChannel(coordinator_address, 
      grpc::InsecureChannelCredentials());
    // Instantiate the stub using the created channel
    coordinator_stub_ = CoordService::NewStub(channel);

    // Send heartbeats to coordinator independently
    std::thread hb([this]() {
      sendHeartbeat();
    });
    hb.detach();
  }

private:
  ///////////// Variables /////////////
  std::string clusterId_;
  std::string serverId_;
  std::string serverIP_;
  std::string serverPort_;

  std::unique_ptr<CoordService::Stub> coordinator_stub_;

  std::string directory;
  std::string timelineFileExt;
  std::string followersFileExt;

  ///////////// Methods /////////////
  // Send heartbeat to coordinator every 5 seconds
  void sendHeartbeat() {
    bool firstHeartbeat = true;
    ServerInfo server_info;
    server_info.set_serverid(std::stoi(serverId_));
    server_info.set_hostname(serverIP_);
    server_info.set_port(serverPort_);
    server_info.set_type("server");
    while (true) {
      // Send heartbeat to coordinator
      Confirmation confirmation;
      ClientContext context;
      context.AddMetadata("cluster-id", clusterId_);
      Status status = coordinator_stub_->Heartbeat(&context, server_info, &confirmation);
      if (!status.ok()) {
        log(ERROR, "Failed to send heartbeat: " + status.error_message());
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

  Client* getClient(const std::string& username) const {
    for (Client* client : client_db) {
      if (client->username == username) {
        return client;
      }
    }
    return nullptr;
  }

  bool follows(const Client* follower, const Client* followee) const {
    for (const Client* client : follower->client_following) {
      if (client->username == followee->username) {
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
      m.msg() + "\n"
    );
  }

  void appendToFile(std::string fileName, std::string fileOutput) {
    std::ofstream of{fileName, std::ios::app};
    of << fileOutput;
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
    std::ifstream ifile(directory + client->username + timelineFileExt);
    const std::size_t NUM_LAST_MESSAGES = 20;
    std::deque<Message> lastNMessages; // sliding window to hold last NUM_LAST_MESSAGES messages
    std::string line;
    std::size_t URL_LENGTH = 19;

    while (std::getline(ifile, line)) {
      // Retrive the post from the timeline file
      std::string timestamp = line;
      std::getline(ifile, line);
      std::string username = line.substr(URL_LENGTH); // omit "http://twitter.com/"
      std::getline(ifile, line);
      std::string msg = line;
      std::getline(ifile, line); // skip the empty line

      // *** INTERESTING SCENARIO ***
      // The client may have unfollowed the followee prior to entering timeline mode,
      // so ensure they are still following the followee to show a post by them
      Client* followee = getClient(username);
      if (!follows(client, followee)) {
        continue;
      }

      // Craft the message to stream
      Message m;
      m.set_username(username);
      m.set_msg(msg);
      m.set_allocated_timestamp(messageTimeToProtoTimestamp(timestamp));

      // Add the message to sliding window
      lastNMessages.push_back(m);

      if (lastNMessages.size() > NUM_LAST_MESSAGES) {
        lastNMessages.pop_front();
      }
    }

    // Stream the 20 most recent timeline messages
    // Order: Most Recent -> Least Recent
    while (!lastNMessages.empty()) {
      Message m = lastNMessages.back();
      client->stream->Write(m);
      lastNMessages.pop_back();
    }
  }

  void sendMessageToFollowers(Client* client, 
    const Message& m, const std::string& fileOutput) {
    for (Client* follower : client->client_followers) {
      // Check if follower has entered timeline mode!
      if (follower->stream) {
        log(INFO, "POST: " + client->username + " --> " + follower->username + "\n");
        follower->stream->Write(m);
      }
      // Store post for persistency
      appendToFile(directory + follower->username + timelineFileExt, 
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
