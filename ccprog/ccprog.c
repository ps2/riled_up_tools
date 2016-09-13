#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mraa.h>
#include <math.h>
#include <mraa/gpio.h>


// Programmer data line bitmasks (programmer I/O port 0)
#define DD                          38 // GPIO 43
#define DC                          37 // GPIO 40
#define RESET_N                     4  // GPIO 135

// Start addresses on DUP (Increased buffer size improves performance)
#define ADDR_BUF0                   0x0000 // Buffer (512 bytes)
#define ADDR_DMA_DESC_0             0x0200 // DMA descriptors (8 bytes)
#define ADDR_DMA_DESC_1             (ADDR_DMA_DESC_0 + 8)

// DMA channels used on DUP
#define CH_DBG_TO_BUF0              0x01   // Channel 0
#define CH_BUF0_TO_FLASH            0x02   // Channel 1

// Debug commands
#define CMD_CHIP_ERASE              0x10
#define CMD_WR_CONFIG               0x19
#define CMD_RD_CONFIG               0x24
#define CMD_READ_STATUS             0x30
#define CMD_RESUME                  0x4C
#define CMD_DEBUG_INSTR_1B          (0x54|1)
#define CMD_DEBUG_INSTR_2B          (0x54|2)
#define CMD_DEBUG_INSTR_3B          (0x54|3)
#define CMD_BURST_WRITE             0x80
#define CMD_GET_CHIP_ID             0x68

// Debug status bitmasks
#define STATUS_CHIP_ERASE_BUSY_BM   0x80 // New debug interface
#define STATUS_PCON_IDLE_BM         0x40
#define STATUS_CPU_HALTED_BM        0x20
#define STATUS_PM_ACTIVE_BM         0x10
#define STATUS_HALT_STATUS_BM       0x08
#define STATUS_DEBUG_LOCKED_BM      0x04
#define STATUS_OSC_STABLE_BM        0x02
#define STATUS_STACK_OVERFLOW_BM    0x01

// DUP registers (XDATA space address)
#define DUP_DBGDATA                 0x6260  // Debug interface data buffer
#define DUP_FCTL                    0x6270  // Flash controller
#define DUP_FADDRL                  0x6271  // Flash controller addr
#define DUP_FADDRH                  0x6272  // Flash controller addr
#define DUP_FWDATA                  0x6273  // Clash controller data buffer
#define DUP_CLKCONSTA               0x709E  // Sys clock status
#define DUP_CLKCONCMD               0x70C6  // Sys clock configuration
#define DUP_MEMCTR                  0x70C7  // Flash bank xdata mapping
#define DUP_DMA1CFGL                0x70D2  // Low byte, DMA config ch. 1
#define DUP_DMA1CFGH                0x70D3  // Hi byte , DMA config ch. 1
#define DUP_DMA0CFGL                0x70D4  // Low byte, DMA config ch. 0
#define DUP_DMA0CFGH                0x70D5  // Low byte, DMA config ch. 0
#define DUP_DMAARM                  0x70D6  // DMA arming register

// Utility macros
//! Set programmer DD line as input
#define SET_DD_INPUT()      (mraa_gpio_dir(gpio_dd, MRAA_GPIO_IN))
//! Set programmer DD line as output
#define SET_DD_OUTPUT()     (mraa_gpio_dir(gpio_dd, MRAA_GPIO_OUT))
//! Low nibble of 16bit variable
#define LOBYTE(w)           ((unsigned char)(w))
//! High nibble of 16bit variable
#define HIBYTE(w)           ((unsigned char)(((unsigned short)(w) >> 8) & 0xFF))


/******************************************************************************
* VARIABLES
*/


mraa_gpio_context gpio_rst;
mraa_gpio_context gpio_dc;
mraa_gpio_context gpio_dd;

