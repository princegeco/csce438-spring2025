#include <algorithm>
#include <cstdio>
#include <ctime>

#include <google/protobuf/timestamp.pb.h>
#include <google/protobuf/duration.pb.h>
#include <chrono>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <stdlib.h>
#include <unistd.h>
#include <google/protobuf/util/time_util.h>
#include <grpc++/grpc++.h>
#include<glog/logging.h>
#define log(severity, msg) LOG(severity) << msg; google::FlushLogFiles(google::severity); 

#include "coordinator.grpc.pb.h"
#include "coordinator.pb.h"

using google::protobuf::Timestamp;
using google::protobuf::Duration;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using csce438::CoordService;
using csce438::ServerInfo;
using csce438::Confirmation;
using csce438::ID;
using csce438::ServerList;
using csce438::SynchService;


struct zNode{
    int serverID;
    std::string hostname;
    std::string port;
    std::string type;
    std::time_t last_heartbeat;
    bool missed_heartbeat;
    bool isActive();

};

//potentially thread safe 
std::mutex v_mutex;
std::vector<zNode*> cluster1;
std::vector<zNode*> cluster2;
std::vector<zNode*> cluster3;

// creating a vector of vectors containing znodes
std::vector<std::vector<zNode*>> clusters = {cluster1, cluster2, cluster3};


//func declarations
int findServer(std::vector<zNode*> v, int id); 
std::time_t getTimeNow();
void checkHeartbeat();


bool zNode::isActive(){
    bool status = false;
    if(!missed_heartbeat){
        status = true;
    }else if(difftime(getTimeNow(),last_heartbeat) < 10){
        status = true;
    }
    return status;
}


class CoordServiceImpl final : public CoordService::Service {

    Status Heartbeat(ServerContext* context, const ServerInfo* serverinfo, Confirmation* confirmation) override {
        auto grpcClusterId = context->client_metadata().find("cluster-id");
        std::string clusterIdStr(grpcClusterId->second.begin(), grpcClusterId->second.end());
        // Subtract 1 for indexing purposes
        int clusterId = std::stoi(clusterIdStr) - 1; 
        
        // Lock the mutex to ensure thread safety until the function returns
        // This prevents multiple threads from modifying the clusters vector at 
        // the same time
        std::lock_guard<std::mutex> lock(v_mutex);
        for (zNode* server : clusters.at(clusterId)){
            // If server already exists in cluster, update its info
            if(server->serverID == serverinfo->serverid()){
                server->last_heartbeat = getTimeNow();
                server->missed_heartbeat = false;
                confirmation->set_status(true);
                log(INFO, "[Heartbeat]: " + serverinfo->type() + " " + 
                    std::to_string(serverinfo->serverid()) + " in cluster " + 
                    clusterIdStr + " sent a heartbeat");
                return Status::OK;
            }
        }
        // If server not found in cluster, add it
        addServer(serverinfo, clusterId);

        confirmation->set_status(true);
        log(INFO, "[Heartbeat]: added a new server to cluster " + clusterIdStr 
            + ": " + serverinfo->hostname() + ":" + serverinfo->port());
        return Status::OK;
    }

    //function returns the server information for requested client id
    //this function assumes there are always 3 clusters and has math
    //hardcoded to represent this.
    Status GetServer(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        int clientId = id->id();
        int assignedClusterId = getClusterId(clientId);
        // Lock the mutex to ensure thread safety until the function returns
        // This prevents multiple threads from modifying the clusters vector at
        // the same time
        std::lock_guard<std::mutex> lock(v_mutex);
        for (zNode* server : clusters.at(assignedClusterId)) {
            // If server is found in cluster, return its info
            if (server->type == "server" && server->isActive()) {
                // NOTE: no deallocations made if server is inactive
                // this is done purposely, so that checkHeartbeat() can alert
                // us to a missed heartbeat
                serverinfo->set_serverid(server->serverID);
                serverinfo->set_hostname(server->hostname);
                serverinfo->set_port(server->port);
                serverinfo->set_type(server->type);
                log(INFO, "[GetServer]: socket info from server" + 
                    std::to_string(server->serverID) + " in cluster " + 
                    std::to_string(assignedClusterId) + " sent to client " + 
                    std::to_string(clientId));
                return Status::OK;
            }
        }
        log(WARNING, "[GetServer]: server info not found for client " + std::to_string(clientId));
        serverinfo->set_serverid(-1);
        return Status::OK;
    }


/**************************************/
/********** HELPER FUNCTIONS **********/
/**************************************/
    // IMPORTANT!!!
    // REVIEW: This function assumes v_mutex is ALREADY locked by the caller
    void addServer(const ServerInfo* serverinfo, int clusterId) {
        zNode* newServer = new zNode;
        newServer->serverID = serverinfo->serverid();
        newServer->hostname = serverinfo->hostname();
        newServer->port = serverinfo->port();
        newServer->type = serverinfo->type();
        newServer->last_heartbeat = getTimeNow();
        newServer->missed_heartbeat = false;
        clusters.at(clusterId).push_back(newServer);
    }
    
    inline int getClusterId(int clientId){
        return ((clientId - 1) % clusters.size()); 
    }
};

void RunServer(std::string port_no){
    //start thread to check heartbeats
    std::thread hb(checkHeartbeat);
    //localhost = 127.0.0.1
    std::string server_address("127.0.0.1:"+port_no);
    CoordServiceImpl service;
    //grpc::EnableDefaultHealthCheckService(true);
    //grpc::reflection::InitProtoReflectionServerBuilderPlugin();
    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    // Wait for the server to shutdown. Note that some other thread must be
    // responsible for shutting down the server for this call to ever return.
    server->Wait();
}

int main(int argc, char** argv) {

    std::string port = "3010";
    int opt = 0;
    while ((opt = getopt(argc, argv, "p:")) != -1){
        switch(opt) {
            case 'p':
                port = optarg;
                break;
            default:
                std::cerr << "Invalid Command Line Argument\n";
        }
    }

    std::string log_file_name = std::string("coordinator-") + port;
    google::InitGoogleLogging(log_file_name.c_str());
    log(INFO, "Logging Initialized. Coordinator starting...");
    RunServer(port);

    return 0;
}



void checkHeartbeat(){
    while(true){
        //check servers for heartbeat > 10
        //if true turn missed heartbeat = true
        // Your code below
        
        v_mutex.lock();

        // iterating through the clusters vector of vectors of znodes
        for (auto& c : clusters){
            for(auto& s : c){
                if(difftime(getTimeNow(),s->last_heartbeat)>10){
                    std::cout << "missed heartbeat from server " << s->serverID << std::endl;
                    if(!s->missed_heartbeat){
                        s->missed_heartbeat = true;
                        s->last_heartbeat = getTimeNow();
                    }
                }
            }
        }

        v_mutex.unlock();

        sleep(3);
    }
}


std::time_t getTimeNow(){
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

