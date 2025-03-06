#!/bin/bash

# Docker container name
CONTAINER_NAME="csce438_mp2_1_container"
WORKING_DIR="mp2_1"

# Start the Docker container if it's not running
echo "Starting Docker container: $CONTAINER_NAME"
docker start $CONTAINER_NAME

# Wait a moment for container to fully start
sleep 2

# Launch terminal for Coordinator
echo "Starting Coordinator..."
start cmd /k docker exec -it $CONTAINER_NAME bash -c "cd $WORKING_DIR && GLOG_logtostderr=1 ./coordinator -p 9090"

# Wait a bit between terminal launches
sleep 1

# Launch terminals for Servers using a loop
NUM_CLUSTERS=2
NUM_SERVERS_PER_CLUSTER=1
PORT=10000

for (( cluster=1; cluster<=NUM_CLUSTERS; cluster++ )); do
    for (( server=1; server<=NUM_SERVERS_PER_CLUSTER; server++ )); do
        echo "Starting Server - Cluster $cluster, Server $server, Port $PORT"
        start cmd /k docker exec -it $CONTAINER_NAME bash -c "cd $WORKING_DIR && GLOG_logtostderr=1 ./tsd -c $cluster -s $server -h localhost -k 9090 -p $PORT"
        
        # Increment port for next server
        PORT=$((PORT+1))
        
        # Wait a bit between terminal launches
        sleep 1
    done
done

echo "All processes started successfully!"