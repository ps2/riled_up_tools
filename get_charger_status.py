#!/usr/bin/env python

from charger import *

# On RiledUp, The charger IC is on I2C bus 1.
ic = FAN54040ChargerIC(1)

print "Status = 0x%0x" % ic.get_status()

