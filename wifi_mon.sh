#!/bin/bash

# Убить всё, что мешает
sudo airmon-ng check kill

# Запустить мониторный режим на wlx...
sudo airmon-ng start wlx088af1287d57

sudo iw dev wlx088af1287d57 interface add mon0 type monitor
sudo ip link set mon0 up
sudo iw dev mon0 set channel 6
