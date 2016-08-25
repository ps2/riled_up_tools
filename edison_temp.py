#!/usr/bin/env python

import subprocess

def get_temp_for_zone(zone):
    file = "/sys/class/thermal/thermal_zone%d/temp" % zone
    f = open(file, 'r')
    temp = int(f.read()) / 1000.0
    f.close()
    return temp

print "Zone 3 temp = %gC" % get_temp_for_zone(3)
print "Zone 4 temp = %gC" % get_temp_for_zone(4)

