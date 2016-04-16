#!/bin/bash

set -e

# install docker if it isn't in path
if ! which docker &> /dev/null; then
  sudo apt-get -y update
  sudo apt-get -y install apt-transport-https ca-certificates
  sudo apt-key adv --keyserver hkp://p80.pool.sks-keyservers.net:80 \
    --recv-keys 58118E89F3A912897C070ADBF76221572C52609D
  sudo echo "deb https://apt.dockerproject.org/repo ubuntu-trusty main" \
    > /etc/apt/sources.list.d/docker.list
  sudo apt-get -y update
  sudo apt-get -y install docker-engine
  sudo service docker start  
fi

# add user to docker group if problem running
if ! docker ps &> /dev/null; then
  sudo usermod -aG docker $USER
  echo "Try again after logging out"
  exit 1
fi

docker ps -a &> /dev/null

echo "Docker installed successfully"
