
# Pull the docker image and launch a docker container

Start a new terminal window to download and use a docker image with all environments setup for you (use sudo if docker commands ask for permission):

    git clone https://github.com/LENSS/csce438-spring2025.git

    cd csce438-spring2025

    docker pull liuyidockers/csce438_env:latest

    docker run -it --name csce438_container -v $(pwd)/mp1_skeleton:/home/csce438/mp1_skeleton liuyidockers/csce438_env:latest

    cd mp1_skeleton


# try using server and client:

Compile the code using the provided makefile:

    make -j4

To clear the directory (and remove .txt files):
   
    make clean

To run the server without glog messages (port number is optional): 

    ./tsd <-p port>
    
To run the server with glog messages: 

    GLOG_logtostderr=1 ./tsd <-p port>


To run the client (port number and host address are optional), you need to open another terminal window, and enter into the launched docker container: 

    docker exec -it csce438_container bash
    cd mp1_skeleton
    ./tsc <-h host_addr -p port> -u user1
    
To run the server with glog messages: 

    GLOG_logtostderr=1 ./tsc <-h host_addr -p port> -u user1
