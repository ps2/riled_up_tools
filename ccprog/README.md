### Wiring

On the proto version of the board, some electrical modifications are needed:

 * Cut the line that connects RESET_OUT on the edison to RST
 * Connect GP135 to RST
 * For DC and DD, add a 1k pull-up resistor to each line.

### Building

`gcc -o ccprog ccprog.c -lmraa`
