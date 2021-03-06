# !/bin/bash
TOPDIR=`pwd`
IPADDR="$(ifconfig | grep -A 1 'eth0' | tail -1 | cut -d ':' -f 2 | cut -d ' ' -f 1)"
PORT="81"

# Compile from source in the same directory
gcc -o $TOPDIR/NonSSLClient $TOPDIR/restclient.c

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 1"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 2"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 3"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 4"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 5"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 6"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 7"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 8"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 9"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 10"

$TOPDIR/NonSSLClient $IPADDR $PORT "TEST 11"

rm -f $TOPDIR/NonSSLClient

