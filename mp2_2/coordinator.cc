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
#include <grpcpp/grpcpp.h>
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
using csce438::AllUsers;
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
    bool isMaster;
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

// create a vector of clients according to which cluster they are in
std::vector<std::unordered_set<std::string>> users(3);

//func declarations
int findServer(std::vector<zNode*> v, int id); 
std::time_t getTimeNow();
void checkHeartbeat();


bool zNode::isActive(){
    bool status = false;
    if(!missed_heartbeat){
        status = true;
    } else if(difftime(getTimeNow(),last_heartbeat) < 10){
        status = true;
    }
    return status;
}


class CoordServiceImpl final : public CoordService::Service {

    // So synchronizers can add users all_users.txt and then publish
    Status GetUsersOnCluster(ServerContext* context, const ID* id, AllUsers* allusers) override {
        int synchId = id->id();
        int assignedClusterId = getClusterId(synchId);
        for (std::string user : users[assignedClusterId]) {
            allusers->add_users(user);
        }
        log(INFO, "[GetUsersOnCluster]: sent users on cluster " + 
            std::to_string(assignedClusterId) + " to synch " + std::to_string(synchId));
        return Status::OK;
    }

    // So synchronizers may know if they are a master
    Status GetFollowerServer(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        int synchId = id->id();
        int assignedClusterId = getClusterId(synchId);
        std::lock_guard<std::mutex> lock(v_mutex);
        for (zNode* server : clusters.at(assignedClusterId)) {
            if (server->type == "synchronizer" && server->serverID == synchId) {
                log(INFO, "[GetFollowerServer]: sent follower server info for synch " + 
                    std::to_string(synchId) + " in cluster " + std::to_string(assignedClusterId + 1));
                buildServerInfoGRPC(serverinfo, server);
                return Status::OK;
            }
        }
        log(WARNING, "[GetFollowerServer]: No follower server found for synch " + 
            std::to_string(synchId));
        return Status(grpc::StatusCode::NOT_FOUND, "Follower server not found");
    }

    // So synchronizers have an update registry of synchronizers to publish to 
    // and consume from
    Status GetAllFollowerServers(ServerContext* context, const ID* id, ServerList* serverlist) override {
        int synchId = id->id();
        int assignedClusterId = getClusterId(synchId);

        std::lock_guard<std::mutex> lock(v_mutex);

        // Add all active follower synchronizers to the serverlist
        for (std::vector<zNode*> cluster : clusters) {
            for (zNode* server : cluster) {
                if (server->serverID != synchId && 
                    server->type == "synchronizer" && 
                    server->isActive()
                ) {
                    serverlist->add_serverid(server->serverID);
                    serverlist->add_hostname(server->hostname);
                    serverlist->add_port(server->port);
                    serverlist->add_type(server->type);
                }
            }
        }
        log(INFO, "[GetAllFollowerServers]: sending list of follower synchronizers of size " 
            + std::to_string(serverlist->serverid_size()));
        return Status::OK;
    }

    // For servers to call
    Status GetSlave(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        // Subtract 1 for indexing purposes
        int clusterId = id->id() - 1;

        std::lock_guard<std::mutex> lock(v_mutex);
        for (zNode* server : clusters.at(clusterId)) {
            log(INFO, "[GetSlave]: checking " + server->type + std::to_string(server->serverID) + 
                " in cluster " + std::to_string(clusterId + 1) + " for activity");
            // Slave server found and is active
            bool slaveServerActive = (server->type == "server" && 
                !server->isMaster && server->isActive());
            if (slaveServerActive) {
                log(INFO, "[GetSlave]: retrieved slave from cluster " 
                    + std::to_string(clusterId + 1));
                buildServerInfoGRPC(serverinfo, server);
                return Status::OK;
            }
        }

        // No active slave server found
        serverinfo->set_serverid(-1);
        log(INFO, "[GetSlave]: unable to retrieve slave from cluster " 
            + std::to_string(clusterId + 1))
        return Status::OK;
    }

    Status Heartbeat(ServerContext* context, const ServerInfo* serverinfo, Confirmation* confirmation) override {
        // Subtract 1 for indexing purposes
        int clusterId = serverinfo->clusterid() - 1; 
        
        // Lock the mutex to ensure thread safety until the function returns
        // This prevents multiple threads from modifying the clusters vector 
        // concurrently
        std::lock_guard<std::mutex> lock(v_mutex);

        updateHeartbeat(serverinfo, clusterId);

        confirmation->set_status(true);

        return Status::OK;
    }

