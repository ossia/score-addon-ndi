#!/bin/bash
rm -rf release
mkdir -p release
cp -rf NDI *.{hpp,cpp,txt,json} LICENSE release/

mkdir -p release/3rdparty
cp -rf 3rdparty/NDI release/3rdparty

mv release score-addon-ndi
7z a score-addon-ndi.zip score-addon-ndi
