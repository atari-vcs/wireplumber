-- WirePlumber
--
-- Copyright © 2020 Collabora Ltd.
--    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
--
-- Based on https://github.com/kikito/sandbox.lua
-- Copyright © 2013 Enrique García Cota
--
-- SPDX-License-Identifier: MIT

local SANDBOX_CONFIG = ...
local SANDBOX_ENV = {}

local function populate_env(id)
  local module, method = id:match('([^%.]+)%.([^%.]+)')
  if module then
    SANDBOX_ENV[module]         = SANDBOX_ENV[module] or {}
    SANDBOX_ENV[module][method] = _G[module][method]
  else
    SANDBOX_ENV[id] = _G[id]
  end
end

-- List of safe functions and packages
if SANDBOX_CONFIG["minimal_std"] then
  -- minimal list, used for config files
  ([[
    _VERSION ipairs pairs select tonumber tostring type

    table

    string.byte string.char  string.find  string.format string.gmatch
    string.gsub string.len   string.lower string.match  string.reverse
    string.sub  string.upper

  ]]):gsub('%S+', populate_env)
else
  -- full list, used for scripts
  ([[
    _VERSION assert error    ipairs   next pairs  print
    pcall    select tonumber tostring type xpcall

    table utf8

    math.abs   math.acos math.asin math.atan       math.ceil
    math.cos   math.deg  math.exp  math.tointeger  math.floor      math.fmod
    math.huge  math.ult  math.log  math.maxinteger math.mininteger math.max
    math.min   math.modf math.pi   math.rad        math.random
    math.sin   math.sqrt math.tan  math.type

    string.byte string.char  string.find  string.format string.gmatch
    string.gsub string.len   string.lower string.match  string.reverse
    string.sub  string.upper

    os.clock os.difftime os.time os.date os.getenv

  ]]):gsub('%S+', populate_env)
end

-- Additionally export everything in SANDBOX_EXPORT
if type(SANDBOX_EXPORT) == "table" then
  for k, v in pairs(SANDBOX_EXPORT) do
    SANDBOX_ENV[k] = v
  end
end

-- Additionally protect packages from malicious scripts trying to override methods
for k, v in pairs(SANDBOX_ENV) do
  if type(v) == "table" then
    SANDBOX_ENV[k] = setmetatable({}, {
      __index = v,
      __newindex = function(_, attr_name, _)
        error('Can not modify ' .. k .. '.' .. attr_name .. '. Protected by the sandbox.')
      end
    })
  end
end

if SANDBOX_CONFIG["isolate_env"] then
  -- in isolate_env mode, use a separate enviornment for each loaded chunk and
  -- store all of them in a global table so that they are not garbage collected
  SANDBOX_ENV_LIST = {}

  function sandbox(chunk, ...)
    -- chunk's environment will be an empty table with __index
    -- to access our SANDBOX_ENV (without being able to write it)
    local env = setmetatable({}, {
      __index = SANDBOX_ENV,
    })
    -- store the chunk's environment so that it is not garbage collected
    table.insert(SANDBOX_ENV_LIST, env)
    -- set it as the chunk's 1st upvalue (__ENV)
    debug.setupvalue(chunk, 1, env)
    -- execute the chunk
    chunk(...)
  end
else
  -- in common_env mode, use the same environment for all loaded chunks
  -- chunk's environment will be an empty table with __index
  -- to access our SANDBOX_ENV (without being able to write it)
  SANDBOX_COMMON_ENV = setmetatable({}, {
    __index = SANDBOX_ENV,
  })

  function sandbox(chunk, ...)
    -- set it as the chunk's 1st upvalue (__ENV)
    debug.setupvalue(chunk, 1, SANDBOX_COMMON_ENV)
    -- execute the chunk
    chunk(...)
  end
end
