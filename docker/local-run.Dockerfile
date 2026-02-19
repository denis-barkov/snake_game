FROM amazonlinux:2023

RUN dnf -y update && \
    dnf -y install \
      git \
      clang \
      boost-devel \
      cmake \
      gcc-c++ \
      libcurl-devel \
      openssl-devel \
      zlib-devel \
      make && \
    dnf clean all

# Build AWS SDK C++ once in the image (core+dynamodb).
RUN git clone --depth 1 --branch 1.11.676 --recurse-submodules https://github.com/aws/aws-sdk-cpp.git /opt/aws-sdk-cpp && \
    cmake -S /opt/aws-sdk-cpp -B /opt/aws-sdk-cpp/build \
      -DBUILD_ONLY="dynamodb" \
      -DCMAKE_BUILD_TYPE=Release \
      -DENABLE_TESTING=OFF && \
    cmake --build /opt/aws-sdk-cpp/build -j2 && \
    cmake --install /opt/aws-sdk-cpp/build && \
    echo -e "/usr/local/lib64\n/usr/local/lib" > /etc/ld.so.conf.d/aws-sdk-cpp.conf && \
    ldconfig || true

WORKDIR /work
