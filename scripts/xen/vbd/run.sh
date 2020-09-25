#!/bin/bash

SRC_DIR="$(dirname $0)"
SRC_IMAGE="$SRC_DIR/../../../build/base/bin/embox"
SRC_CONFIG="$SRC_DIR/embox.cfg"

DIR="/tmp/vbd"
CONFIG="$DIR/embox.cfg"
IMAGE="$DIR/embox"
DISK="/embox/disk.img"

mkdir -p $DIR

cp $SRC_CONFIG $CONFIG
cp $SRC_IMAGE $IMAGE

echo "kernel = \"$IMAGE\"" >> $CONFIG
echo "disk = ['$DISK,,xvda']" >> $CONFIG

xl -vvv create -c -f $CONFIG $@