//! DUP DMA descriptor
const unsigned char dma_desc_0[8] =
{
  // Debug Interface -> Buffer
  HIBYTE(DUP_DBGDATA),            // src[15:8]
  LOBYTE(DUP_DBGDATA),            // src[7:0]
  HIBYTE(ADDR_BUF0),              // dest[15:8]
  LOBYTE(ADDR_BUF0),              // dest[7:0]
  0,                              // len[12:8] - filled in later
  0,                              // len[7:0]
  31,                             // trigger: DBG_BW
  0x11                            // increment destination
};
//! DUP DMA descriptor
const unsigned char dma_desc_1[8] =
{
  // Buffer -> Flash controller
  HIBYTE(ADDR_BUF0),              // src[15:8]
  LOBYTE(ADDR_BUF0),              // src[7:0]
  HIBYTE(DUP_FWDATA),             // dest[15:8]
  LOBYTE(DUP_FWDATA),             // dest[7:0]
  0,                              // len[12:8] - filled in later
  0,                              // len[7:0]
  18,                             // trigger: FLASH
  0x42,                           // increment source
};
static unsigned char write_data[4] = {0x55, 0xAA, 0x55, 0xAA};
static unsigned char read_data[4];


/**************************************************************************//**
* @brief    Writes a byte on the debug interface. Requires DD to be
*           output when function is called.
*
* @param    data    Byte to write
*
* @return   None.
******************************************************************************/
#pragma inline
void write_debug_byte(unsigned char data)
{
  unsigned char i;
  for (i = 0; i < 8; i++)
  {
    // Set clock high and put data on DD line
    mraa_gpio_write(gpio_dc, 1);
    mraa_gpio_write(gpio_dd, (data & 0x80) ? 1 : 0);
    data <<= 1;
    mraa_gpio_write(gpio_dc, 0); // set clock low (DUP capture flank)
  }
}


/**************************************************************************//**
* @brief    Reads a byte from the debug interface. Requires DD to be
*           input when function is called.
*
* @return   Returns the byte read.
******************************************************************************/
#pragma inline
unsigned char read_debug_byte(void)
{
  unsigned char i;
  unsigned char data;
  for (i = 0; i < 8; i++)
  {
    mraa_gpio_write(gpio_dc, 1);
    data <<= 1;
    data |= mraa_gpio_read(gpio_dd);  // Read DD line
    mraa_gpio_write(gpio_dc, 0);
  }
  return data;
}


/**************************************************************************//**
* @brief    Function waits for DUP to indicate that it is ready. The DUP will
*           pulls DD line low when it is ready. Requires DD to be input when
*           function is called.
*
* @return   Returns 0 if function timed out waiting for DD line to go low
* @return   Returns 1 when DUP has indicated it is ready.
******************************************************************************/
#pragma inline
unsigned char wait_dup_ready(void)
{
  // DUP pulls DD low when ready
  int count = 16;
  while (mraa_gpio_read(gpio_dd) && count)
  {
    // Clock out 8 bits before checking if DD is low again
    read_debug_byte();
    count--;
  }
  return (count == 0) ? 0 : 1;
}


/**************************************************************************//**
* @brief    Issues a command on the debug interface. Only commands that return
*           one output byte are supported.
*
* @param    cmd             Command byte
* @param    cmd_bytes       Pointer to the array of data bytes following the
*                           command byte [0-3]
* @param    num_cmd_bytes   The number of data bytes (input to DUP) [0-3]
*
* @return   1 if success, 0 if device did not respond
******************************************************************************/
unsigned char debug_command(unsigned char cmd, unsigned char *cmd_bytes, unsigned short num_cmd_bytes, unsigned char *output)
{
  unsigned short i;
  unsigned char ok = 1;
  unsigned char expect_response;
  unsigned char response;

  expect_response = output == NULL ? 0 : 1;

  // Make sure DD is output
  SET_DD_OUTPUT();

  // Send command
  if (expect_response) {
    cmd = cmd | 0b100;
  }
  printf("Writing debug command 0x%02x\n", cmd);
  write_debug_byte(cmd);

  // Send bytes
  for (i = 0; i < num_cmd_bytes; i++)
  {
    write_debug_byte(cmd_bytes[i]);
  }

  if (expect_response) {

    // Set DD as input
    SET_DD_INPUT();

    // Wait for data to be ready
    ok = wait_dup_ready();

    if (ok) {
      // Read returned byte
      response = read_debug_byte();
      if (output != NULL) {
        *output = response;
      }
    }

    // Set DD as output
    SET_DD_OUTPUT();
  }

  return ok;
}


