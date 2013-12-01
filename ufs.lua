local function assert(cond, err, ...)
  collectgarbage("collect") -- force gc, to test for bugs
  if cond == nil then error(tostring(err)) end -- annoyingly, assert does not call tostring!
  if type(cond) == "function" then return cond, err, ... end
  if cond == true then return ... end
  return cond, ...
end

--[[
local S = require "syscall"

assert(S.abi.le, "This test requires little endian machine")

S.setenv("RUMP_VERBOSE", "1")
]]
local S = require "syscall.rump.init".init("vfs", "fs.ffs", "dev", "dev.disk")

local dev = "/de-vice"

assert(S.rump.etfs_register(dev, "rump/ufs.img", "blk"))

S.mkdir("/mnt", "0755")
assert(S.mount("ffs", "/mnt", "", dev))

S.unlink("/mnt/file")

local fd = assert(S.creat("/mnt/file", "rwxu"))

local len, block = 10 * 1024 * 1024, 8192
local blocks = len / block
local buf = S.t.buffer(block)

-- fill it with zeros
for i = 1, blocks do
  assert(fd:write(buf, block))
end

for i = 1, 10000 do
  local r = (math.random(blocks) - 1) * block
  fd:pread(buf, block, r)
  local r = (math.random(blocks) - 1) * block
  fd:pread(buf, block, r)
  local r = (math.random(blocks) - 1) * block
  fd:pwrite(buf, block, r)
end

assert(fd:close())

assert(S.unmount("/mnt"))