    //function returns the server information for requested client id
    //this function assumes there are always 3 clusters and has math
    //hardcoded to represent this.
    Status GetServer(ServerContext* context, const ID* id, ServerInfo* serverinfo) override {
        int clientId = id->id();
        int assignedClusterId = getClusterId(clientId);

        // store the client to send to synchronizers later
        users[assignedClusterId].insert(
            std::to_string(clientId)
        );

        // Lock the mutex to ensure thread safety until the function returns
        // This prevents multiple threads from modifying the clusters vector at
        // the same time
        std::lock_guard<std::mutex> lock(v_mutex);

        zNode* slaveServer = nullptr;
        zNode* masterSynchronizer = nullptr;
        zNode* slaveSynchronizer = nullptr;
        for (zNode* server : clusters.at(assignedClusterId)) {
            bool masterServerActive = (
                server->type == "server" 
                && server->isMaster 
                && server->isActive()
            );
            bool slaveServerActive = (
                server->type == "server"
                && !server->isMaster
                && server->isActive()
            );
            bool masterSynchronizerActive = (
                server->type == "synchronizer"
                && server->isMaster
                && server->isActive()
            );
            bool slaveSynchronizerActive = (
                server->type == "synchronizer"
                && !server->isMaster
                && server->isActive()
            );
            if (masterServerActive) {
                // NOTE: no deallocations made if server is inactive
                // this is done purposely, so that checkHeartbeat() can alert
                // us to a missed heartbeat
                buildServerInfoGRPC(serverinfo, server);
                log(INFO, "[GetServer]: socket info from server " + 
                    std::to_string(server->serverID) + " in cluster " + 
                    std::to_string(assignedClusterId + 1) + " sent to client " + 
                    std::to_string(clientId));
                return Status::OK;
            } else if (slaveServerActive) {
                slaveServer = server;
            } else if (masterSynchronizerActive) {
                masterSynchronizer = server;
            } else if (slaveSynchronizerActive) {
                slaveSynchronizer = server;
            }
        }

        // If no active master server is found, return the slave server if it exists
        if (slaveServer) {
            log(INFO, "[GetServer]: slave server " + std::to_string(slaveServer->serverID) +
                " promoted to master in cluster " + std::to_string(assignedClusterId + 1));
            slaveServer->isMaster = true; // Promote slave to master
            if (slaveSynchronizer) {
                if (masterSynchronizer) {
                    masterSynchronizer->isMaster = false; // Demote master synchronizer to slave
                }
                log(INFO, "[GetServer]: slave synchronizer " + std::to_string(slaveSynchronizer->serverID) + 
                    " promoted to master in cluster " + std::to_string(assignedClusterId + 1));
                slaveSynchronizer->isMaster = true; // Promote synchronizer to master
            }
            buildServerInfoGRPC(serverinfo, slaveServer);
            log(INFO, "[GetServer]: socket info from server " + 
                std::to_string(slaveServer->serverID) + " in cluster " + 
                std::to_string(assignedClusterId + 1) + " sent to client " + 
                std::to_string(clientId));
            return Status::OK;
        }

        // No servers were found in the client's assigned cluster
        log(WARNING, "[GetServer]: server info not found for client " + std::to_string(clientId));
        serverinfo->set_serverid(-1);
        return Status::OK;
    }


/**************************************/
/********** HELPER FUNCTIONS **********/
/**************************************/
private:
    // IMPORTANT!!!
    // REVIEW: This method assumes v_mutex is ALREADY locked by the caller
    void createServer(const ServerInfo* serverinfo, int clusterId, bool isMaster) {
        zNode* newServer = new zNode;
        newServer->serverID = serverinfo->serverid();
        newServer->hostname = serverinfo->hostname();
        newServer->port = serverinfo->port();
        newServer->type = serverinfo->type();
        newServer->isMaster = isMaster;
        newServer->last_heartbeat = getTimeNow();
        newServer->missed_heartbeat = false;
        clusters.at(clusterId).push_back(newServer); // finally, add the server to the cluster
    }

    // IMPORTANT!!!
    // REVIEW: This method assumes v_mutex is ALREADY locked by the caller
    void buildServerInfoGRPC(ServerInfo* serverinfo, zNode* server) {
        serverinfo->set_serverid(server->serverID);
        serverinfo->set_hostname(server->hostname);
        serverinfo->set_port(server->port);
        serverinfo->set_type(server->type);
        serverinfo->set_clusterid(getClusterId(server->serverID) + 1); // +1 for 1-indexing
        serverinfo->set_ismaster(server->isMaster);
    }
    
    inline int getClusterId(int clientId){
        return ((clientId - 1) % clusters.size()); 
    }

    // IMPORTANT!!!
    // REVIEW: This method assumes v_mutex is ALREADY locked by the caller
    void updateHeartbeat(const ServerInfo* serverinfo, int clusterId) {
        zNode* masterServer = nullptr;
        zNode* masterSynchronizer = nullptr;
        // Iterate through all servers in the cluster
        for (zNode* server : clusters.at(clusterId)){
            // If server already exists in cluster, update its info
            bool serverMatch = (
                server->type == serverinfo->type() 
                && server->serverID == serverinfo->serverid()
            );
            if (serverMatch) {
                server->last_heartbeat = getTimeNow();
                server->missed_heartbeat = false;
                log(INFO, "[Heartbeat]: " + serverinfo->type() + " " + 
                    std::to_string(serverinfo->serverid()) + " in cluster " + 
                    std::to_string(clusterId + 1) + " sent a heartbeat");
                return;
            }
            // Check to see if master server or synchronizer exist
            bool masterServerExists = server->isMaster && server->type == "server";
            bool masterSynchronizerExists = server->isMaster && server->type == "synchronizer";
            if (masterServerExists) {
                masterServer = server;
            } else if (masterSynchronizerExists) {
                masterSynchronizer = server;
            }
        }
    
        // Create a new server, assign it as master or slave
        if (serverinfo->type() == "server") {
            if (masterServer) {
                createServer(serverinfo, clusterId, false); // creates slave
            } else {
                createServer(serverinfo, clusterId, true);
            }
        } else { // synchronizer
            if (masterSynchronizer) {
                createServer(serverinfo, clusterId, false); // creates slave
            } else {
                createServer(serverinfo, clusterId, true);
            }
        }
        log(INFO, "[Heartbeat]: added a new " + serverinfo->type() + " to cluster " + std::to_string(clusterId + 1)
            + ": " + serverinfo->hostname() + ":" + serverinfo->port());
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
        
        v_mutex.lock();

        // iterating through the clusters vector of vectors of znodes
        for (auto& c : clusters){
            for(auto& s : c){
                if(difftime(getTimeNow(),s->last_heartbeat)>10){
                    // std::cout << "missed heartbeat from " + s->type + " " << s->serverID << std::endl;
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