/**************************************************************************//**
* @brief    Resets the DUP into debug mode. Function assumes that
*           the programmer I/O has already been configured using e.g.
*           programmer_init().
*
* @return   None.
******************************************************************************/
void debug_init(void)
{
  int i;


  // Send two flanks on DC while keeping RESET_N low
  mraa_gpio_write(gpio_dc, 0);
  mraa_gpio_write(gpio_dd, 0);
  mraa_gpio_write(gpio_rst, 0);

  for (i = 0; i < 150000; i++);   // Wait

  mraa_gpio_write(gpio_dc, 1);
  mraa_gpio_write(gpio_dc, 0);
  mraa_gpio_write(gpio_dc, 1);
  mraa_gpio_write(gpio_dc, 0);

  for (i = 0; i < 150000; i++);   // Wait

  mraa_gpio_write(gpio_rst, 1); // RESET_N high

  for (i = 0; i < 150000; i++);   // Wait
  for (i = 0; i < 150000; i++);   // Wait
}


/**************************************************************************//**
* @brief    Reads the chip ID over the debug interface using the
*           GET_CHIP_ID command.
*
* @return   Returns 0 if read failed, 1 if succeeded
******************************************************************************/
unsigned char read_chip_id(unsigned char *chip_id, unsigned char *revision)
{
  unsigned char ok;

  // Make sure DD is output
  SET_DD_OUTPUT();

  // Send command
  write_debug_byte(CMD_GET_CHIP_ID);

  // Set DD as input
  SET_DD_INPUT();

  // Wait for data to be ready
  ok = wait_dup_ready();
  if (!ok) {
    return ok;
  }

  // Read ID and revision
  *chip_id = read_debug_byte(); // ID
  *revision = read_debug_byte();      // Revision

  // Set DD as output
  SET_DD_OUTPUT();

  return ok;
}

/**************************************************************************//**
* @brief    Sends a block of data over the debug interface using the
*           BURST_WRITE command.
*
* @param    src         Pointer to the array of input bytes
* @param    num_bytes   The number of input bytes
*
* @return   1 on success, 0 on failure.
******************************************************************************/
unsigned char burst_write_block(unsigned char *src, unsigned short num_bytes)
{
  unsigned int i;
  unsigned char ok;
  unsigned char cmd;

  // Make sure DD is output
  SET_DD_OUTPUT();

  printf("Burst writing %d bytes\n", num_bytes);

  if (num_bytes > 1023) {
    printf("Unable to send more than 1023 bytes at a time through burst_write_block!\n");
    return 0;
  }

  cmd = CMD_BURST_WRITE | HIBYTE(num_bytes) | 0b100;
  printf("Writing cmd: 0x%02x\n", cmd);
  write_debug_byte(cmd);
  write_debug_byte(LOBYTE(num_bytes));

  for (i = 0; i < 150000; i++);   // Wait

  for (i = 0; i < num_bytes; i++)
  {
    write_debug_byte(src[i]);
  }

  // Set DD as input
  SET_DD_INPUT();

  // Wait for DUP to be ready
  ok = wait_dup_ready();

  if (ok) {
    read_debug_byte(); // ignore output
  }

  // Set DD as output
  SET_DD_OUTPUT();

  return ok;
}


