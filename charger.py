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

        # SAFETY register can only be set at startup, to set limits
        isafe = 0b1010  # 1550 mA
        vsafe = 0b0000  # 4.2v (default)
        val = (isafe << 4) | vsafe
        self.write(self.SAFETY, val)
        self.write(self.SAFETY, val)

        # Disable watchdog timer
        self.write(self.WD_CONTROL, 0b01101110)
        
        # This sets Voreg to 4.20V
        self.set_voreg(4.2)

        # Make input current unlimited, set weak battery voltage (Vlowv) to 3.4v
        #self.write(self.CONTROL1, 0b11000000)
        self.write(self.CONTROL1, 0b11000001)  # Boost mode for USB-OTG

        # Set IO_LEVEL to 1
        self.write(self.VBUS_CONTROL, 0b00000100)

        # Set IOCHARGE to 1550mA, ITERM to default
        self.write(self.IBAT, 0b01010001)

    def print_status(self):
        print "Voreg = %gv" % self.voreg()

        vbus_control = self.read(self.VBUS_CONTROL)
        if (vbus_control & 0b1000000):
            print "PROD = 1: Charger operating in Production Test mode"
        else:
            print "PROD = 0: Charger operating in normal mode"

        if (vbus_control & 0b100000):
            print "IO_LEVEL = 1: Current control is set to 340 mA"
        else:
            print "Iocharge = %d mA" % self.iocharge()

        mon0 = self.read(self.MONITOR0)
        if (mon0 & 0b10000000):
            print "ITERM_CMP = 1: Icharge > Iterm"
        else:
            print "ITERM_CMP = 0: Icharge <= Iterm"

        if (mon0 & 0b1000000):
            print "VBAT_CMP = 1: Vbat < Vbus"
        else:
            print "VBAT_CMP = 0: Vbat >= Vbus"

        if (mon0 & 0b100000):
            print "LINCHG = 1: 30mA linear charger ON (Vbat < Vshort)"
        else:
            print "LINCHG = 0: 30mA linear charger OFF"

        if (mon0 & 0b10000):
            print "T_120 = 1: Die temperature > 120c. Current charge limited to 340 mA"
        else:
            print "T_120 = 0: Die temperature < 120c. Current charge NOT limited to 340 mA"

        if (mon0 & 0b1000):
            print "ICHG = 1: Icharge loop NOT controlling charge current"
        else:
            print "ICHG = 0: ICHARGE loop IS controlling charge current"

        if (mon0 & 0b100):
            print "IBUS = 1: IBUS (input current) NOT controlling charge current"
        else:
            print "IBUS = 0: IBUS (input current) IS controlling charge current"

        if (mon0 & 0b10):
            print "VBUS_VALID = 1: Vbus IS capable of charging"
        else:
            print "VBUS_VALID = 1: Vbus is NOT capable of charging"

        if (mon0 & 0b1):
            print "CV = 1: OREG controlling charger"
        else:
            print "CV = 0: OREG not controlling charger (other limiting loops are controlling)"

        mon1 = self.read(self.MONITOR1)
        if (mon1 & 0b10000000):
            print "GATE = 1: GATE pin is HIGH, Q5 is off"
        else:
            print "GATE = 0: GATE pin is LOW, Q5 is driven on"

        if (mon1 & 0b1000000):
            print "VBAT = 1: Vbat > Vbatmin in PP charging, Vbat > Vlow in PWM charging"
        else:
            print "VBAT = 0: Vbat < Vbatmin in PP charging, Vbat < Vlow in PWM charging"

        if (mon1 & 0b100000):
            print "POK_B = 1: Host should limit power usage"
        else:
            print "POK_B = 0: High current draw from host is OK"

        if (mon1 & 0b10000):
            print "DIS_LEVEL = 1: DIS Pin is HIGH"
        else:
            print "DIS_LEVEL = 0: DIS Pin is LOW"

        if (mon1 & 0b1000):
            print "NOBAT = 1: Battery absent"
        else:
            print "NOBAT = 0: Battery present"
            
        if (mon1 & 0b100):
            print "PC_ON = 1: Post charging (background charging is under progress)"
        else:
            print "PC_ON = 0: Post charging (background charging is NOT under progress)"

        c0 = self.read(self.CONTROL0)
        fault = c0 & 0b111
        
        print "FAULT = " + {
            0: "No Fault",
            1: "VBUS OVP",
            2: "Sleep Mode",
            3: "Poor Input Source",
            4: "Battery OVP",
            5: "Thermal Shutdown",
            6: "Timer Fault",
            7: "No Battery"}[fault]


    def voreg(self):
        v = (self.read(self.OREG) >> 2) * 0.02 + 3.50
        if v > 4.44:
            v = 4.44
        return v

    def set_voreg(self, voltage):
        if voltage > 4.44:
            voltage = 4.44
        if voltage < 3.50:
            voltage = 3.50

        val = int(round((voltage - 3.50) / 0.02))

        self.write(self.OREG, (val & 0b111111) << 2)

    def iocharge(self):
        val = (self.read(self.IBAT) >> 3) & 0xf
        return val * 100 + 550

    def reset_timer(self):
        self.write(self.CONTROL0, 0b1100000)

