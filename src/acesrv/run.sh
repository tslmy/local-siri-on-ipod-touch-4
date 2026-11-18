#!/bin/sh
# launchd wrapper (launching the binary directly from launchd got it SIGKILLed on iOS 6).
# assistantd connects to http://guzzoni.apple.com:4443, which /etc/hosts maps to this iPod.
exec /var/root/siri/ace/acesrv -v -a 127.0.0.1 -p 4443