/**************************************************************************//**
* @brief    Issues a CHIP_ERASE command on the debug interface and waits for it
*           to complete.
*
* @return   0 if erase failed, 1 if succeeded
******************************************************************************/
unsigned char chip_erase(void)
{
  unsigned char status;
  unsigned char ok;
  // Send command
  ok = debug_command(CMD_CHIP_ERASE, 0, 0, &status);
  if (!ok) {
    return ok;
  }

  // Wait for status bit 7 to go low
  do {
    ok = debug_command(CMD_READ_STATUS, 0, 0, &status);
    if (!ok) {
      printf("Failed to read status.\n");
      return ok;
    }
    printf("after erase, status = 0x%02x.\n", status);
  } while(!(status & STATUS_CHIP_ERASE_BUSY_BM));
  return 1;
}


/**************************************************************************//**
* @brief    Writes a block of data to the DUP's XDATA space.
*
* @param    address     XDATA start address
* @param    values      Pointer to the array of bytes to write
* @param    num_bytes   Number of bytes to write
*
* @return   1 if success, 0 on failure.
******************************************************************************/
unsigned char write_xdata_memory_block(unsigned short address, const unsigned char *values, unsigned short num_bytes)
{
  unsigned char instr[3];
  unsigned short i;
  unsigned char ok;
  unsigned char res;

  // MOV DPTR, address
  instr[0] = 0x90;
  instr[1] = HIBYTE(address);
  instr[2] = LOBYTE(address);
  ok = debug_command(CMD_DEBUG_INSTR_3B, instr, 3, &res);
  if (!ok) { return ok; }

  for (i = 0; i < num_bytes; i++)
  {
    // MOV A, values[i]
    instr[0] = 0x74;
    instr[1] = values[i];
    ok = debug_command(CMD_DEBUG_INSTR_2B, instr, 2, &res);
    if (!ok) { return ok; }

    // MOV @DPTR, A
    instr[0] = 0xF0;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &res);
    if (!ok) { return ok; }

    // INC DPTR
    instr[0] = 0xA3;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &res);
    if (!ok) { return ok; }
  }
}


/**************************************************************************//**
* @brief    Writes a byte to a specific address in the DUP's XDATA space.
*
* @param    address     XDATA address
* @param    value       Value to write
*
* @return   None.
******************************************************************************/
unsigned char write_xdata_memory(unsigned short address, unsigned char value)
{
  unsigned char instr[3];
  unsigned char res;
  unsigned char ok;

  // MOV DPTR, address
  instr[0] = 0x90;
  instr[1] = HIBYTE(address);
  instr[2] = LOBYTE(address);
  ok = debug_command(CMD_DEBUG_INSTR_3B, instr, 3, &res);
  if (!ok) { return ok; }

  // MOV A, values[i]
  instr[0] = 0x74;
  instr[1] = value;
  ok = debug_command(CMD_DEBUG_INSTR_2B, instr, 2, &res);
  if (!ok) { return ok; }

  // MOV @DPTR, A
  instr[0] = 0xF0;
  ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &res);
  if (!ok) { return ok; }
}


/**************************************************************************//**
* @brief    Read a byte from a specific address in the DUP's XDATA space.
*
* @param    address     XDATA address
*
* @return   Value read from XDATA
******************************************************************************/
unsigned char read_xdata_memory(unsigned short address, unsigned char *resp)
{
  unsigned char instr[3];
  unsigned char ok;

  // MOV DPTR, address
  instr[0] = 0x90;
  instr[1] = HIBYTE(address);
  instr[2] = LOBYTE(address);
  *resp = 0;

  ok = debug_command(CMD_DEBUG_INSTR_3B, instr, 3, resp);
  if (!ok) {
    return ok;
  }


  // MOVX A, @DPTR
  instr[0] = 0xE0;

  ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, resp);

  return ok;
}


