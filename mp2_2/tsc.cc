#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>
#include <unistd.h>
#include <csignal>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity);

#include "client.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

// Coodinator Communication
#include "coordinator.grpc.pb.h"
using csce438::ID;
using csce438::ServerInfo;
using csce438::CoordService;

// Server Communication
#include "sns.grpc.pb.h"
using csce438::Message;
using csce438::ListReply;
using csce438::Request;
using csce438::Reply;
using csce438::SNSService;

void sig_ignore(int sig) {
  std::cout << "Signal caught " + sig;
}

Message MakeMessage(const std::string& username, const std::string& msg) {
    Message m;
    m.set_username(username);
    m.set_msg(msg);
    google::protobuf::Timestamp* timestamp = new google::protobuf::Timestamp();
    timestamp->set_seconds(time(NULL));
    timestamp->set_nanos(0);
    m.set_allocated_timestamp(timestamp);
    return m;
}

IStatus processReply(const Reply& reply) {
  if (reply.msg() == "SUCCESS") {
    return SUCCESS;
  } else if (reply.msg() == "FAILURE_ALREADY_EXISTS") {
    return FAILURE_ALREADY_EXISTS;
  } else if (reply.msg() == "FAILURE_NOT_EXISTS") {
    return FAILURE_NOT_EXISTS;
  } else if (reply.msg() == "FAILURE_INVALID_USERNAME") {
    return FAILURE_INVALID_USERNAME;
  } else if (reply.msg() == "FAILURE_NOT_A_FOLLOWER") {
    return FAILURE_NOT_A_FOLLOWER;
  } else if (reply.msg() == "FAILURE_INVALID") {
    return FAILURE_INVALID;
  } else {
    return FAILURE_UNKNOWN;
  }
}

class Client : public IClient
{
public:
  Client(const std::string& cIP,
	 const std::string& cPort,
	 const std::string& uId)
    :coordinatorIP(cIP), coordinatorPort(cPort), userId(uId) {}

  
protected:
  virtual int connectTo();
  virtual IReply processCommand(std::string& input);
  virtual void processTimeline();

private:
  std::string coordinatorIP;
  std::string coordinatorPort;
  std::string userId;
  
  // You can have an instance of coordinator 
  // and server stub
  // as member variables.
  std::unique_ptr<CoordService::Stub> coordinator_stub_;
  std::unique_ptr<SNSService::Stub> server_stub_;

  // Login Helper Function
  IReply ServerLogin();
  
  IReply Login();
  IReply List();
  IReply Follow(const std::string &username);
  IReply UnFollow(const std::string &username);
  void   Timeline(const std::string &username);
};


///////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////
int Client::connectTo()
{
  // ------------------------------------------------------------
  // In this function, you are supposed to create a stub so that
  // you call service methods in the processCommand/processTimeline
  // functions. That is, the stub should be accessible when you want
  // to call any service methods in those functions.
  // Please refer to gRpc tutorial how to create a stub.
  // ------------------------------------------------------------
  // Construct coordinator address
  std::string coordinator_address = coordinatorIP + ":" + coordinatorPort;
  // Create a gRPC channel to the coordinator
  auto channel = grpc::CreateChannel(coordinator_address, 
    grpc::InsecureChannelCredentials());
  // Instantiate the stub using the created channel
  coordinator_stub_ = CoordService::NewStub(channel);
  // Attempt to log in immediately after creating the stub
  IReply ire = Login();
  if (ire.comm_status != SUCCESS) {
    return -1; // Indicate failure to connect & log in
  }
  log(INFO, "Successfully logged into TinySNS server");
  // Return success
  return 1;
}

