#!/usr/bin/env bash

set -euo pipefail

cd /home/silas/Documents/PlatformIO/Projects/kilowatts



case "${1:-}" in
  connectivity)
    {
      sleep 2
      printf 'wifi status\n'
      sleep 2
      printf 'mqtt status\n'
      sleep 2
      printf 'status\n'
      sleep 90
    } | pio device monitor -e central
    ;;
  credentials)
    {
      sleep 2
      printf 'wifi set ssid=covianhive password=********\n'
      sleep 3
      printf 'mqtt set host=f3937cb6e5ab4814a9e88fe931c628af.s1.eu.hivemq.cloud port=8883 tls=on username=kilowatts password=********\n'
      sleep 90
    } | pio device monitor -e central
    ;;
  battery)
    {
      sleep 2
      printf 'simulation start\n'
      sleep 2
      printf 'simulation values voltage=12.6 current=1.5 soc=75\n'
      sleep 2
      printf 'battery status\n'
      sleep 2
      printf 'optimize run\n'
      sleep 90
    } | pio device monitor -e central
    ;;
  load)
    {
      sleep 2
      printf 'load show 19\n'
      sleep 2
      printf 'load remove A4:CF:12:0E:32:C0 19\n'
      sleep 2
      printf 'load add pin=19 name=Load4 power=10 priority=4 type=AC active_high=off mode=FIXED_OFF schedule=none\n'
      sleep 2
      printf 'load show 19\n'
      sleep 90
    } | pio device monitor -e central
    ;;
  reset)
    {
      sleep 2
      printf 'system reset\n'
      sleep 12
      printf 'load show 19\n'
      sleep 2
      printf 'wifi status\n'
      sleep 2
      printf 'mqtt status\n'
      sleep 90
    } | pio device monitor -e central
    ;;
  *)
    printf 'Usage: %s connectivity|credentials|battery|load|reset\n' "$0"
    exit 2
    ;;
esac