/**************************************************************************//**
* @brief    Reads 1-32767 bytes from DUP's flash to a given buffer on the
*           programmer.
*
* @param    bank        Flash bank to read from [0-7]
* @param    address     Flash memory start address [0x0000 - 0x7FFF]
* @param    values      Pointer to destination buffer.
*
* @return   1 if success, 0 if failure
******************************************************************************/
unsigned char read_flash_memory_block(unsigned char bank,unsigned short flash_addr, unsigned short num_bytes, unsigned char *values)
{
  unsigned char instr[3];
  unsigned short i;
  unsigned short xdata_addr = (0x8000 + flash_addr);
  unsigned char ok;

  // 1. Map flash memory bank to XDATA address 0x8000-0xFFFF
  ok = write_xdata_memory(DUP_MEMCTR, bank);
  if (!ok) { return ok; }

  // 2. Move data pointer to XDATA address (MOV DPTR, xdata_addr)
  instr[0] = 0x90;
  instr[1] = HIBYTE(xdata_addr);
  instr[2] = LOBYTE(xdata_addr);
  ok = debug_command(CMD_DEBUG_INSTR_3B, instr, 3, NULL);
  if (!ok) { return ok; }

  for (i = 0; i < num_bytes; i++)
  {
    // 3. Move value pointed to by DPTR to accumulator (MOVX A, @DPTR)
    instr[0] = 0xE0;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &values[i]);
    if (!ok) { return ok; }

    // 4. Increment data pointer (INC DPTR)
    instr[0] = 0xA3;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, NULL);
    if (!ok) { return ok; }
  }
}


/**************************************************************************//**
* @brief    Writes 4-2048 bytes to DUP's flash memory. Parameter \c num_bytes
*           must be a multiple of 4.
*
* @param    src         Pointer to programmer's source buffer (in XDATA space)
* @param    start_addr  FLASH memory start address [0x0000 - 0x7FFF]
* @param    num_bytes   Number of bytes to transfer [4-1024]
*
* @return   1 for success, 0 for failure.
******************************************************************************/
unsigned char write_flash_memory_block(unsigned char *src, unsigned long start_addr, unsigned short num_bytes)
{
  unsigned char ok = 0;
  unsigned char fctl = 0;

  // 1. Write the 2 DMA descriptors to RAM
  printf("here1\n");
  ok = write_xdata_memory_block(ADDR_DMA_DESC_0, dma_desc_0, 8);
  if (!ok) { return ok; }
  printf("here2\n");
  ok = write_xdata_memory_block(ADDR_DMA_DESC_1, dma_desc_1, 8);
  if (!ok) { return ok; }

  // 2. Update LEN value in DUP's DMA descriptors
  unsigned char len[2] = {HIBYTE(num_bytes), LOBYTE(num_bytes)};
  printf("here3\n");
  ok = write_xdata_memory_block((ADDR_DMA_DESC_0+4), len, 2);  // LEN, DBG => ram
  if (!ok) { return ok; }
  printf("here4\n");
  ok = write_xdata_memory_block((ADDR_DMA_DESC_1+4), len, 2);  // LEN, ram => flash
  if (!ok) { return ok; }

  // 3. Set DMA controller pointer to the DMA descriptors
  printf("here5\n");
  ok = write_xdata_memory(DUP_DMA0CFGH, HIBYTE(ADDR_DMA_DESC_0));
  if (!ok) { return ok; }
  printf("here6\n");
  ok = write_xdata_memory(DUP_DMA0CFGL, LOBYTE(ADDR_DMA_DESC_0));
  if (!ok) { return ok; }
  printf("here7\n");
  ok = write_xdata_memory(DUP_DMA1CFGH, HIBYTE(ADDR_DMA_DESC_1));
  if (!ok) { return ok; }
  printf("here8\n");
  ok = write_xdata_memory(DUP_DMA1CFGL, LOBYTE(ADDR_DMA_DESC_1));
  if (!ok) { return ok; }

  // 4. Set Flash controller start address (wants 16MSb of 18 bit address)
  printf("here9\n");
  ok = write_xdata_memory(DUP_FADDRH, HIBYTE( (start_addr>>2) ));
  if (!ok) { return ok; }
  printf("here10\n");
  ok = write_xdata_memory(DUP_FADDRL, LOBYTE( (start_addr>>2) ));
  if (!ok) { return ok; }

  // 5. Arm DBG=>buffer DMA channel and start burst write
  ok = write_xdata_memory(DUP_DMAARM, CH_DBG_TO_BUF0);
  printf("starting burst_write_block\n");
  if (!ok) { return ok; }
  ok = burst_write_block(src, num_bytes);
  if (ok) {
    printf("burst_write_block ok\n");
  } else {
    printf("burst_write_block failed\n");
    return ok;
  }

  // 6. Start programming: buffer to flash
  ok = write_xdata_memory(DUP_DMAARM, CH_BUF0_TO_FLASH);
  if (!ok) { return ok; }
  ok = write_xdata_memory(DUP_FCTL, 0x06);
  if (!ok) { return ok; }

  // 7. Wait until flash controller is done
  while(1) {
    ok = read_xdata_memory(0xDFAE, &fctl);
    if (!ok) {
      printf("read FCTL failed.\n");
      return 0;
    }
    printf("dfae = 0x%02x\n", fctl);
    if (!(fctl & 0x80)) {
      break;
    }
  }
  return 1;
}


