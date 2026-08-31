-- Frontend witness for the xemu package: run the core inside Chimera for a
-- fixed number of frames with nothing pressed, then dump the start of EE RAM.
-- The driver compares that dump byte-for-byte against the native reference
-- (run-native --dump-domain).
--
-- A slice rather than the whole domain, deliberately: System RAM is 64 megabytes,
-- and pulling that through Lua one byte at a time takes longer than the
-- emulation does. The first megabyte is where the kernel and the running
-- program live, it changes every frame, and it is where a divergence between
-- the frontend's machine and the reference one would show first. The domain's
-- full size travels in the metadata, so the driver still sees whether the
-- frontend built the machine it expected.
--
-- Job description comes from the file named by the MINIHAWK_JOB env var:
--   frames=<how many frames to advance>
--   out=<path to write the RAM dump (binary)>
--   meta=<path to write result metadata (text)>
--   shot=<optional path to write a screenshot>
--   bytes=<optional slice length, default 1MB>

local DOMAIN = "System RAM"

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
		"ramsize=" .. (meta.ramsize or 0),
		"ramhash=" .. (meta.ramhash or ""),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

local sysid = emu.getsystemid()
if sysid ~= "XBOX" then
	finish("ERROR", "wrong system id: " .. tostring(sysid))
end
if emu.getcorename() ~= "xemu" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

local frames = tonumber(job.frames) or 120
for _ = 1, frames do
	emu.frameadvance()
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()
pcall(function()
	memory.usememorydomain(DOMAIN)
	meta.ramsize = memory.getcurrentmemorydomainsize()
	meta.ramhash = memory.hash_region(0, meta.ramsize, DOMAIN)
end)

if job.shot ~= nil and job.shot ~= "" then
	client.screenshot(job.shot)
end

local slice = tonumber(job.bytes) or (1024 * 1024)
if slice > meta.ramsize then slice = meta.ramsize end
local ram = memory.read_bytes_as_array(0, slice, DOMAIN)
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
