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

// DMA addresses
#define ADDR_BUF0                 0xf000 // Buffer (512 bytes)j
#define FLASH_BUF_LEN             0x0200 // Buffer length (512)
#define ADDR_DMA_DESC             0xff00 // DMA descriptor (8 bytes)

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
#define DUP_FADDRL                  0xDFAC  // Flash controller addr
#define DUP_FADDRH                  0xDFAD  // Flash controller addr
#define DUP_FCTL                    0xDFAE  // Flash controller
#define DUP_FWDATA                  0xDFAF  // Flash Write Data
#define DUP_DMA0CFGL                0xDFD4  // Low byte, DMA config ch. 0
#define DUP_DMA0CFGH                0xDFD5  // Low byte, DMA config ch. 0
#define DUP_DMAARM                  0xDFD6  // DMA arming register

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
const unsigned char dma_desc[8] =
{
  // Buffer -> Flash controller
  HIBYTE(ADDR_BUF0),              // src[15:8]
  LOBYTE(ADDR_BUF0),              // src[7:0]
  HIBYTE(DUP_FWDATA),             // dest[15:8]
  LOBYTE(DUP_FWDATA),             // dest[7:0]
  HIBYTE(FLASH_BUF_LEN),          // len[12:8] - filled in later
  LOBYTE(FLASH_BUF_LEN),          // len[7:0]
  0x12,                           // trigger: FLASH
  0x42,                           // increment source
};

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
  //printf("Writing debug command 0x%02x\n", cmd);
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

  instr[0] = 0;
  ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &res);
  if (!ok) {
    printf("NOP failed!\n");
    return ok;
  }

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

  instr[0] = 0;
  ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &res);
  if (!ok) {
    printf("NOP failed!\n");
    return ok;
  }

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
* @param    address     Flash memory start address [0x0000 - 0x7FFF]
* @param    values      Pointer to destination buffer.
*
* @return   1 if success, 0 if failure
******************************************************************************/
unsigned char read_flash_memory_block(unsigned short flash_addr, unsigned short num_bytes, unsigned char *values)
{
  unsigned char instr[3];
  unsigned short i;
  unsigned char ok;
  unsigned char resp;

  instr[0] = 0;
  ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &resp);
  if (!ok) {
    printf("NOP failed!\n");
    return ok;
  }

  // 2. Move data pointer to XDATA address (MOV DPTR, flash_addr)
  instr[0] = 0x90;
  instr[1] = HIBYTE(flash_addr);
  instr[2] = LOBYTE(flash_addr);
  ok = debug_command(CMD_DEBUG_INSTR_3B, instr, 3, &resp);
  if (!ok) { return ok; }

  for (i = 0; i < num_bytes; i++)
  {
    // 3. Move value pointed to by DPTR to accumulator (MOVX A, @DPTR)
    instr[0] = 0xE0;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &values[i]);
    if (!ok) { return ok; }

    // 4. Increment data pointer (INC DPTR)
    instr[0] = 0xA3;
    ok = debug_command(CMD_DEBUG_INSTR_1B, instr, 1, &resp);
    if (!ok) { return ok; }
  }
}

/**************************************************************************//**
* @brief    Writes 1024 bytes to DUP's flash memory. Parameter \c num_bytes
*           must be a multiple of 4.
*
* @param    src         Pointer to programmer's source buffer (in XDATA space)
* @param    start_addr  FLASH memory start address [0x0000 - 0x7FFF]
*
* @return   1 for success, 0 for failure.
******************************************************************************/
unsigned char write_flash_memory_block(unsigned char *src, unsigned long start_addr)
{
  unsigned char ok = 0;
  unsigned char fctl = 0;


  // 1. Write the DMA descriptor to RAM
  ok = write_xdata_memory_block(ADDR_DMA_DESC, dma_desc, 8);
  if (!ok) { return ok; }

  // 2. Set DMA controller pointer to the DMA descriptors
  ok = write_xdata_memory(DUP_DMA0CFGH, HIBYTE(ADDR_DMA_DESC));
  if (!ok) { return ok; }
  ok = write_xdata_memory(DUP_DMA0CFGL, LOBYTE(ADDR_DMA_DESC));
  if (!ok) { return ok; }

  // 3. Set Flash controller start address (wants 16MSb of 18 bit address)
  ok = write_xdata_memory(DUP_FADDRH, HIBYTE( (start_addr>>2) ));
  if (!ok) { return ok; }
  ok = write_xdata_memory(DUP_FADDRL, LOBYTE( (start_addr>>2) ));
  if (!ok) { return ok; }

  // 4. Write data to buffer
  ok = write_xdata_memory_block(ADDR_BUF0, src, 512);

  // 5. Arm buffer -> flash DMA channel (channel 0)
  ok = write_xdata_memory(DUP_DMAARM, 0x01);
  if (!ok) { return ok; }
  ok = write_xdata_memory(DUP_FCTL, 0x02); // Trigger write
  if (!ok) { return ok; }
  // Wait for write to finish
  while (1) {
    ok = read_xdata_memory(DUP_FCTL, &fctl);
    if (!ok) { return ok; }
    if (fctl == 0x00) {
      break;
    }
  }

  // 4. Write second half of data to buffer
  ok = write_xdata_memory_block(ADDR_BUF0, src + 512, 512);

  // 5. Arm buffer -> flash DMA channel (channel 0)
  ok = write_xdata_memory(DUP_DMAARM, 0x01);
  if (!ok) { return ok; }
  ok = write_xdata_memory(DUP_FCTL, 0x02); // Trigger write
  if (!ok) { return ok; }
  // Wait for write to finish
  while (1) {
    ok = read_xdata_memory(DUP_FCTL, &fctl);
    if (!ok) { return ok; }
    if (fctl == 0x00) {
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

#define TOTAL_FLASH_SIZE 0x830
#define FLASH_BLOCK_SIZE 0x400

void dump_flash()
{
  unsigned char ok;
  unsigned char flash_buf[TOTAL_FLASH_SIZE];
  FILE *fp;
  printf("Reading flash.\n");
  ok = read_flash_memory_block(0x0000, TOTAL_FLASH_SIZE, flash_buf);
  if (!ok) {
    printf("Read flash failed!\n");
    return;
  }
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

    printf("Writing flash.");
    fflush(stdout);
    while(offset < TOTAL_FLASH_SIZE) {
      ok = write_flash_memory_block(flash_buf, offset);
      if (ok) {
	printf(".");
	fflush(stdout);
        offset += FLASH_BLOCK_SIZE;
      } else {
        printf("Writing flash failed.");
        break;
      }
    }
    printf("\n");
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