void close_gpios()
{
  mraa_gpio_close(gpio_rst);
  mraa_gpio_close(gpio_dc);
  mraa_gpio_close(gpio_dd);
}

#define TOTAL_FLASH_SIZE 0x7fff
#define FLASH_BLOCK_SIZE 0x3ff

void dump_flash()
{
  unsigned char flash_buf[TOTAL_FLASH_SIZE];
  FILE *fp;
  read_flash_memory_block(0, 0x0000, TOTAL_FLASH_SIZE, flash_buf);
  fp = fopen("flash.out", "w+");
  fwrite(flash_buf, 1, TOTAL_FLASH_SIZE, fp);
  fclose(fp);
}

void write_flash()
{
  unsigned char flash_buf[TOTAL_FLASH_SIZE];
  unsigned char ok;
  unsigned int len_to_write = 0;
  unsigned int offset = 0;
  unsigned int i;
  unsigned int swap;
  FILE *fp;
  fp = fopen("firmware.dat", "r");
  if (fp != NULL) {
    fread(flash_buf, 1, TOTAL_FLASH_SIZE, fp);
    fclose(fp);

    // Need to swap bytes for flash controller
    //for(i=0; i<(TOTAL_FLASH_SIZE/2); i++) {
    //  swap = flash_buf[i*2];
    //  flash_buf[i*2] = flash_buf[i*2+1];
    //  flash_buf[i*2+1] = swap;
    //}

    printf("Writing flash\n");
    while(offset < TOTAL_FLASH_SIZE) {
      len_to_write = TOTAL_FLASH_SIZE - offset;
      if (len_to_write > FLASH_BLOCK_SIZE) {
        len_to_write = FLASH_BLOCK_SIZE;
      }
      len_to_write = len_to_write - (len_to_write % 2); // Smallest write is 16 bits
      ok = write_flash_memory_block(flash_buf, offset, len_to_write);
      if (ok) {
        offset += len_to_write;
      } else {
        printf("Writing flash failed.\n");
        break;
      }
    }
  } else {
    printf("Unable to open file \"firmware.dat\" for reading.\n");
  }
}

