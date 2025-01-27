sudo apt install -y cmake build-essential autoconf pkg-config libssl-dev git
git clone --recurse-submodules -b v1.57.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON -DgRPC_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR ../..
make -j 4
sudo make install
popd
git clone https://github.com/google/glog.git
cd glog
git checkout 3a0d4d22c5ae0b9a2216988411cfa6bf860cc372
cmake -S . -B build -G "Unix Makefiles"
cmake --build build
sudo cmake --build build --target install
