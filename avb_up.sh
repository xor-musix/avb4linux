#!/bin/bash
# Simple script to run igb_avb

export AVBHOME=$HOME/.avb
export INTERFACE=$1
export SAMPLERATE=$2

# Set the MAC address of the i210
export OWNMAC=a08cfdc31602

#The MAC address of the remote AVB device
export SOURCEMAC=0001f2014ea9

# The MAAP MAC address used by the AVB device for its Talker stream
export TALKERMAC=91e0f000ae53

rmmod igb
rmmod igb_avb
modprobe i2c_algo_bit
#modprobe dca
modprobe ptp
insmod $AVBHOME/igb_avb.ko samplerate=$SAMPLERATE avb_device_talker_mac_base_parm=$TALKERMAC avb_device_source_mac_parm=$SOURCEMAC own_mac_parm=$OWNMAC

ethtool -i $INTERFACE

sleep 1
ifconfig $INTERFACE down
echo 0 > /sys/class/net/$INTERFACE/queues/tx-0/xps_cpus
echo 0 > /sys/class/net/$INTERFACE/queues/tx-1/xps_cpus
echo f > /sys/class/net/$INTERFACE/queues/tx-2/xps_cpus
echo f > /sys/class/net/$INTERFACE/queues/tx-3/xps_cpus
ifconfig $INTERFACE up promisc

sleep 1

echo "Starting daemons on "$INTERFACE

groupadd ptp > /dev/null 2>&1
$AVBHOME/daemon_cl $INTERFACE &
$AVBHOME/mrpd -mvs -i $INTERFACE &
$AVBHOME/maap_daemon -i $INTERFACE -d /dev/null &
$AVBHOME/shaper_daemon -d &

sleep 10

$AVBHOME/avb-user $INTERFACE $SAMPLERATE $TALKERMAC $SOURCEMAC $OWNMAC