/**************************************************************************//**
* @brief    Main function.
*
* @return   None.
******************************************************************************/
int main(int argc, char **argv)
{

  // CC-DEBUGGER protocol
  // http://www.ti.com/lit/ug/swra124/swra124.pdf

  unsigned char chip_id = 0;
  unsigned char revision = 0;
  unsigned char debug_config = 0;
  unsigned char instr[3];
  int num_tries = 0;
  unsigned char status;
  unsigned char ok;
  unsigned char reg;
  int i;

  // Init mraa gpio structs
  gpio_rst = mraa_gpio_init(RESET_N);
  gpio_dc = mraa_gpio_init(DC);
  gpio_dd = mraa_gpio_init(DD);

  // Initialize gpios as outputs
  mraa_gpio_dir(gpio_rst, MRAA_GPIO_OUT);
  mraa_gpio_dir(gpio_dc, MRAA_GPIO_OUT);
  mraa_gpio_dir(gpio_dd, MRAA_GPIO_OUT);

  // Init levels on gpios
  mraa_gpio_write(gpio_rst, 1);
  mraa_gpio_write(gpio_dc, 0);
  mraa_gpio_write(gpio_dd, 0);

  /****************************************
  * Initialize debug interface
  *****************************************/

  while (num_tries < 3) {
    // Put the DUP in debug mode
    debug_init();

    // Not sure why this is necessary, but the cc-debugger does it
    instr[0] = 0x00;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &reg);
    if (!ok) {
      printf("NOP failed!\n");
      break;
    }
    for (i = 0; i < 150000; i++);   // Wait

    ok = debug_command(CMD_READ_STATUS, 0, 0, &status);
    if (!ok) {
      printf("read status failed!\n");
      break;
    }
    printf("status = 0x%02x\n", status);
    for (i = 0; i < 150000; i++);   // Wait

    /****************************************
    * Read chip ID
    *****************************************/
    ok = read_chip_id(&chip_id, &revision);
    if (!ok) {
      printf("Read chip id failed.");
      continue;
    }
    printf("Chip id = 0x%x, revision = 0x%x\n", chip_id, revision);
    break;
  }

  for (i = 0; i < 150000; i++);   // Wait


  ok = read_xdata_memory(0xDFC6, &reg);
  if (ok) {
    printf("CLKCON = 0x%02x\n", reg);
  } else {
    printf("could not read CLKCON!\n");
  }

  // Write FWT for 24MHz clock (24MHz = 0x20)
  ok = write_xdata_memory(0xDFAB, 0x20);
  if (ok) {
    printf("Updated FWT\n");
  } else {
    printf("could not update FWT!\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  // Read FWT
  ok = read_xdata_memory(0xDFAB, &reg);
  if (ok) {
    printf("FWT = 0x%02x\n", reg);
  } else {
    printf("could not read FWT!\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  // Write Config
  instr[0] = 0x22;
  ok = debug_command(CMD_WR_CONFIG, instr, 1, &reg);
  if (ok) {
    printf("Debug Config = 0x%02x\n", reg);
  } else {
    printf("could not read Debug Config !\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  // Read Config
  ok = debug_command(CMD_RD_CONFIG, 0, 0, &reg);
  if (ok) {
    printf("Wrote debug config\n", reg);
  } else {
    printf("could not write debug config !\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  // Read P0DIR
  ok = read_xdata_memory(0xDFFD, &reg);
  if (ok) {
    printf("P0DIR = 0x%02x\n", reg);
  } else {
    printf("could not read P0DIR!\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  // Write P0DIR
  ok = write_xdata_memory(0xDFFD, 0b11);
  if (ok) {
    printf("Updated P0DIR\n");
  } else {
    printf("could not update P0DIR!\n");
  }
  for (i = 0; i < 150000; i++);   // Wait

  printf("Erasing chip.\n");
  if (chip_erase()) {
    printf("Chip erased.\n");
  } else {
    printf("Chip erase failed.\n");
  }

  // Read P0DIR
  ok = read_xdata_memory(0xDFFD, &reg);
  if (ok) {
    printf("P0DIR = 0x%02x\n", reg);
  } else {
    printf("could not read P0DIR!\n");
  }

  write_flash();
  dump_flash();

  close_gpios();

}