IReply Client::processCommand(std::string& input)
{
  // ------------------------------------------------------------
  // GUIDE 1:
  // In this function, you are supposed to parse the given input
  // command and create your own message so that you call an 
  // appropriate service method. The input command will be one
  // of the followings:
  //
  // FOLLOW <username>
  // UNFOLLOW <username>
  // LIST
  // TIMELINE
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // GUIDE 2:
  // Then, you should create a variable of IReply structure
  // provided by the client.h and initialize it according to
  // the result. Finally you can finish this function by returning
  // the IReply.
  // ------------------------------------------------------------
  
  
  // ------------------------------------------------------------
  // HINT: How to set the IReply?
  // Suppose you have "FOLLOW" service method for FOLLOW command,
  // IReply can be set as follow:
  // 
  //     // some codes for creating/initializing parameters for
  //     // service method
  //     IReply ire;
  //     grpc::Status status = stub_->FOLLOW(&context, /* some parameters */);
  //     ire.grpc_status = status;
  //     if (status.ok()) {
  //         ire.comm_status = SUCCESS;
  //     } else {
  //         ire.comm_status = FAILURE_NOT_EXISTS;
  //     }
  //      
  //      return ire;
  // 
  // IMPORTANT: 
  // For the command "LIST", you should set both "all_users" and 
  // "following_users" member variable of IReply.
  // ------------------------------------------------------------

    // Declare variables
    IReply ire;
    std::istringstream iss(input);
    std::string cmd, username2;
    iss >> cmd;

    // Process commands
    if (cmd == "LIST") {
      ire = List();
    } else if (cmd == "FOLLOW" || cmd == "UNFOLLOW") {
      iss >> username2;
      if (cmd == "FOLLOW") {
        ire = Follow(username2);
      } else { // UNFOLLOW
        ire = UnFollow(username2);
      }
    } else if (cmd == "TIMELINE") {
      // Do not allow processTimeline() to be executed from client.cc
      // Instead, if processTimeline returns, set the IReply to indicate failure
      // and return it to client.cc
      processTimeline();
      ire.grpc_status = Status::OK;
      ire.comm_status = FAILURE_UNKNOWN;
    } else {
      ire.grpc_status = Status::OK;
      ire.comm_status = FAILURE_INVALID;
    }

    return ire;
}


void Client::processTimeline()
{
    Timeline(userId);
}

// List Command
IReply Client::List() {

  // Declare variables
  IReply ire;
  ClientContext context;
  Request request;
  ListReply list_reply;
  Status status;

  // Set up and send request to server
  request.set_username(userId);
  status = server_stub_->List(&context, request, &list_reply);

  // Return error if request fails
  if (!status.ok()) {
    log(WARNING, "[List]: request failed: " + status.error_message());
    ire.grpc_status = Status::OK; // refer to displayCommandReply() in client.cc
    ire.comm_status = FAILURE_UNKNOWN;
    return ire;
  }

  // Populate iReply structure
  log(INFO, "[List]: request succeeded: ");
  ire.grpc_status = status;
  ire.comm_status = SUCCESS;
  ire.all_users.assign(list_reply.all_users().begin(), list_reply.all_users().end());
  ire.followers.assign(list_reply.followers().begin(), list_reply.followers().end());

  return ire;
}

// Follow Command        
IReply Client::Follow(const std::string& username2) {

  // Declare variables
  IReply ire;
  ClientContext context;
  Request request;
  Reply reply;
  Status status;
    
  // Set up and send request to server
  request.set_username(userId);
  request.add_arguments(username2);
  status = server_stub_->Follow(&context, request, &reply);

  // Return error if request fails
  if (!status.ok()) {
    log(WARNING, "[Follow]: request failed: " + status.error_message());
    ire.grpc_status = Status::OK; // refer to displayCommandReply() in client.cc
    ire.comm_status = FAILURE_UNKNOWN;
    return ire;
  }

  // Populate iReply structure
  ire.grpc_status = status;
  ire.comm_status = processReply(reply);

  return ire;
}

// UNFollow Command  
IReply Client::UnFollow(const std::string& username2) {

  // Declare variables
  IReply ire;
  ClientContext context;
  Request request;
  Reply reply;
  Status status;

  // Set up and send request to server
  request.set_username(userId);
  request.add_arguments(username2);
  status = server_stub_->UnFollow(&context, request, &reply);

  // Return error if request fails
  if (!status.ok()) {
    log(WARNING, "[UnFollow]: request failed: " + status.error_message());
    ire.grpc_status = Status::OK; // refer to displayCommandReply() in client.cc
    ire.comm_status = FAILURE_UNKNOWN;
    return ire;
  }

  // Populate iReply structure
  ire.grpc_status = status;
  ire.comm_status = processReply(reply);

  return ire;
}

