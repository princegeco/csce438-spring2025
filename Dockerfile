# Use Ubuntu 22.04 as the base image
FROM ubuntu:22.04

# Set environment variables
ENV MY_INSTALL_DIR=/home/csce438/.local
ENV PATH=$MY_INSTALL_DIR/bin:$PATH
ENV LD_LIBRARY_PATH=/usr/local/lib

# Install necessary packages
RUN apt update && apt upgrade -y && \
    apt install -y sudo cmake build-essential autoconf pkg-config libssl-dev git vim wget

# Create the user "csce438" and set up the home directory
RUN useradd -ms /bin/bash csce438 && \
    mkdir -p /home/csce438/.local && \
    chown -R csce438:csce438 /home/csce438

RUN echo "csce438 ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

# Copy the setup files into the container
COPY setup-438-env.sh /home/csce438/setup-438-env.sh

# Set permissions and execute the setup script
RUN chmod 755 /home/csce438/setup-438-env.sh
# executing the setup script directly in this Dockerfile doesn't work for some reason.
# you need to do `./setup-438-env.sh` from /home/csce438 once you exec into the container

# Switch to the csce438 user and set up working directories
USER csce438
WORKDIR /home/csce438
RUN mkdir -p /home/csce438/mp1_skeleton && \
    chown -R csce438:csce438 /home/csce438/mp1_skeleton

# Set the default entry point to bash
CMD ["/bin/bash"]

