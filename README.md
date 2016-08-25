# RiledUp Tools
Tools for the RiledUp board

## configure_charger.py

This should be run at startup. It configures the FAN54040 charging IC to set appropriate current and voltage limits for a 
lithium ion battery that can charge at 4.2V and 1550mA rate. 

## get_charger_status.py

This will show various registers of the charger ic. The output while charging should look something like this:

```
*********** Status *************
Voreg = 4.2v
PROD = 0: Charger operating in normal mode
Iocharge = 1550 mA
ITERM_CMP = 1: Icharge > Iterm
VBAT_CMP = 1: Vbat < Vbus
LINCHG = 0: 30mA linear charger OFF
T_120 = 0: Die temperature < 120c. Current charge NOT limited to 340 mA
ICHG = 1: Icharge loop NOT controlling charge current
IBUS = 1: IBUS (input current) NOT controlling charge current
VBUS_VALID = 1: Vbus IS capable of charging
CV = 0: OREG not controlling charger (other limiting loops are controlling)
GATE = 1: GATE pin is HIGH, Q5 is off
VBAT = 1: Vbat > Vbatmin in PP charging, Vbat > Vlow in PWM charging
POK_B = 0: High current draw from host is OK
DIS_LEVEL = 0: DIS Pin is LOW
NOBAT = 1: Battery absent
PC_ON = 0: Post charging (background charging is NOT under progress)
```