/* Login Helper */
IReply Client::ServerLogin() {
  // Declare variables
  IReply ire;
  ClientContext context;
  Request request;
  Reply reply;
  Status status;

  // Set up and send request to server
  request.set_username(userId);
  status = server_stub_->Login(&context, request, &reply);

  // Return error if request fails
  if (!status.ok()) {
    log(WARNING, "[Login]: request failed: " + status.error_message());
    ire.grpc_status = Status::OK; // refer to displayCommandReply() in client.cc
    ire.comm_status = FAILURE_UNKNOWN;
    return ire;
  }

  // Populate iReply structure
  ire.grpc_status = status;
  ire.comm_status = processReply(reply);

  return ire;
}


// Login Command  
IReply Client::Login() {

  log(INFO, "[Login]: reaching out to coordinator...");

  // Declare variables
  ClientContext context;
  ID id;
  ServerInfo server_info;
  Status status;

  // Set up and send request to server
  id.set_id(std::stoi(userId));
  status = coordinator_stub_->GetServer(&context, id, &server_info);

  // Create server stub if connection is successful
  if (status.ok() && server_info.serverid() != -1) {
    std::string server_address = server_info.hostname() + ":" + server_info.port();
    auto channel = grpc::CreateChannel(server_address, 
      grpc::InsecureChannelCredentials());
    server_stub_ = SNSService::NewStub(channel);
  } else {
    log(WARNING, "Failed to connect to server: " + status.error_message());
    return IReply{status, FAILURE_INVALID};
  }

  log(INFO, "[Login]: retrieved server info...connecting to server at " 
    + server_info.hostname() + ":" + server_info.port());
  
  return ServerLogin();
}

// Timeline Command
void Client::Timeline(const std::string& username) {

  // ------------------------------------------------------------
  // In this function, you are supposed to get into timeline mode.
  // You may need to call a service method to communicate with
  // the server. Use getPostMessage/displayPostMessage functions 
  // in client.cc file for both getting and displaying messages 
  // in timeline mode.
  // ------------------------------------------------------------

  // ------------------------------------------------------------
  // IMPORTANT NOTICE:
  //
  // Once a user enter to timeline mode , there is no way
  // to command mode. You don't have to worry about this situation,
  // and you can terminate the client program by pressing
  // CTRL-C (SIGINT)
  // ------------------------------------------------------------
  ClientContext context;
  std::shared_ptr<ClientReaderWriter<Message, Message>> stream(
      server_stub_->Timeline(&context));

  // Write initial message to identify user
  Message init_m = MakeMessage(username, "");
  // Return error if request fails
  if (!stream->Write(init_m)) {
    log(WARNING, "[Timeline]: request failed");
    return;
  }
  log(INFO, "[Timeline]: stream created successfully");
  std::cout << "Command completed successfully\n";
  std::cout << "Now you are in the timeline\n";

  // Start thread for reading incoming messages
  std::thread reader([&]() {
      Message server_msg;
      while (stream->Read(&server_msg)) {
        std::time_t time = server_msg.timestamp().seconds();
        displayPostMessage(server_msg.username(),
                            server_msg.msg(), time);
      }
  });
  
  // Start thread for writing messages
  std::thread writer([&]() {
    while (1) {
      // Prepare message
      std::string msg = getPostMessage();
      Message m = MakeMessage(username, msg);
      // Send message 
      stream->Write(m);
    }
    stream->WritesDone();
  });

  reader.join();
  writer.join();

  Status status = stream->Finish();
  if (!status.ok()) {
      std::cerr << "Timeline RPC failed" << std::endl;
  }
}


//////////////////////////////////////////////
// Main Function
/////////////////////////////////////////////
int main(int argc, char** argv) {

  std::string coordinatorIP = "localhost";
  std::string coordinatorPort = "9090";
  std::string userId = "1";
    
  int opt = 0;
  while ((opt = getopt(argc, argv, "h:k:u:")) != -1){
    switch(opt) {
    case 'h':
      coordinatorIP = optarg;break;
    case 'k':
      coordinatorPort = optarg;break;
    case 'u':
      userId = optarg;break;
    default:
      std::cout << "Invalid Command Line Argument\n";
    }
  }
      
  std::string log_file_name = std::string("client-") + userId;
  google::InitGoogleLogging(log_file_name.c_str());
  log(INFO, "Logging Initialized. Client starting...");
  
  Client myc(coordinatorIP, coordinatorPort, userId);
  
  myc.run();
  
  return 0;
}
