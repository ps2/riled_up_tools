#!/usr/bin/env python

from charger import *

# On RiledUp, The charger IC is on I2C bus 1.
ic = FAN54040ChargerIC(1)

print "Pre-configuration settings:"
print "VOREG = 0x%0x" % ic.voreg()
ic.configure_defaults()
print "Post-configuration settings:"
print "VOREG = 0x%0x" % ic.voreg()

