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
  end

  def parse(data)
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
rescue StopIteration
end
