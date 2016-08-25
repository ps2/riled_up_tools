import subprocess

class I2CChip(object):
    def __init__(self, bus, chip):
        self.bus = bus
        self.chip = chip

    def read(self, addr):
        return int(subprocess.check_output("i2cget -y %d %d %d b" % (self.bus, self.chip, addr), shell=True),16)

    def write(self, addr, value):
        subprocess.check_output("i2cset -y %d %d %d %d b" % (self.bus, self.chip, addr, value), shell=True)


class FAN54040ChargerIC(I2CChip):
    I2C_ADDR = 0x6b

    # i2c registers
    CONTROL0      = 0x0
    CONTROL1      = 0x1
    OREG          = 0x2
    IC_INFO       = 0x3
    IBAT          = 0x4
    VBUS_CONTROL  = 0x5
    SAFETY        = 0x6
    POST_CHARGING = 0x7
    MONITOR0      = 0x10
    MONITOR1      = 0x11
    NTC           = 0x12
    WD_CONTROL    = 0x13

    def __init__(self, bus):
        super(FAN54040ChargerIC, self).__init__(bus, self.I2C_ADDR)

    def configure_defaults(self):
        # This sets Voreg to 4.20V
        self.set_voreg(0x23) 

    def get_status(self):
        return self.read(self.MONITOR0)

    def voreg(self):
        return self.read(self.OREG) >> 2

    def set_voreg(self, val):
        self.write(self.OREG, (val & 0b111111) << 2)

