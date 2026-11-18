#!/bin/sh
# launchd wrapper: launching the binary directly from launchd got it SIGKILLed on iOS 6.
exec /var/root/siri/localsirid /var/root/siri
