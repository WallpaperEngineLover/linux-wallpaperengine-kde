#!/bin/bash
if [ $# -eq 0 ]
  then
    echo "Please provide the current build's executable path. You might want to run this script off the same folder."
    exit
fi

[ -d "output" ] || mkdir output

for folder in ~/.steam/steam/steamapps/workshop/content/431960/*; do
  if [ -d "$folder" ]; then
    bgid=$(basename "$folder")

    echo "Running wallpaperengine for background $bgid and waiting for it to finish"
    $1 $bgid 2>&1 | grep -v "Error receiving video packet: " > output/$bgid.log

    if [ -f "output.webm" ]; then
      mv output.webm output/$bgid.webm
      ffmpeg -ss 00:00:03 -i output/$bgid.webm -frames:v 1  output/$bgid.jpg
      cp $folder/project.json output/$bgid.json
    fi
  fi
done