#!/bin/bash
set -euo pipefail

echo "Updating apt package index..."
sudo apt update

echo "Installing system dependencies..."
sudo apt-get install -y \
  sudo \
  wget \
  curl \
  gnupg \
  apt-transport-https \
  unzip \
  zip \
  git \
  build-essential \
  g++ \
  zlib1g-dev \
  default-jdk \
  python3-dev \
  python3-pip \
  protobuf-compiler \
  rapidjson-dev \
  clang-format

echo "Installing Bazelisk (ARM64/AMD64 aware)..."
ARCH="$(dpkg --print-architecture)"
case "$ARCH" in
  arm64)
    BAZELISK_URL="https://github.com/bazelbuild/bazelisk/releases/download/v1.19.0/bazelisk-linux-arm64"
    ;;
  amd64)
    BAZELISK_URL="https://github.com/bazelbuild/bazelisk/releases/download/v1.19.0/bazelisk-linux-amd64"
    ;;
  *)
    echo "Unsupported architecture: $ARCH"
    exit 1
    ;;
esac

sudo wget -O /usr/local/bin/bazel "$BAZELISK_URL"
sudo chmod +x /usr/local/bin/bazel

echo "Bazel version:"
bazel --version

echo "Java version:"
java -version

echo "Python version:"
python3 --version

echo "Setting JAVA_HOME..."
JAVA_BIN="$(readlink -f "$(command -v javac)")"
JAVA_HOME_DIR="$(dirname "$(dirname "$JAVA_BIN")")"
if ! grep -q 'export JAVA_HOME=' ~/.bashrc 2>/dev/null; then
  echo "export JAVA_HOME=$JAVA_HOME_DIR" >> ~/.bashrc
fi
export JAVA_HOME="$JAVA_HOME_DIR"

echo "Setting Bazel path if needed..."
if ! grep -q '/usr/local/bin' ~/.bashrc 2>/dev/null; then
  echo 'export PATH=/usr/local/bin:$PATH' >> ~/.bashrc
fi
export PATH=/usr/local/bin:$PATH

echo "Installing Python packages if pip requirements exist..."
if [ -f requirements.txt ]; then
  python3 -m pip install --upgrade pip
  python3 -m pip install -r requirements.txt
fi

echo "Configuring git hook if repository is present..."
if [ -d .git ] && [ -f hooks/pre-push ]; then
  rm -f .git/hooks/pre-push
  ln -s "$(pwd)/hooks/pre-push" .git/hooks/pre-push
fi

echo "INSTALL.sh completed successfully."
