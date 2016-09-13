#!/usr/bin/env ruby
require 'csv'

if ARGV.count < 1
  puts "Usage: #{$0} ccdebugfile.csv"
  exit 1
end

class CCDebugParser
  COMMANDS = {
    0x10 => "Chip Erase",
    0x18 => "Write Config",
    0x20 => "Read Config",
    0x30 => "Read Status",
    0x48 => "Resume",
    0x50 => "Debug Instruction",
    0x80 => "Burst Write",
    0x68 => "Get Chip ID",
  }

  def initialize
    default_handler = Proc.new do |cmd, args, response, data|
      puts "#{COMMANDS[cmd & 0b11111000]} 0x#{cmd.to_s(16)} #{args.inspect}, #{response}"
    end

    @cmd_handlers = {}

    COMMANDS.each { |code,name|
      @cmd_handlers[code] = default_handler
    }

    @cmd_handlers[0x50] = self.method(:parse_debug_instruction)
  end

  # SFR registers
  SFR = {
    0x82 => "DPL0",
    0x83 => "DPH0",
    0x92 => "DPS",
  }

  XDATA_REG = {
    0xDF36 => "PARTNUM",
  }


  def parse_debug_instruction(cmd, args, response, data)
    args_hex = args.map {|a| a.to_s(16)}
    resp_hex = response.to_s(16)
    print " - "
    case args[0]
    when 0x00 # NOP
      puts "NOP"
    when 0xe5 # MOV A,direct
      arg = SFR[args[1]] || args_hex[1]
      puts "MOV A, #{arg} -> #{resp_hex}"
    when 0x74 # MOV A,#data
      arg = SFR[args[1]] || args_hex[1]
      puts "MOV A,\##{arg} -> #{resp_hex}"
    when 0x75 # MOV direct,#data
      arg0 = SFR[args[1]] || args_hex[1]
      arg1 = SFR[args[2]] || args_hex[2]
      puts "MOV #{arg0}, \##{arg1} -> #{resp_hex}"
    when 0x90 # MOV DPTR,#data16
      addr = (args[1] << 8) + args[2]
      addr = XDATA_REG[addr] || addr.to_s(16)
      puts "MOV DPTR, #{addr} -> #{resp_hex}"
    when 0xe0 # MOVX A,@DPTR
      puts "MOVX A,@DPTR"
    when 0xa3
      puts "INC DPTR"
    when 0xF0
      puts "MOVX @DPTR,A"
    else
      puts "Undecoded Opcode #{args_hex.join(',')} -> #{resp_hex}"
    end

  end

  def parse(data)
    # Skip first row, which is header:
    data.next
    while cmd = data.next
      arg_count = cmd & 0b11
      args = arg_count.times.map { data.next }
      resp = data.next if cmd & 0b100
      handler = @cmd_handlers[cmd & 0b11111000]
      if handler
        handler.call(cmd, args, resp, data)
      else
        puts "No handler for command 0x#{cmd.to_s(16)}"
      end
    end
  end

end

# The third item in each csv row contains the byte sent across DD
begin
  parser = CCDebugParser.new
  parser.parse(CSV.foreach(ARGV[0]).lazy.map {|row| row[2].to_i(16)})
rescue StopIteration, Errno::EPIPE  # Catch end -of-input, and end-of-input
end
